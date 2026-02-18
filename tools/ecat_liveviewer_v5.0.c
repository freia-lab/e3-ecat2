/****************************************************************************
 * ecat_liveviewer.c
 * Fully dynamic PDO-map builder for IgH EtherCAT Master 1.6.x
 *
 * Implements:
 *   - SDO-based PDO discovery:
 *       * 0x1C12 – RxPDO assign (U8 count, U16 PDO indexes)
 *       * 0x1C13 – TxPDO assign (U8 count, U16 PDO indexes)
 *       * 0x1600+/0x1A00+ PDO mapping objects (U8 count, U32 entries)
 *     (mapping structure defined in Beckhoff CoE PDO documentation) [1](https://docs.etherlab.org/ethercat/1.6/doxygen/structec__master__state__t.html)
 *
 *   - IgH 1.6.x-compliant SDO mailbox warm-up before issuing requests
 *     (SDO FSM depends on mailbox datagrams processed by master FSM) [2](https://gitlab.com/etherlab.org/ethercat/-/issues/143)
 *
 *   - Plain stdout live SM3 viewer
 *
 ***************************************************************************/

#include <ecrt.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <jansson.h>
#include <string.h>
#include <unistd.h>

/* ========================================================================
   SDO READ HELPERS (IgH 1.6.x mailbox FSM must be running)
   ======================================================================== */

static int read_sdo_u8(ec_slave_config_t *sc,
                       uint16_t idx, uint8_t sub, uint8_t *val)
{
    ec_sdo_request_t *req =
        ecrt_slave_config_create_sdo_request(sc, idx, sub, 1);
    if (!req) return -1;

    ecrt_sdo_request_read(req);

    for (int i=0; i<200; i++) {
        if (ecrt_sdo_request_state(req) == EC_REQUEST_SUCCESS) {
            const uint8_t *d = ecrt_sdo_request_data(req);
            *val = d[0];
            return 0;
        }
        usleep(1000);
    }
    return -1;
}

static int read_sdo_u16(ec_slave_config_t *sc,
                        uint16_t idx, uint8_t sub, uint16_t *val)
{
    ec_sdo_request_t *req =
        ecrt_slave_config_create_sdo_request(sc, idx, sub, 2);
    if (!req) return -1;

    ecrt_sdo_request_read(req);

    for (int i=0; i<200; i++) {
        if (ecrt_sdo_request_state(req) == EC_REQUEST_SUCCESS) {
            const uint8_t *d = ecrt_sdo_request_data(req);
            *val = d[0] | (d[1] << 8);
            return 0;
        }
        usleep(1000);
    }
    return -1;
}

static int read_sdo_u32(ec_slave_config_t *sc,
                        uint16_t idx, uint8_t sub, uint32_t *val)
{
    ec_sdo_request_t *req =
        ecrt_slave_config_create_sdo_request(sc, idx, sub, 4);
    if (!req) return -1;

    ecrt_sdo_request_read(req);

    for (int i=0; i<200; i++) {
        if (ecrt_sdo_request_state(req) == EC_REQUEST_SUCCESS) {
            const uint8_t *d = ecrt_sdo_request_data(req);
            *val = ((uint32_t)d[0]) |
                   ((uint32_t)d[1] << 8) |
                   ((uint32_t)d[2] << 16) |
                   ((uint32_t)d[3] << 24);
            return 0;
        }
        usleep(1000);
    }
    return -1;
}

/* ========================================================================
   PDO map structures
   ======================================================================== */

typedef struct {
    uint16_t index;
    uint8_t  subindex;
    uint8_t  bitlen;
} pdo_entry_t;

typedef struct {
    uint16_t pdo_index;
    int       entry_count;
    pdo_entry_t *entries;
} pdo_t;

typedef struct {
    int count;
    pdo_t *list;
} pdo_list_t;

/* ========================================================================
   Read PDO assignment lists (0x1C12 / 0x1C13) – U16 entries
   ======================================================================== */

