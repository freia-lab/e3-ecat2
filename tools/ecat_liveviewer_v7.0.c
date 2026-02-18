/****************************************************************************
 * ecat_liveviewer.c  —  Dynamic PDO reader (synchronous master SDO uploads)
 *
 * Works with IgH EtherCAT Master 1.6.x and slaves that reject config-SDO.
 * Sequence:
 *   1) Request master, create domain, activate master
 *   2) Wait until the slave is PREOP (FSM pumping)
 *   3) Use ecrt_master_sdo_upload() to read:
 *      - 0x1C12/0x1C13 (assign)  → U8 count + U16 PDO indices
 *      - 0x1600…/0x1A00… (map)   → U8 count + U32 entries
 *   4) Build ec_pdo_info_t / ec_sync_info_t and configure PDOs
 *   5) Register PDO entries, run plain-stdout SM3 viewer
 *
 * Notes:
 *  - Vendor=0x0000006c, Product=0x0000a72c, position=0
 *  - JSON lists SM3 fields only (name, offset, type)
 ****************************************************************************/

#include <ecrt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <jansson.h>

/* ----------------------------- JSON SM3 fields ---------------------------- */

typedef struct {
    char name[128];
    int  offset;           /* byte offset within SM3 (TX PDO image) */
    int  type;             /* 1=u8, 2=u16, 4=u32 */
} field_t;

static int load_fields(const char *path, field_t **out, int *nf)
{
    json_error_t err;
    json_t *root = json_load_file(path, 0, &err);
    if (!root) {
        fprintf(stderr, "JSON error: %s (line %d)\n", err.text, err.line);
        return -1;
    }
    json_t *fields = json_object_get(root, "fields");
    json_t *s0     = json_object_get(fields, "slave0");
    json_t *sm3    = json_object_get(s0, "sm3");

    int n = json_array_size(sm3);
    field_t *arr = calloc(n, sizeof(field_t));
    if (!arr) { json_decref(root); return -1; }

    for (int i=0; i<n; i++) {
        json_t *f = json_array_get(sm3, i);
        const char *nm = json_string_value(json_object_get(f,"name"));
        const char *ty = json_string_value(json_object_get(f,"type"));
        strncpy(arr[i].name, nm?nm:"?", 127);
        arr[i].offset = (int) json_integer_value(json_object_get(f,"offset"));
        if (!ty || !strcasecmp(ty,"u8"))      arr[i].type = 1;
        else if (!strcasecmp(ty,"u16"))       arr[i].type = 2;
        else                                   arr[i].type = 4;
    }

    json_decref(root);
    *out = arr; *nf = n;
    return 0;
}

/* ----------------------------- PDO structures ---------------------------- */

typedef struct {
    uint16_t idx;
    uint8_t  sub;
    uint8_t  bits;
} pdo_entry_t;

typedef struct {
    uint16_t pdo_index;
    int      entry_count;
    pdo_entry_t *entries;
} pdo_t;

typedef struct {
    int count;
    pdo_t *list;
} pdo_list_t;

/* ------------------- Helpers: master-synchronous SDO upload --------------- */
/* ==== BEGIN: Robust master SDO upload helpers (debug & compatibility) ==== */

#ifndef ECRT_VERSION
#  define ECRT_VERSION(a,b) (((a) << 8) + (b))
#endif
#ifndef ECRT_VER_MAJOR
#  define ECRT_VER_MAJOR 1
#endif
#ifndef ECRT_VER_MINOR
#  define ECRT_VER_MINOR 6
#endif

/* Select the right ecrt_master_sdo_upload() signature (7 or 8 args) */
#if (ECRT_VER_MAJOR > 1) || (ECRT_VER_MAJOR == 1 && ECRT_VER_MINOR >= 6)
#  define MASTER_SDO_UPLOAD(master,pos,idx,sub,buf,psz,pabort) \
     ecrt_master_sdo_upload((master),(pos),(idx),(sub),(buf),(psz),(pabort), 2000 /*ms*/)
