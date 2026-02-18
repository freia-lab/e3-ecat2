/****************************************************************************
 * ecat_liveviewer.c
 * Dynamic PDO reader for IgH EtherCAT Master 1.6.x
 *
 * NO STATIC PDO TABLES.
 * Reads:
 *   - 0x1C12:00 → U8 PDO count     (RxPDO assign)
 *   - 0x1C12:n  → U16 PDO indices  (RxPDO assign)
 *   - 0x1C13:00 → U8 PDO count     (TxPDO assign)
 *   - 0x1C13:n  → U16 PDO indices  (TxPDO assign)
 *
 *   - 0x1600+/0x1A00+ mapping objects (U8 entry count, followed by U32 mappings)
 *
 * Includes:
 *   - PREOP transition wait-loop
 *   - Mailbox activation warm-up loop
 *   - Fully correct const-init for ec_sync_info_t and ec_pdo_info_t
 *   - SM3 viewer based on JSON
 ****************************************************************************/

#include <ecrt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <jansson.h>

/******************************************************************************
 *                             SDO HELPER FUNCTIONS
 *****************************************************************************/

static int read_sdo_u8(ec_slave_config_t *sc, uint16_t idx, uint8_t sub, uint8_t *out)
{
    ec_sdo_request_t *req = ecrt_slave_config_create_sdo_request(sc, idx, sub, 1);
    if (!req) return -1;

    ecrt_sdo_request_read(req);

    for (int i=0;i<200;i++) {
        if (ecrt_sdo_request_state(req) == EC_REQUEST_SUCCESS) {
            const uint8_t *d = ecrt_sdo_request_data(req);
            *out = d[0];
            return 0;
        }
        usleep(1000);
    }
    return -1;
}

static int read_sdo_u16(ec_slave_config_t *sc, uint16_t idx, uint8_t sub, uint16_t *out)
{
    ec_sdo_request_t *req = ecrt_slave_config_create_sdo_request(sc, idx, sub, 2);
    if (!req) return -1;

    ecrt_sdo_request_read(req);

    for (int i=0;i<200;i++){
        if (ecrt_sdo_request_state(req) == EC_REQUEST_SUCCESS) {
            const uint8_t *d = ecrt_sdo_request_data(req);
            *out = d[0] | (d[1]<<8);
            return 0;
        }
        usleep(1000);
    }
    return -1;
}

static int read_sdo_u32(ec_slave_config_t *sc, uint16_t idx, uint8_t sub, uint32_t *out)
{
    ec_sdo_request_t *req = ecrt_slave_config_create_sdo_request(sc, idx, sub, 4);
    if (!req) return -1;

    ecrt_sdo_request_read(req);

    for (int i=0;i<200;i++){
        if (ecrt_sdo_request_state(req) == EC_REQUEST_SUCCESS) {
            const uint8_t *d = ecrt_sdo_request_data(req);
            *out = ((uint32_t)d[0]) |
                   ((uint32_t)d[1]<<8) |
                   ((uint32_t)d[2]<<16) |
                   ((uint32_t)d[3]<<24);
            return 0;
        }
        usleep(1000);
    }
    return -1;
}

/******************************************************************************
 *                               PDO STRUCTURES
 *****************************************************************************/

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
    int     count;
    pdo_t  *list;
} pdo_list_t;

/******************************************************************************
 *                        PDO ASSIGN LIST (0x1C12/0x1C13)
 *****************************************************************************/

static int read_pdo_assign_list(ec_slave_config_t *sc,
                                uint16_t assign_idx,
                                uint16_t *pdo_idx_out,
                                int *n_out)
{
    uint8_t count = 0;

    /* Subindex 0 = U8 number of PDOs */
    if (read_sdo_u8(sc, assign_idx, 0, &count)) return -1;
    if (count > 16) count = 16;

    for (int i=0; i<count; i++) {
        /* Subindex i+1 = U16 PDO index */
        uint16_t pdo_i = 0;
        if (read_sdo_u16(sc, assign_idx, i+1, &pdo_i)) return -1;
        pdo_idx_out[i] = pdo_i;
    }

    *n_out = count;
    return 0;
}

