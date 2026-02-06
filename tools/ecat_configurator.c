#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jansson.h>
#include <ecrt.h>

// JSON structure for one slave
typedef struct {
    uint16_t sm2_pdo_index;
    uint16_t sm2_entry_index;
    int      sm2_size;

    uint16_t sm3_pdo_index;
    uint16_t sm3_entry_index;
    int      sm3_size;
} slave_cfg_t;


// ------------------------------
// Load JSON
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
    if (!sm2 || !sm3) { fprintf(stderr, "Missing 'sm2' or 'sm3'\n"); json_decref(root); return -1; }

    cfg->sm2_pdo_index   = strtol(json_string_value(json_object_get(sm2,"pdo_index")), NULL, 0);
    cfg->sm2_entry_index = strtol(json_string_value(json_object_get(sm2,"entry_index")),NULL, 0);
    cfg->sm2_size        = json_integer_value(json_object_get(sm2,"size_bytes"));

    cfg->sm3_pdo_index   = strtol(json_string_value(json_object_get(sm3,"pdo_index")), NULL, 0);
    cfg->sm3_entry_index = strtol(json_string_value(json_object_get(sm3,"entry_index")),NULL, 0);
    cfg->sm3_size        = json_integer_value(json_object_get(sm3,"size_bytes"));

    json_decref(root);
    return 0;
}


// ------------------------------
// Configure one slave from JSON
// ------------------------------
static int configure_slave(ec_master_t *master, int pos, const slave_cfg_t *cfg)
{
    // Build entries
    ec_pdo_entry_info_t *sm2_entries = calloc(cfg->sm2_size, sizeof(ec_pdo_entry_info_t));
    ec_pdo_entry_info_t *sm3_entries = calloc(cfg->sm3_size, sizeof(ec_pdo_entry_info_t));
    if (!sm2_entries || !sm3_entries) {
        fprintf(stderr, "calloc failed.\n");
        return -1;
    }

    for (int i = 0; i < cfg->sm2_size; i++) {
        sm2_entries[i].index      = cfg->sm2_entry_index;
        sm2_entries[i].subindex   = i + 1;
        sm2_entries[i].bit_length = 8;
    }

    for (int i = 0; i < cfg->sm3_size; i++) {
        sm3_entries[i].index      = cfg->sm3_entry_index;
        sm3_entries[i].subindex   = i + 1;
        sm3_entries[i].bit_length = 8;
    }

    // PDOs
    ec_pdo_info_t sm2_pdos[1] = {
        { cfg->sm2_pdo_index, cfg->sm2_size, sm2_entries }
    };
    ec_pdo_info_t sm3_pdos[1] = {
        { cfg->sm3_pdo_index, cfg->sm3_size, sm3_entries }
    };

    // SM layout
    ec_sync_info_t syncs[] = {
        {0, EC_DIR_OUTPUT, 0, NULL, EC_WD_DISABLE},
        {1, EC_DIR_INPUT,  0, NULL, EC_WD_DISABLE},
        {2, EC_DIR_OUTPUT, 1, sm2_pdos, EC_WD_ENABLE},
        {3, EC_DIR_INPUT,  1, sm3_pdos, EC_WD_DISABLE},
        {0xff}
    };

    // *** FIXED: EtherLab requires alias + position ***
    ec_slave_config_t *sc =
        ecrt_master_slave_config(master,
                                 0,                 // alias
                                 (uint16_t)pos,     // position
                                 0x0000006c,        // vendor
                                 0x0000a72c);       // product
    if (!sc) {
        fprintf(stderr, "Failed to get slave config at pos %d.\n", pos);
        return -1;
    }

    // Attach PDOs using sync descriptors
    if (ecrt_slave_config_pdos(sc, EC_END, syncs)) {
        fprintf(stderr, "ecrt_slave_config_pdos failed.\n");
        return -1;
    }

    printf("Slave %d configured OK.\n", pos);
    return 0;
}


// ------------------------------
// Main
// ------------------------------
int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <json-config>\n", argv[0]);
        return 1;
    }

    const char *json_path = argv[1];
    slave_cfg_t cfg;

    if (load_config(json_path, &cfg))
        return 1;

    ec_master_t *master = ecrt_request_master(0);
    if (!master) { fprintf(stderr, "request_master failed.\n"); return 1; }

    if (configure_slave(master, 0, &cfg))
        return 1;

    if (ecrt_master_activate(master)) {
        fprintf(stderr, "master_activate failed.\n");
        return 1;
    }

    printf("Configuration completed.\n");
    return 0;
}