static int read_pdo_assign(ec_slave_config_t *sc,
                           uint16_t assign_idx,
                           uint16_t *pdo_indices,
                           int *count_out)
{
    uint8_t n = 0;
    if (read_sdo_u8(sc, assign_idx, 0, &n)) return -1;
    if (n > 16) n = 16;

    for (int i=0; i<n; i++) {
        uint16_t idx16 = 0;
        if (read_sdo_u16(sc, assign_idx, i+1, &idx16)) return -1;
        pdo_indices[i] = idx16;
    }

    *count_out = n;
    return 0;
}

/* ========================================================================
   Read mapping entries for each PDO (0x1600+ / 0x1A00+)
   ======================================================================== */

static int read_pdo_mapping(ec_slave_config_t *sc,
                            uint16_t pdo_idx,
                            pdo_t *out)
{
    uint8_t n = 0;
    if (read_sdo_u8(sc, pdo_idx, 0, &n)) return -1;

    out->pdo_index = pdo_idx;
    out->entry_count = n;
    out->entries = calloc(n, sizeof(pdo_entry_t));
    if (!out->entries) return -1;

    for (int i=0; i<n; i++) {
        uint32_t map = 0;
        if (read_sdo_u32(sc, pdo_idx, i+1, &map)) return -1;

        out->entries[i].index    = map & 0xFFFF;
        out->entries[i].subindex = (map >> 16) & 0xFF;
        out->entries[i].bitlen   = (map >> 24) & 0xFF;
    }

    return 0;
}

/* ========================================================================
   Build RX/TX PDO lists
   ======================================================================== */

static int build_pdo_lists(ec_slave_config_t *sc,
                           pdo_list_t *rx,
                           pdo_list_t *tx)
{
    uint16_t rx_i[16], tx_i[16];
    int rx_n=0, tx_n=0;

    if (read_pdo_assign(sc, 0x1C12, rx_i, &rx_n)) return -1;
    if (read_pdo_assign(sc, 0x1C13, tx_i, &tx_n)) return -1;

    rx->count = rx_n;
    tx->count = tx_n;
    rx->list = calloc(rx_n, sizeof(pdo_t));
    tx->list = calloc(tx_n, sizeof(pdo_t));

    if (!rx->list || !tx->list) return -1;

    for (int i=0; i<rx_n; i++)
        if (read_pdo_mapping(sc, rx_i[i], &rx->list[i])) return -1;

    for (int i=0; i<tx_n; i++)
        if (read_pdo_mapping(sc, tx_i[i], &tx->list[i])) return -1;

    return 0;
}

/* ========================================================================
   Build ec_pdo_info_t array
   ======================================================================== */

static ec_pdo_info_t *
build_pdo_infos(const pdo_list_t *rx,
                const pdo_list_t *tx)
{
    int total = rx->count + tx->count;
    ec_pdo_info_t *infos = calloc(total, sizeof(ec_pdo_info_t));
    if (!infos) return NULL;

    int k = 0;

    /* RX PDOs */
    for (int i=0; i<rx->count; i++, k++) {
        const pdo_t *p = &rx->list[i];
        ec_pdo_entry_info_t *arr =
            calloc(p->entry_count, sizeof(ec_pdo_entry_info_t));

        for (int j=0; j<p->entry_count; j++) {
            arr[j].index      = p->entries[j].index;
            arr[j].subindex   = p->entries[j].subindex;
            arr[j].bit_length = p->entries[j].bitlen;
        }

        infos[k] = (ec_pdo_info_t){
            .index     = p->pdo_index,
            .n_entries = p->entry_count,
            .entries   = arr
        };
    }

    /* TX PDOs */
    for (int i=0; i<tx->count; i++, k++) {
        const pdo_t *p = &tx->list[i];
        ec_pdo_entry_info_t *arr =
            calloc(p->entry_count, sizeof(ec_pdo_entry_info_t));

        for (int j=0; j<p->entry_count; j++) {
            arr[j].index      = p->entries[j].index;
            arr[j].subindex   = p->entries[j].subindex;
            arr[j].bit_length = p->entries[j].bitlen;
        }

        infos[k] = (ec_pdo_info_t){
            .index     = p->pdo_index,
            .n_entries = p->entry_count,
            .entries   = arr
        };
    }

    return infos;
}