#else
#  define MASTER_SDO_UPLOAD(master,pos,idx,sub,buf,psz,pabort) \
     ecrt_master_sdo_upload((master),(pos),(idx),(sub),(buf),(psz),(pabort))
#endif

/* Common wrapper that logs the call, result and sizes when DEBUG_SDO is defined */
static int master_sdo_read(ec_master_t *master, unsigned pos,
                           uint16_t idx, uint8_t sub,
                           uint8_t *buf, size_t *inout_sz,
                           uint32_t *abort_code)
{
#ifdef DEBUG_SDO
    fprintf(stderr, "[SDO] pos=%u idx=0x%04x sub=0x%02x req_sz=%zu\n",
            pos, idx, sub, *inout_sz);
#endif
    int rc = MASTER_SDO_UPLOAD(master, pos, idx, sub, buf, inout_sz, abort_code);

#ifdef DEBUG_SDO
    if (rc)
        fprintf(stderr, "[SDO]  rc=%d (%s) abort=0x%08x got_sz=%zu\n",
                rc, strerror(-rc), *abort_code, *inout_sz);
    else
        fprintf(stderr, "[SDO]  rc=0 OK abort=0x%08x got_sz=%zu\n",
                *abort_code, *inout_sz);
#endif
    return rc;
}

/* Accept 1/2/4‑byte returns for “count” fields; validate range at the call site. */
static int sdo_u8_any(ec_master_t *master, unsigned pos,
                      uint16_t idx, uint8_t sub, uint8_t *v)
{
    uint8_t  tmp[4] = {0};
    size_t   sz = sizeof(tmp);
    uint32_t abort_code = 0;

    int rc = master_sdo_read(master, pos, idx, sub, tmp, &sz, &abort_code);
    if (rc) return rc;

    if (sz == 1) { *v = tmp[0]; return 0; }
    if (sz == 2) { *v = tmp[0]; return 0; } /* tolerate 2‑byte returns carrying 8‑bit value */
    if (sz == 4) { *v = tmp[0]; return 0; } /* tolerate 4‑byte returns */

    return -1; /* unexpected size */
}

/* Accept 2/4‑byte little‑endian; we only use the low 16 bits for assigns. */
static int sdo_u16_any(ec_master_t *master, unsigned pos,
                       uint16_t idx, uint8_t sub, uint16_t *v)
{
    uint8_t  tmp[8] = {0};
    size_t   sz = sizeof(tmp);
    uint32_t abort_code = 0;

    int rc = master_sdo_read(master, pos, idx, sub, tmp, &sz, &abort_code);
    if (rc) return rc;

    if (sz == 2 || sz == 4 || sz == 8) {
        *v = (uint16_t)(tmp[0] | (tmp[1] << 8));
        return 0;
    }
    return -1;
}

/* Require 4 bytes for mapping entries; anything else is an error. */
static int sdo_u32_exact(ec_master_t *master, unsigned pos,
                         uint16_t idx, uint8_t sub, uint32_t *v)
{
    uint8_t  tmp[4] = {0};
    size_t   sz = sizeof(tmp);
    uint32_t abort_code = 0;

    int rc = master_sdo_read(master, pos, idx, sub, tmp, &sz, &abort_code);
    if (rc) return rc;

    if (sz != 4) return -1;

    *v = (uint32_t)tmp[0] |
         ((uint32_t)tmp[1] << 8) |
         ((uint32_t)tmp[2] << 16) |
         ((uint32_t)tmp[3] << 24);
    return 0;
}