/******************************************************************************
 *                           READ SINGLE PDO MAPPING
 *****************************************************************************/

static int read_pdo_mapping(ec_slave_config_t *sc, uint16_t pdo_idx, pdo_t *out)
{
    uint8_t n = 0;

    /* 1600:00 / 1A00:00 = number of mapped entries */
    if (read_sdo_u8(sc, pdo_idx, 0, &n)) return -1;

    out->pdo_index = pdo_idx;
    out->entry_count = n;
    out->entries = calloc(n, sizeof(pdo_entry_t));
    if (!out->entries) return -1;

    for (int i=0;i<n;i++) {
        uint32_t map = 0;
        if (read_sdo_u32(sc, pdo_idx, i+1, &map)) return -1;

        out->entries[i].idx  = map & 0xFFFF;
        out->entries[i].sub  = (map >>16) & 0xFF;
        out->entries[i].bits = (map >>24) & 0xFF;
    }

    return 0;
}

/******************************************************************************
 *                          BUILD COMPLETE PDO LISTS
 *****************************************************************************/

static int build_pdo_lists(ec_slave_config_t *sc,
                           pdo_list_t *rx, pdo_list_t *tx)
{
    uint16_t rx_i[16], tx_i[16];
    int rx_n=0, tx_n=0;

    if (read_pdo_assign_list(sc, 0x1C12, rx_i, &rx_n)) return -1;
    if (read_pdo_assign_list(sc, 0x1C13, tx_i, &tx_n)) return -1;

    rx->count = rx_n;
    tx->count = tx_n;
    rx->list = calloc(rx_n, sizeof(pdo_t));
    tx->list = calloc(tx_n, sizeof(pdo_t));

    if (!rx->list || !tx->list) return -1;

    for (int i=0;i<rx_n;i++)
        if (read_pdo_mapping(sc, rx_i[i], &rx->list[i])) return -1;

    for (int i=0;i<tx_n;i++)
        if (read_pdo_mapping(sc, tx_i[i], &tx->list[i])) return -1;

    return 0;
}

/******************************************************************************
 *                        BUILD ec_pdo_info_t ARRAY
 *****************************************************************************/

static ec_pdo_info_t *
build_info(const pdo_list_t *rx, const pdo_list_t *tx)
{
    int total = rx->count + tx->count;
    ec_pdo_info_t *infos = calloc(total, sizeof(ec_pdo_info_t));
    if (!infos) return NULL;

    int k=0;

    /* RX */
    for (int i=0;i<rx->count;i++,k++){
        const pdo_t *p = &rx->list[i];
        ec_pdo_entry_info_t *arr =
            calloc(p->entry_count, sizeof(ec_pdo_entry_info_t));

        for (int j=0;j<p->entry_count;j++){
            arr[j].index      = p->entries[j].idx;
            arr[j].subindex   = p->entries[j].sub;
            arr[j].bit_length = p->entries[j].bits;
        }

        infos[k] = (ec_pdo_info_t){
            .index     = p->pdo_index,
            .n_entries = p->entry_count,
            .entries   = arr
        };
    }

    /* TX */
    for (int i=0;i<tx->count;i++,k++){
        const pdo_t *p = &tx->list[i];
        ec_pdo_entry_info_t *arr =
            calloc(p->entry_count, sizeof(ec_pdo_entry_info_t));

        for (int j=0;j<p->entry_count;j++){
            arr[j].index      = p->entries[j].idx;
            arr[j].subindex   = p->entries[j].sub;
            arr[j].bit_length = p->entries[j].bits;
        }

        infos[k] = (ec_pdo_info_t){
            .index     = p->pdo_index,
            .n_entries = p->entry_count,
            .entries   = arr
        };
    }

    return infos;
}

/******************************************************************************
 *                         SYNC MANAGER CONFIGURATION
 *****************************************************************************/

