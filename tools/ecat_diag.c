#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <jansson.h>
#include <ecrt.h>

typedef struct {
    uint16_t sm2_pdo_index;
    uint16_t sm2_entry_index;
    int      sm2_size;
    uint16_t sm3_pdo_index;
    uint16_t sm3_entry_index;
    int      sm3_size;
} slave_cfg_t;


// ------------------------------
// Load JSON configuration
// ------------------------------
static int load_config(const char *path, slave_cfg_t *cfg)
{
    json_t *root = NULL, *slave0 = NULL, *sm2 = NULL, *sm3 = NULL;
    json_error_t err;

    root = json_load_file(path, 0, &err);
    if (!root) {
        fprintf(stderr, "JSON error: %s (line %d)\n", err.text, err.line);
        return -1;
    }

    slave0 = json_object_get(root, "slave0");
    if (!slave0) { fprintf(stderr, "Missing 'slave0'\n"); json_decref(root); return -1; }

    sm2 = json_object_get(slave0, "sm2");
    sm3 = json_object_get(slave0, "sm3");

    cfg->sm2_pdo_index   = strtol(json_string_value(json_object_get(sm2,"pdo_index")), NULL, 16);
    cfg->sm2_entry_index = strtol(json_string_value(json_object_get(sm2,"entry_index")),NULL, 16);
    cfg->sm2_size        = json_integer_value(json_object_get(sm2,"size_bytes"));

    cfg->sm3_pdo_index   = strtol(json_string_value(json_object_get(sm3,"pdo_index")), NULL, 16);
    cfg->sm3_entry_index = strtol(json_string_value(json_object_get(sm3,"entry_index")),NULL, 16);
    cfg->sm3_size        = json_integer_value(json_object_get(sm3,"size_bytes"));

    json_decref(root);
    return 0;
}


// ------------------------------
// Configure slave PDO layout
// ------------------------------
static int configure_slave(ec_master_t *master, int pos, const slave_cfg_t *cfg)
{
    ec_pdo_entry_info_t *sm2_entries =
        calloc(cfg->sm2_size, sizeof(ec_pdo_entry_info_t));
    ec_pdo_entry_info_t *sm3_entries =
        calloc(cfg->sm3_size, sizeof(ec_pdo_entry_info_t));

    if (!sm2_entries || !sm3_entries) {
        fprintf(stderr, "calloc failed.\n");
        return -1;
    }

    for (int i = 0; i < cfg->sm2_size; i++) {
        sm2_entries[i] = (ec_pdo_entry_info_t){
            cfg->sm2_entry_index, (uint8_t)(i + 1), 8
        };
    }

    for (int i = 0; i < cfg->sm3_size; i++) {
        sm3_entries[i] = (ec_pdo_entry_info_t){
            cfg->sm3_entry_index, (uint8_t)(i + 1), 8
        };
    }

    ec_pdo_info_t sm2_pdos[] = {
        { cfg->sm2_pdo_index, cfg->sm2_size, sm2_entries }
    };

    ec_pdo_info_t sm3_pdos[] = {
        { cfg->sm3_pdo_index, cfg->sm3_size, sm3_entries }
    };

    ec_sync_info_t syncs[] = {
        {0, EC_DIR_OUTPUT, 0, NULL, EC_WD_DISABLE},
        {1, EC_DIR_INPUT,  0, NULL, EC_WD_DISABLE},
        {2, EC_DIR_OUTPUT, 1, sm2_pdos, EC_WD_ENABLE},
        {3, EC_DIR_INPUT,  1, sm3_pdos, EC_WD_DISABLE},
        {0xff}
    };

    ec_slave_config_t *sc =
        ecrt_master_slave_config(master,
                                 0,             // alias
                                 (uint16_t)pos, // position
                                 0x0000006c,    // vendor
                                 0x0000a72c);   // product
    if (!sc) {
        fprintf(stderr, "ecrt_master_slave_config failed\n");
        return -1;
    }

    if (ecrt_slave_config_pdos(sc, EC_END, syncs)) {
        fprintf(stderr, "ecrt_slave_config_pdos failed\n");
        return -1;
    }

    return 0;
}


// ------------------------------
// Print domain offsets (CORRECT IgH METHOD)
// ------------------------------
typedef struct {
    unsigned int byte;
    unsigned int bit;
} reg_info_t;


static int build_and_print_offsets(ec_master_t *master, const slave_cfg_t *cfg)
{
    ec_domain_t *domain = ecrt_master_create_domain(master);
    if (!domain) {
        fprintf(stderr, "create_domain failed\n");
        return -1;
    }

    const unsigned total = cfg->sm2_size + cfg->sm3_size;

    ec_pdo_entry_reg_t *regs =
        calloc(total + 1, sizeof(ec_pdo_entry_reg_t));
    reg_info_t *infos =
        calloc(total, sizeof(reg_info_t));

    if (!regs || !infos) {
        fprintf(stderr, "allocation failed\n");
        return -1;
    }

    unsigned idx = 0;

    // SM2 (write)
    for (int i = 0; i < cfg->sm2_size; i++, idx++) {
        regs[idx].alias        = 0;
        regs[idx].position     = 0;
        regs[idx].vendor_id    = 0x0000006c;
        regs[idx].product_code = 0x0000a72c;
        regs[idx].index        = cfg->sm2_entry_index;
        regs[idx].subindex     = i + 1;

        regs[idx].offset       = &infos[idx].byte;
        regs[idx].bit_position = &infos[idx].bit;
    }

    // SM3 (read)
    for (int i = 0; i < cfg->sm3_size; i++, idx++) {
        regs[idx].alias        = 0;
        regs[idx].position     = 0;
        regs[idx].vendor_id    = 0x0000006c;
        regs[idx].product_code = 0x0000a72c;
        regs[idx].index        = cfg->sm3_entry_index;
        regs[idx].subindex     = i + 1;

        regs[idx].offset       = &infos[idx].byte;
        regs[idx].bit_position = &infos[idx].bit;
    }

    // Register the PDO entries
    if (ecrt_domain_reg_pdo_entry_list(domain, regs)) {
        fprintf(stderr, "domain_reg_pdo_entry_list failed\n");
        return -1;
    }

    if (ecrt_master_activate(master)) {
        fprintf(stderr, "activate failed\n");
        return -1;
    }

    printf("\n----- Domain Offsets -----\n");
    idx = 0;

    for (int i = 0; i < cfg->sm2_size; i++, idx++) {
        printf("  %3u:  0x%04x:%u  offset=%u bit=%u  (SM2 Write)\n",
            idx, cfg->sm2_entry_index, i + 1,
            infos[idx].byte, infos[idx].bit);
    }

    for (int i = 0; i < cfg->sm3_size; i++, idx++) {
        printf("  %3u:  0x%04x:%u  offset=%u bit=%u  (SM3 Read)\n",
            idx, cfg->sm3_entry_index, i + 1,
            infos[idx].byte, infos[idx].bit);
    }

    printf("\nTotal entries: %u\n", total);
    printf("Domain size: %zu bytes\n",
           ecrt_domain_size(domain));

    return 0;
}


// ------------------------------
// MAIN
// ------------------------------
int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <ecat_pdo_config.json>\n", argv[0]);
        return 1;
    }

    slave_cfg_t cfg;

    if (load_config(argv[1], &cfg))
        return 1;

    ec_master_t *master = ecrt_request_master(0);
    if (!master) {
        fprintf(stderr, "request_master failed\n");
        return 1;
    }

    if (configure_slave(master, 0, &cfg))
        return 1;

    if (build_and_print_offsets(master, &cfg))
        return 1;

    return 0;
}