/* ---- read assign list with verbose error context ---- */
static int read_assign_list(ec_master_t *master, unsigned pos,
                            uint16_t assign_idx,
                            uint16_t *pdo_idxs, int *n_out)
{
    uint8_t cnt = 0;

    if (sdo_u8_any(master, pos, assign_idx, 0, &cnt)) {
        fprintf(stderr, "ERROR: failed to read 0x%04X:00 (PDO count)\n", assign_idx);
        return -1;
    }
    if (cnt == 0) {
        fprintf(stderr, "ERROR: 0x%04X:00 returned 0 count\n", assign_idx);
        return -1;
    }
    if (cnt > 16) cnt = 16;

#ifdef DEBUG_SDO
    fprintf(stderr, "[ASSIGN] 0x%04X:00 = %u entries\n", assign_idx, cnt);
#endif

    for (int i=0; i<cnt; i++) {
        uint16_t pdoi = 0;
        if (sdo_u16_any(master, pos, assign_idx, (uint8_t)(i+1), &pdoi)) {
            fprintf(stderr, "ERROR: failed to read 0x%04X:%d (PDO index)\n",
                    assign_idx, i+1);
            return -1;
        }
#ifdef DEBUG_SDO
        fprintf(stderr, "[ASSIGN] 0x%04X:%d = 0x%04X\n",
                assign_idx, i+1, pdoi);
#endif
        pdo_idxs[i] = pdoi;
    }

    *n_out = cnt;
    return 0;
}

/* ---- read mapping list with verbose error context ---- */
static int read_pdo_map(ec_master_t *master, unsigned pos,
                        uint16_t pdo_idx, pdo_t *out)
{
    uint8_t n = 0;
    if (sdo_u8_any(master, pos, pdo_idx, 0, &n)) {
        fprintf(stderr, "ERROR: failed to read 0x%04X:00 (entry count)\n", pdo_idx);
        return -1;
    }
    if (n == 0) {
        fprintf(stderr, "ERROR: 0x%04X has 0 entries\n", pdo_idx);
        return -1;
    }

#ifdef DEBUG_SDO
    fprintf(stderr, "[PDO] 0x%04X:00 = %u entries\n", pdo_idx, n);
#endif

    out->pdo_index   = pdo_idx;
    out->entry_count = n;
    out->entries     = calloc(n, sizeof(pdo_entry_t));
    if (!out->entries) {
        fprintf(stderr, "ERROR: calloc entries (%u) failed for PDO 0x%04X\n", n, pdo_idx);
        return -1;
    }

    for (int i=0; i<n; i++) {
        uint32_t map = 0;
        if (sdo_u32_exact(master, pos, pdo_idx, (uint8_t)(i+1), &map)) {
            fprintf(stderr, "ERROR: failed to read 0x%04X:%d (map entry)\n",
                    pdo_idx, i+1);
            return -1;
        }
        out->entries[i].idx  = (uint16_t)(map & 0xFFFF);
        out->entries[i].sub  = (uint8_t)((map >> 16) & 0xFF);
        out->entries[i].bits = (uint8_t)((map >> 24) & 0xFF);

#ifdef DEBUG_SDO
        fprintf(stderr, "[PDO] 0x%04X:%d = idx 0x%04X sub 0x%02X bits %u\n",
                pdo_idx, i+1, out->entries[i].idx,
                out->entries[i].sub, out->entries[i].bits);
#endif
    }
    return 0;
}

static int build_pdo_lists(ec_master_t *master, unsigned pos,
                           pdo_list_t *rx, pdo_list_t *tx)
{
    uint16_t rx_i[16], tx_i[16];
    int rx_n=0, tx_n=0;

    if (read_assign_list(master, pos, 0x1C12, rx_i, &rx_n)) return -1;
    if (read_assign_list(master, pos, 0x1C13, tx_i, &tx_n)) return -1;

#ifdef DEBUG_SDO
    fprintf(stderr, "[ASSIGN] rx_n=%d tx_n=%d\n", rx_n, tx_n);
#endif

    rx->count = rx_n; tx->count = tx_n;
    rx->list = calloc(rx_n, sizeof(pdo_t));
    tx->list = calloc(tx_n, sizeof(pdo_t));
    if (!rx->list || !tx->list) {
        fprintf(stderr, "ERROR: calloc PDO list failed\n");
        return -1;
    }

    for (int i=0; i<rx_n; i++)
        if (read_pdo_map(master, pos, rx_i[i], &rx->list[i])) return -1;
    for (int i=0; i<tx_n; i++)
        if (read_pdo_map(master, pos, tx_i[i], &tx->list[i])) return -1;

    return 0;
}
/* ==== END: Robust master SDO upload helpers ==== */
/* -------------------------- PDO assignment & mapping ---------------------- */