static ec_sync_info_t *
build_syncs(const pdo_list_t *rx,
            const pdo_list_t *tx,
            const ec_pdo_info_t *infos)
{
    ec_sync_info_t *s = calloc(5, sizeof(ec_sync_info_t));

    s[0] = (ec_sync_info_t){ .index=0, .dir=EC_DIR_OUTPUT, .n_pdos=0 };
    s[1] = (ec_sync_info_t){ .index=1, .dir=EC_DIR_INPUT,  .n_pdos=0 };

    s[2] = (ec_sync_info_t){
        .index  = 2,
        .dir    = EC_DIR_OUTPUT,
        .n_pdos = rx->count,
        .pdos   = infos
    };

    s[3] = (ec_sync_info_t){
        .index  = 3,
        .dir    = EC_DIR_INPUT,
        .n_pdos = tx->count,
        .pdos   = infos + rx->count
    };

    s[4] = (ec_sync_info_t){ .index=0xFF };

    return s;
}

/******************************************************************************
 *                   PDO ENTRY REGISTRATION (OFFSETS)
 *****************************************************************************/

static ec_pdo_entry_reg_t *
build_entry_regs(const pdo_list_t *rx,
                 const pdo_list_t *tx,
                 unsigned **out_off,
                 int *total_out)
{
    int total=0;

    for (int i=0;i<rx->count;i++)
        total += rx->list[i].entry_count;
    for (int i=0;i<tx->count;i++)
        total += tx->list[i].entry_count;

    *total_out = total;

    unsigned *offs = calloc(total, sizeof(unsigned));
    *out_off = offs;

    ec_pdo_entry_reg_t *r =
        calloc(total+1, sizeof(ec_pdo_entry_reg_t));

    int k=0;

    /* RX first */
    for (int i=0;i<rx->count;i++) {
        const pdo_t *p = &rx->list[i];
        for (int j=0;j<p->entry_count;j++,k++){
            r[k].alias        = 0;
            r[k].position     = 0;
            r[k].vendor_id    = 0x0000006c;
            r[k].product_code = 0x0000a72c;
            r[k].index        = p->entries[j].idx;
            r[k].subindex     = p->entries[j].sub;
            r[k].offset       = &offs[k];
        }
    }

    /* TX */
    for (int i=0;i<tx->count;i++){
        const pdo_t *p = &tx->list[i];
        for (int j=0;j<p->entry_count;j++,k++){
            r[k].alias        = 0;
            r[k].position     = 0;
            r[k].vendor_id    = 0x0000006c;
            r[k].product_code = 0x0000a72c;
            r[k].index        = p->entries[j].idx;
            r[k].subindex     = p->entries[j].sub;
            r[k].offset       = &offs[k];
        }
    }

    r[k] = (ec_pdo_entry_reg_t){0};
    return r;
}

/******************************************************************************
 *                           JSON SM3 FIELDS LOADER
 *****************************************************************************/

typedef struct {
    char name[128];
    int offset;
    int type;  /*1=u8,2=u16,4=u32*/
} field_t;

static int load_fields(const char *path, field_t **out, int *nf)
{
    json_error_t err;
    json_t *root = json_load_file(path,0,&err);
    if (!root) return -1;

    json_t *sm3 = json_object_get(
                      json_object_get(
                          json_object_get(root,"fields"),
                          "slave0"),
                      "sm3");

    int n = json_array_size(sm3);
    field_t *arr = calloc(n, sizeof(field_t));

    for (int i=0;i<n;i++){
        json_t *f = json_array_get(sm3,i);
        strncpy(arr[i].name,
            json_string_value(json_object_get(f,"name")),
            127);
        arr[i].offset = json_integer_value(json_object_get(f,"offset"));
        const char *ty = json_string_value(json_object_get(f,"type"));

        if (!strcasecmp(ty,"u8")) arr[i].type=1;
        else if (!strcasecmp(ty,"u16")) arr[i].type=2;
        else arr[i].type=4;
    }

    *out = arr;
    *nf  = n;
    json_decref(root);
    return 0;
}

/******************************************************************************
 *                                 MAIN
 *****************************************************************************/