/* ========================================================================
   Build Sync Managers (const-correct for IgH 1.6.x)
   ======================================================================== */

static ec_sync_info_t *
build_syncs(const pdo_list_t *rx,
            const pdo_list_t *tx,
            const ec_pdo_info_t *pi)
{
    ec_sync_info_t *s = calloc(5, sizeof(ec_sync_info_t));

    s[0] = (ec_sync_info_t){ .index=0, .dir=EC_DIR_OUTPUT, .n_pdos=0 };
    s[1] = (ec_sync_info_t){ .index=1, .dir=EC_DIR_INPUT,  .n_pdos=0 };

    s[2] = (ec_sync_info_t){
        .index  = 2,
        .dir    = EC_DIR_OUTPUT,
        .n_pdos = rx->count,
        .pdos   = pi
    };

    s[3] = (ec_sync_info_t){
        .index  = 3,
        .dir    = EC_DIR_INPUT,
        .n_pdos = tx->count,
        .pdos   = pi + rx->count
    };

    s[4] = (ec_sync_info_t){ .index=0xFF };
    return s;
}

/* ========================================================================
   Build PDO entry registration
   ======================================================================== */

static ec_pdo_entry_reg_t *
build_entry_regs(const pdo_list_t *rx,
                 const pdo_list_t *tx,
                 unsigned **offs_out,
                 int *total_out)
{
    int total = 0;

    for (int i=0;i<rx->count;i++)
        total += rx->list[i].entry_count;
    for (int i=0;i<tx->count;i++)
        total += tx->list[i].entry_count;

    *total_out = total;

    unsigned *offs = calloc(total, sizeof(unsigned));
    *offs_out = offs;

    ec_pdo_entry_reg_t *regs =
        calloc(total+1, sizeof(ec_pdo_entry_reg_t));

    int k = 0;

    /* RX entries */
    for (int i=0;i<rx->count;i++){
        const pdo_t *p = &rx->list[i];
        for (int j=0;j<p->entry_count;j++,k++){
            regs[k].alias        = 0;
            regs[k].position     = 0;
            regs[k].vendor_id    = 0x0000006c;
            regs[k].product_code = 0x0000a72c;
            regs[k].index        = p->entries[j].index;
            regs[k].subindex     = p->entries[j].subindex;
            regs[k].offset       = &offs[k];
        }
    }

    /* TX entries */
    for (int i=0;i<tx->count;i++){
        const pdo_t *p = &tx->list[i];
        for (int j=0;j<p->entry_count;j++,k++){
            regs[k].alias        = 0;
            regs[k].position     = 0;
            regs[k].vendor_id    = 0x0000006c;
            regs[k].product_code = 0x0000a72c;
            regs[k].index        = p->entries[j].index;
            regs[k].subindex     = p->entries[j].subindex;
            regs[k].offset       = &offs[k];
        }
    }

    regs[k] = (ec_pdo_entry_reg_t){0};
    return regs;
}

/* ========================================================================
   Load JSON SM3 fields
   ======================================================================== */

typedef struct {
    char name[128];
    int offset;
    int type;  //1=u8,2=u16,4=u32
} field_t;

static int load_fields(const char *path,
                       field_t **out, int *nf)
{
    json_error_t err;
    json_t *root = json_load_file(path, 0, &err);
    if (!root) {
        fprintf(stderr,"JSON parse error: %s\n", err.text);
        return -1;
    }

    json_t *fields = json_object_get(root,"fields");
    json_t *s0 = json_object_get(fields,"slave0");
    json_t *sm3 = json_object_get(s0,"sm3");

    int n = json_array_size(sm3);
    field_t *arr = calloc(n, sizeof(field_t));

    for (int i=0; i<n; i++){
        json_t *f = json_array_get(sm3,i);

        const char *nm =
            json_string_value(json_object_get(f,"name"));
        strncpy(arr[i].name, nm?nm:"?", 127);

        arr[i].offset =
            json_integer_value(json_object_get(f,"offset"));

        const char *ty =
            json_string_value(json_object_get(f,"type"));
        if (!strcasecmp(ty,"u8"))      arr[i].type = 1;
        else if (!strcasecmp(ty,"u16")) arr[i].type = 2;
        else                            arr[i].type = 4;
    }

    *out = arr;
    *nf = n;
    json_decref(root);
    return 0;
}