/* --------------------------- PDO→IgH config structures -------------------- */

static ec_pdo_info_t *make_infos(const pdo_list_t *rx, const pdo_list_t *tx)
{
    int total = rx->count + tx->count;
    ec_pdo_info_t *infos = calloc(total, sizeof(ec_pdo_info_t));
    if (!infos) return NULL;

    int k=0;

    for (int i=0; i<rx->count; i++,k++) {
        const pdo_t *p = &rx->list[i];
        ec_pdo_entry_info_t *arr = calloc(p->entry_count, sizeof(ec_pdo_entry_info_t));
        for (int j=0; j<p->entry_count; j++) {
            arr[j].index = p->entries[j].idx;
            arr[j].subindex = p->entries[j].sub;
            arr[j].bit_length = p->entries[j].bits;
        }
        infos[k] = (ec_pdo_info_t){ .index=p->pdo_index, .n_entries=p->entry_count, .entries=arr };
    }
    for (int i=0; i<tx->count; i++,k++) {
        const pdo_t *p = &tx->list[i];
        ec_pdo_entry_info_t *arr = calloc(p->entry_count, sizeof(ec_pdo_entry_info_t));
        for (int j=0; j<p->entry_count; j++) {
            arr[j].index = p->entries[j].idx;
            arr[j].subindex = p->entries[j].sub;
            arr[j].bit_length = p->entries[j].bits;
        }
        infos[k] = (ec_pdo_info_t){ .index=p->pdo_index, .n_entries=p->entry_count, .entries=arr };
    }
    return infos;
}

static ec_sync_info_t *make_syncs(const pdo_list_t *rx,
                                  const pdo_list_t *tx,
                                  const ec_pdo_info_t *infos)
{
    ec_sync_info_t *s = calloc(5, sizeof(ec_sync_info_t));
    s[0] = (ec_sync_info_t){ .index=0, .dir=EC_DIR_OUTPUT, .n_pdos=0 };
    s[1] = (ec_sync_info_t){ .index=1, .dir=EC_DIR_INPUT,  .n_pdos=0 };
    s[2] = (ec_sync_info_t){ .index=2, .dir=EC_DIR_OUTPUT, .n_pdos=rx->count, .pdos=infos };
    s[3] = (ec_sync_info_t){ .index=3, .dir=EC_DIR_INPUT,  .n_pdos=tx->count, .pdos=infos+rx->count };
    s[4] = (ec_sync_info_t){ .index=0xFF };
    return s;
}

static ec_pdo_entry_reg_t *
make_regs(const pdo_list_t *rx, const pdo_list_t *tx,
          unsigned **offs_out, int *n_total)
{
    int total=0;
    for (int i=0;i<rx->count;i++) total += rx->list[i].entry_count;
    for (int i=0;i<tx->count;i++) total += tx->list[i].entry_count;

    *n_total = total;
    unsigned *offs = calloc(total, sizeof(unsigned));
    *offs_out = offs;

    ec_pdo_entry_reg_t *regs = calloc(total+1, sizeof(ec_pdo_entry_reg_t));
    int k=0;
    for (int i=0;i<rx->count;i++){
        const pdo_t *p=&rx->list[i];
        for (int j=0;j<p->entry_count;j++,k++){
            regs[k].alias=0; regs[k].position=0;
            regs[k].vendor_id=0x0000006c; regs[k].product_code=0x0000a72c;
            regs[k].index=p->entries[j].idx; regs[k].subindex=p->entries[j].sub;
            regs[k].offset=&offs[k];
        }
    }
    for (int i=0;i<tx->count;i++){
        const pdo_t *p=&tx->list[i];
        for (int j=0;j<p->entry_count;j++,k++){
            regs[k].alias=0; regs[k].position=0;
            regs[k].vendor_id=0x0000006c; regs[k].product_code=0x0000a72c;
            regs[k].index=p->entries[j].idx; regs[k].subindex=p->entries[j].sub;
            regs[k].offset=&offs[k];
        }
    }
    regs[k] = (ec_pdo_entry_reg_t){0};
    return regs;
}