int main(int argc,char **argv)
{
    if (argc<2){
        printf("Usage: sudo %s pdo_config.json\n",argv[0]);
        return 1;
    }

    /* Load SM3 field list */
    field_t *fields;
    int nfields;
    if (load_fields(argv[1], &fields, &nfields)) {
        fprintf(stderr,"JSON load failed\n");
        return 1;
    }

    /* Request master */
    ec_master_t *master = ecrt_request_master(0);
    if (!master) return 1;

    /* Create minimal domain (needed to activate master) */
    ec_domain_t *dom = ecrt_master_create_domain(master);
    if (!dom) return 1;

    /* Configure slave (no PDO configuration yet!) */
    ec_slave_config_t *sc =
        ecrt_master_slave_config(master,0,0,
                                 0x0000006c,0x0000a72c);
    if (!sc) return 1;

    printf("Waiting for master activation...\n");
    if (ecrt_master_activate(master)) {
        fprintf(stderr,"activate failed\n");
        return 1;
    }

    /* Mailbox / FSM warm-up */
    printf("Waiting for PREOP and mailbox availability...\n");
    for (;;) {
        /* Drive the master FSM */
        ecrt_master_receive(master);
        ecrt_domain_process(dom);
        ecrt_domain_queue(dom);
        ecrt_master_send(master);

        /* Check slave state */
        ec_slave_config_state_t st;
        ecrt_slave_config_state(sc, &st);

        if (st.al_state == EC_AL_STATE_PREOP)
            break;

        usleep(5000);
    }

    printf("Slave entered PREOP.\n");
    printf("Priming mailbox before SDO...\n");

    for (int i=0;i<200;i++){
        ecrt_master_receive(master);
        ecrt_domain_process(dom);
        ecrt_domain_queue(dom);
        ecrt_master_send(master);
        usleep(1000);
    }

    /* ---------- NOW SAFE TO ISSUE SDO CALLS ---------- */

    pdo_list_t rx, tx;
    printf("Reading PDO assignment + mapping...\n");
    if (build_pdo_lists(sc, &rx, &tx)) {
        fprintf(stderr,"PDO map build failed\n");
        return 1;
    }

    printf("PDO map read OK.\n");

    /* Build sync + PDO info */
    ec_pdo_info_t  *infos = build_info(&rx,&tx);
    ec_sync_info_t *syncs = build_syncs(&rx,&tx,infos);

    printf("Configuring slave PDOs...\n");
    if (ecrt_slave_config_pdos(sc, EC_END, syncs)) {
        fprintf(stderr,"slave_config_pdos failed\n");
        return 1;
    }

    /* Re-register domain with real PDO list */
    unsigned *offs;
    int nentries;
    ec_pdo_entry_reg_t *regs =
        build_entry_regs(&rx,&tx,&offs,&nentries);

    if (ecrt_domain_reg_pdo_entry_list(dom, regs)) {
        fprintf(stderr,"domain_reg failed\n");
        return 1;
    }

    uint8_t *pd = ecrt_domain_data(dom);

    /* Compute boundary: first TX entry = after all RX entries */
    int rx_entries = 0;
    for (int i=0;i<rx.count;i++)
        rx_entries += rx.list[i].entry_count;

    int sm3_base = offs[rx_entries];

    /* ---------------- MAIN LOOP ---------------- */
    printf("Entering cyclic readout...\n");

    while (1){
        ecrt_master_receive(master);
        ecrt_domain_process(dom);

        system("clear");
        printf("=== Live SM3 Viewer ===\n");

        for (int i=0;i<nfields;i++){
            int off = sm3_base + fields[i].offset;
            uint32_t v=0;

            if (fields[i].type==1) v=pd[off];
            else if (fields[i].type==2)
                v=pd[off] | (pd[off+1]<<8);
            else
                v=pd[off] | (pd[off+1]<<8) |
                  (pd[off+2]<<16)|(pd[off+3]<<24);

            printf("%-28s : %u\n", fields[i].name, v);
        }

        ecrt_domain_queue(dom);
        ecrt_master_send(master);

        usleep(100000);
    }

    return 0;
}