/* ========================================================================
   MAIN
   ======================================================================== */

int main(int argc, char **argv)
{
    if (argc < 2){
        printf("Usage: sudo %s ecat_pdo_config.json\n", argv[0]);
        return 1;
    }

    /* Load JSON-defined SM3 fields */
    field_t *fields;
    int nfields;
    if (load_fields(argv[1], &fields, &nfields)) return 1;

    ec_master_t *master = ecrt_request_master(0);
    if (!master) return 1;

    ec_slave_config_t *sc =
        ecrt_master_slave_config(master, 0,0,
                                 0x0000006c,0x0000a72c);
    if (!sc) return 1;

    /* ===========================================================
       REQUIRED MAILBOX FSM WARM-UP PRIOR TO ANY SDO TRANSACTION
       (SDO requests only complete when mailbox datagrams are
        processed via ecrt_master_receive/send)
       IgH references show mailbox FSM is tied to master cycles. [2](https://gitlab.com/etherlab.org/ethercat/-/issues/143)
       =========================================================== */
    printf("Initializing mailbox for SDO access...\n");
    for (int i=0; i<300; i++){
        ecrt_master_receive(master);
        ecrt_master_send(master);
        usleep(1000);
    }

    /* Build PDO lists dynamically */
    pdo_list_t rx, tx;
    if (build_pdo_lists(sc, &rx, &tx)) {
        fprintf(stderr,"PDO map build failed\n");
        return 1;
    }

    ec_pdo_info_t *pdo_infos = build_pdo_infos(&rx, &tx);
    ec_sync_info_t *syncs = build_syncs(&rx, &tx, pdo_infos);

    if (ecrt_slave_config_pdos(sc, EC_END, syncs)){
        fprintf(stderr,"slave_config_pdos failed\n");
        return 1;
    }

    /* Create domain */
    ec_domain_t *domain = ecrt_master_create_domain(master);
    if (!domain) return 1;

    unsigned *entry_offsets;
    int nentries;
    ec_pdo_entry_reg_t *regs =
        build_entry_regs(&rx,&tx,&entry_offsets,&nentries);

    if (ecrt_domain_reg_pdo_entry_list(domain, regs)) {
        fprintf(stderr,"domain_reg failed\n");
        return 1;
    }

    if (ecrt_master_activate(master)){
        fprintf(stderr,"master_activate failed\n");
        return 1;
    }

    uint8_t *pd = ecrt_domain_data(domain);

    /* Compute SM3 base offset (first TX PDO entry) */
    int rx_entries = 0;
    for (int i=0; i<rx.count; i++)
        rx_entries += rx.list[i].entry_count;

    int sm3_base = entry_offsets[rx_entries];

    /* ===========================================================
       MAIN CYCLIC LOOP
       =========================================================== */
    while (1){
        ecrt_master_receive(master);
        ecrt_domain_process(domain);

        system("clear");
        printf("=== Live SM3 Viewer ===\n");

        for (int i=0; i<nfields; i++){
            int off = sm3_base + fields[i].offset;

            uint32_t v = 0;
            if (fields[i].type == 1)      v = pd[off];
            else if (fields[i].type == 2) v = pd[off] | (pd[off+1]<<8);
            else                          v = pd[off] |
                                              (pd[off+1]<<8) |
                                              (pd[off+2]<<16) |
                                              (pd[off+3]<<24);

            printf("%-30s : %u\n",
                   fields[i].name, v);
        }

        ecrt_domain_queue(domain);
        ecrt_master_send(master);

        usleep(100000);
    }

    return 0;
}