/* --------------------------------- MAIN ----------------------------------- */

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: sudo %s ecat_pdo_config.json\n", argv[0]);
        return 1;
    }

    /* Load JSON SM3 fields */
    field_t *fields; int nfields;
    if (load_fields(argv[1], &fields, &nfields)) return 1;

    /* Master + empty domain (required to activate) */
    ec_master_t *master = ecrt_request_master(0);
    if (!master) { fprintf(stderr,"request_master failed\n"); return 1; }

    ec_domain_t *domain = ecrt_master_create_domain(master);
    if (!domain) { fprintf(stderr,"create_domain failed\n"); return 1; }

    /* Minimal slave config (PDOs come later) */
    ec_slave_config_t *sc =
        ecrt_master_slave_config(master, 0, 0, 0x0000006c, 0x0000a72c);
    if (!sc) { fprintf(stderr,"slave_config failed\n"); return 1; }

    /* Activate master first so the kernel FSM runs and SDOs can be serviced */
    if (ecrt_master_activate(master)) {
        fprintf(stderr,"master_activate failed\n"); return 1;
    }

    /* Wait for PREOP (mailbox available) */
    printf("Waiting for PREOP...\n");
    for (;;) {
        ecrt_master_receive(master);
        ecrt_domain_process(domain);
        ecrt_domain_queue(domain);
        ecrt_master_send(master);
        usleep(5000);

        ec_slave_config_state_t st; ecrt_slave_config_state(sc,&st);
        if (st.al_state == EC_AL_STATE_PREOP) break;
    }
    printf("PREOP reached.\n");

    /* A little extra pump before the first SDO */
    for (int i=0;i<100;i++){
        ecrt_master_receive(master);
        ecrt_domain_process(domain);
        ecrt_domain_queue(domain);
        ecrt_master_send(master);
        usleep(1000);
    }

    /* --------- Read PDO map via synchronous master SDO uploads --------- */
    pdo_list_t rx={0}, tx={0};
    printf("Reading PDO assignment and mapping (master SDO uploads)...\n");
    if (build_pdo_lists(master, 0 /*position*/, &rx, &tx)) {
        fprintf(stderr, "PDO map build failed (master SDO).\n");
        return 1;
    }
    printf("PDO map read OK. RX=%d TX=%d\n", rx.count, tx.count);

    /* Build IgH config structures and apply */
    ec_pdo_info_t *infos = make_infos(&rx,&tx);
    ec_sync_info_t *syncs = make_syncs(&rx,&tx,infos);
    if (ecrt_slave_config_pdos(sc, EC_END, syncs)) {
        fprintf(stderr,"slave_config_pdos failed\n"); return 1;
    }

    /* Now register PDO entries to compute process-data offsets */
    unsigned *offs; int nentries;
    ec_pdo_entry_reg_t *regs = make_regs(&rx,&tx,&offs,&nentries);
    if (ecrt_domain_reg_pdo_entry_list(domain, regs)) {
        fprintf(stderr,"domain_reg failed\n"); return 1;
    }

    uint8_t *pd = ecrt_domain_data(domain);

    /* Compute SM3 base as the first TX entry (comes after all RX entries) */
    int rx_entries = 0;
    for (int i=0;i<rx.count;i++) rx_entries += rx.list[i].entry_count;
    int sm3_base = offs[rx_entries];

    /* ---------------------------- Main loop ---------------------------- */
    while (1) {
        ecrt_master_receive(master);
        ecrt_domain_process(domain);

        system("clear");
        printf("=== Live SM3 Viewer ===\n");
        for (int i=0;i<nfields;i++){
            int off = sm3_base + fields[i].offset;
            uint32_t v = 0;
            if (fields[i].type==1)      v = pd[off];
            else if (fields[i].type==2) v = pd[off] | (pd[off+1]<<8);
            else                        v = pd[off] | (pd[off+1]<<8) |
                                            (pd[off+2]<<16) | (pd[off+3]<<24);
            printf("%-28s : %u\n", fields[i].name, v);
        }

        ecrt_domain_queue(domain);
        ecrt_master_send(master);
        usleep(100000);
    }

    return 0;
}
