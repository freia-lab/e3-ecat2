#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <jansson.h>
#include <ecrt.h>

typedef struct {
    uint16_t pdo_index;
    uint16_t entry_index;
    int      size_bytes;
} sm_cfg_t;

typedef struct {
    uint16_t alias;
    uint16_t position;
    uint32_t vendor_id;
    uint32_t product_code;
    sm_cfg_t sm2; // outputs -> slave
    sm_cfg_t sm3; // inputs  <- slave
} slave_cfg_t;

typedef struct {
    uint32_t vendor_id;
    uint32_t product_code;
    int      max_bytes_per_direction;
} defaults_t;

typedef struct {
    defaults_t  defaults;
    slave_cfg_t *slaves;
    size_t       n_slaves;
} app_cfg_t;

typedef struct {
    unsigned int byte;
    unsigned int bit;
} reg_info_t;

static uint32_t parse_u32_hex(const char *s, uint32_t fallback) {
    if (!s || !*s) return fallback;
    return (uint32_t)strtoul(s, NULL, 0); // handles "0x...."
}

static int load_json(const char *path, app_cfg_t *cfg)
{
    json_t *root = NULL, *jdefs = NULL, *jslaves = NULL;
    json_error_t err;

    memset(cfg, 0, sizeof(*cfg));
    root = json_load_file(path, 0, &err);
    if (!root) {
        fprintf(stderr, "JSON error: %s (line %d)\n", err.text, err.line);
        return -1;
    }

    // defaults
    jdefs = json_object_get(root, "defaults");
    if (jdefs && json_is_object(jdefs)) {
        cfg->defaults.vendor_id = parse_u32_hex(json_string_value(json_object_get(jdefs,"vendor_id")), 0);
        cfg->defaults.product_code = parse_u32_hex(json_string_value(json_object_get(jdefs,"product_code")), 0);
        json_t *jmax = json_object_get(jdefs, "max_bytes_per_direction");
        cfg->defaults.max_bytes_per_direction = json_is_integer(jmax) ? (int)json_integer_value(jmax) : 250;
    } else {
        cfg->defaults.vendor_id = 0;
        cfg->defaults.product_code = 0;
        cfg->defaults.max_bytes_per_direction = 250;
    }

    // slaves
    jslaves = json_object_get(root, "slaves");
    if (!jslaves || !json_is_array(jslaves)) {
        fprintf(stderr, "JSON: 'slaves' array missing\n");
        json_decref(root);
        return -1;
    }

    cfg->n_slaves = json_array_size(jslaves);
    cfg->slaves = (slave_cfg_t*)calloc(cfg->n_slaves, sizeof(slave_cfg_t));
    if (!cfg->slaves) { json_decref(root); return -1; }

    for (size_t i = 0; i < cfg->n_slaves; i++) {
        json_t *js = json_array_get(jslaves, i);
        if (!js || !json_is_object(js)) { json_decref(root); return -1; }

        slave_cfg_t *s = &cfg->slaves[i];

        s->alias    = (uint16_t)(json_integer_value(json_object_get(js, "alias")));
        s->position = (uint16_t)(json_integer_value(json_object_get(js, "position")));

        const char *vid = json_string_value(json_object_get(js, "vendor_id"));
        const char *pc  = json_string_value(json_object_get(js, "product_code"));
        s->vendor_id    = vid ? parse_u32_hex(vid, cfg->defaults.vendor_id) : cfg->defaults.vendor_id;
        s->product_code = pc  ? parse_u32_hex(pc,  cfg->defaults.product_code) : cfg->defaults.product_code;

        json_t *j2 = json_object_get(js, "sm2");
        json_t *j3 = json_object_get(js, "sm3");
        if (!j2 || !j3) { json_decref(root); return -1; }

        s->sm2.pdo_index   = (uint16_t)parse_u32_hex(json_string_value(json_object_get(j2,"pdo_index")), 0x1600);
        s->sm2.entry_index = (uint16_t)parse_u32_hex(json_string_value(json_object_get(j2,"entry_index")), 0x7000);
        s->sm2.size_bytes  = (int)json_integer_value(json_object_get(j2,"size_bytes"));

        s->sm3.pdo_index   = (uint16_t)parse_u32_hex(json_string_value(json_object_get(j3,"pdo_index")), 0x1A00);
        s->sm3.entry_index = (uint16_t)parse_u32_hex(json_string_value(json_object_get(j3,"entry_index")), 0x6000);
        s->sm3.size_bytes  = (int)json_integer_value(json_object_get(j3,"size_bytes"));

        // Basic sanity
        if (s->sm2.size_bytes <= 0 || s->sm3.size_bytes <= 0) {
            fprintf(stderr, "Slave %zu: invalid size_bytes\n", i);
            json_decref(root); return -1;
        }
        if (s->sm2.size_bytes > cfg->defaults.max_bytes_per_direction ||
            s->sm3.size_bytes > cfg->defaults.max_bytes_per_direction) {
            fprintf(stderr, "Slave %zu: size exceeds max_bytes_per_direction (%d)\n",
                    i, cfg->defaults.max_bytes_per_direction);
            json_decref(root); return -1;
        }
    }

    json_decref(root);
    return 0;
}

static int configure_slave(ec_master_t *master, const slave_cfg_t *s)
{
    // allocate entries
    ec_pdo_entry_info_t *sm2_entries = calloc(s->sm2.size_bytes, sizeof(ec_pdo_entry_info_t));
    ec_pdo_entry_info_t *sm3_entries = calloc(s->sm3.size_bytes, sizeof(ec_pdo_entry_info_t));
    if (!sm2_entries || !sm3_entries) { fprintf(stderr, "calloc failed\n"); return -1; }

    for (int i = 0; i < s->sm2.size_bytes; i++) {
        sm2_entries[i].index = s->sm2.entry_index;
        sm2_entries[i].subindex = (uint8_t)(i + 1);
        sm2_entries[i].bit_length = 8;
    }
    for (int i = 0; i < s->sm3.size_bytes; i++) {
        sm3_entries[i].index = s->sm3.entry_index;
        sm3_entries[i].subindex = (uint8_t)(i + 1);
        sm3_entries[i].bit_length = 8;
    }

    ec_pdo_info_t sm2_pdos[] = { { s->sm2.pdo_index, s->sm2.size_bytes, sm2_entries } };
    ec_pdo_info_t sm3_pdos[] = { { s->sm3.pdo_index, s->sm3.size_bytes, sm3_entries } };

    ec_sync_info_t syncs[] = {
        {0, EC_DIR_OUTPUT, 0, NULL, EC_WD_DISABLE},
        {1, EC_DIR_INPUT,  0, NULL, EC_WD_DISABLE},
        {2, EC_DIR_OUTPUT, 1, sm2_pdos, EC_WD_ENABLE},
        {3, EC_DIR_INPUT,  1, sm3_pdos, EC_WD_DISABLE},
        {0xff}
    };

    ec_slave_config_t *sc = ecrt_master_slave_config(
        master, s->alias, s->position, s->vendor_id, s->product_code);
    if (!sc) { fprintf(stderr, "slave_config(alias=%u,pos=%u) failed\n", s->alias, s->position); return -1; }

    if (ecrt_slave_config_pdos(sc, EC_END, syncs)) {
        fprintf(stderr, "slave_config_pdos failed (pos=%u)\n", s->position);
        return -1;
    }

    printf("Configured slave alias=%u pos=%u (SM2=%dB, SM3=%dB)\n",
        s->alias, s->position, s->sm2.size_bytes, s->sm3.size_bytes);
    return 0;
}

static int configure_all(ec_master_t *master, const app_cfg_t *cfg)
{
    for (size_t i = 0; i < cfg->n_slaves; i++) {
        if (configure_slave(master, &cfg->slaves[i])) return -1;
    }
    return 0;
}

static int build_domain_and_print_offsets(ec_master_t *master, const app_cfg_t *cfg)
{
    ec_domain_t *domain = ecrt_master_create_domain(master);
    if (!domain) { fprintf(stderr, "create_domain failed\n"); return -1; }

    // Count total entries
    size_t total = 0;
    for (size_t i = 0; i < cfg->n_slaves; i++) {
        total += cfg->slaves[i].sm2.size_bytes;
        total += cfg->slaves[i].sm3.size_bytes;
    }

    // Build regs + capture offsets/bits
    ec_pdo_entry_reg_t *regs = calloc(total + 1, sizeof(ec_pdo_entry_reg_t));
    reg_info_t *infos = calloc(total, sizeof(reg_info_t));
    if (!regs || !infos) { fprintf(stderr, "alloc failed\n"); return -1; }

    size_t ix = 0;
    for (size_t s = 0; s < cfg->n_slaves; s++) {
        const slave_cfg_t *sl = &cfg->slaves[s];
        // SM2 (write)
        for (int i = 0; i < sl->sm2.size_bytes; i++, ix++) {
            regs[ix].alias = sl->alias;
            regs[ix].position = sl->position;
            regs[ix].vendor_id = sl->vendor_id;
            regs[ix].product_code = sl->product_code;
            regs[ix].index = sl->sm2.entry_index;
            regs[ix].subindex = (uint8_t)(i + 1);
            regs[ix].offset = &infos[ix].byte;
            regs[ix].bit_position = &infos[ix].bit;
        }
        // SM3 (read)
        for (int i = 0; i < sl->sm3.size_bytes; i++, ix++) {
            regs[ix].alias = sl->alias;
            regs[ix].position = sl->position;
            regs[ix].vendor_id = sl->vendor_id;
            regs[ix].product_code = sl->product_code;
            regs[ix].index = sl->sm3.entry_index;
            regs[ix].subindex = (uint8_t)(i + 1);
            regs[ix].offset = &infos[ix].byte;
            regs[ix].bit_position = &infos[ix].bit;
        }
    }
    // terminator is already zeroed

    if (ecrt_domain_reg_pdo_entry_list(domain, regs)) {
        fprintf(stderr, "domain_reg_pdo_entry_list failed\n"); return -1;
    }
    if (ecrt_master_activate(master)) {
        fprintf(stderr, "master_activate failed\n"); return -1;
    }

    // Print offsets
    printf("\n===== Domain mapping =====\n");
    ix = 0;
    size_t running_base = 0;
    for (size_t s = 0; s < cfg->n_slaves; s++) {
        const slave_cfg_t *sl = &cfg->slaves[s];

        printf("Slave alias=%u pos=%u (vendor=0x%08x product=0x%08x)\n",
            sl->alias, sl->position, sl->vendor_id, sl->product_code);

        printf("  SM2 (Output -> slave), %d entries @ index 0x%04x:\n",
            sl->sm2.size_bytes, sl->sm2.entry_index);
        for (int i = 0; i < sl->sm2.size_bytes; i++, ix++) {
            printf("    %4zu: 0x%04x:%-3d  offset=%-4u bit=%u\n",
                ix, sl->sm2.entry_index, i+1, infos[ix].byte, infos[ix].bit);
        }

        printf("  SM3 (Input <- slave), %d entries @ index 0x%04x:\n",
            sl->sm3.size_bytes, sl->sm3.entry_index);
        for (int i = 0; i < sl->sm3.size_bytes; i++, ix++) {
            printf("    %4zu: 0x%04x:%-3d  offset=%-4u bit=%u\n",
                ix, sl->sm3.entry_index, i+1, infos[ix].byte, infos[ix].bit);
        }

        // Simple invariant: this slave contributed sm2+sm3 bytes
        running_base += (size_t)sl->sm2.size_bytes + (size_t)sl->sm3.size_bytes;
    }

    printf("\nTotal domain entries: %zu\n", total);
    printf("Domain size (bytes): %zu\n", ecrt_domain_size(domain));

    // Unit-test style validations
    int failures = 0;
    // 1) Check offsets monotonic (we registered in a contiguous sequence)
    for (size_t i = 0; i < total; i++) {
        if (infos[i].bit != 0) { fprintf(stderr, "[TEST] Non-zero bit position at %zu\n", i); failures++; }
        if (i > 0) {
            if (!(infos[i].byte == infos[i-1].byte + 1 || infos[i].byte == infos[i-1].byte)) {
                // this simplistic check is sufficient because all entries are 8-bit;
                // exact equality can happen only if master aligns differently (rare with all 8-bit).
                if (infos[i].byte != i) { // expected packed layout
                    fprintf(stderr, "[TEST] Unexpected offset jump at %zu (got %u)\n", i, infos[i].byte);
                    failures++;
                }
            }
        } else {
            if (infos[i].byte != 0) { fprintf(stderr, "[TEST] First offset not zero\n"); failures++; }
        }
    }
    // 2) Check domain size equals last offset+1 for all-8bit entries
    if (ecrt_domain_size(domain) != total) {
        fprintf(stderr, "[TEST] Domain size %zu != total entries %zu\n",
            ecrt_domain_size(domain), total);
        failures++;
    }

    if (failures) {
        fprintf(stderr, "VALIDATION FAILED: %d issue(s) detected.\n", failures);
        return -1;
    } else {
        printf("VALIDATION PASSED: mapping and domain size OK.\n");
    }

    return 0;
}

static void print_usage(const char *argv0) {
    printf("Usage:\n");
    printf("  sudo %s <config.json>\n", argv0);
    printf("\nOptions:\n");
    printf("  --sleep <sec>   Hold master for N seconds (default 2)\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) { print_usage(argv[0]); return 1; }

    const char *json_path = argv[1];
    int hold_sec = 2;

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--sleep") && i+1 < argc) {
            hold_sec = atoi(argv[++i]);
        }
    }

    app_cfg_t cfg;
    if (load_json(json_path, &cfg)) return 1;

    ec_master_t *master = ecrt_request_master(0);
    if (!master) { fprintf(stderr, "request_master failed\n"); return 1; }

    if (configure_all(master, &cfg)) return 1;
    if (build_domain_and_print_offsets(master, &cfg)) return 1;

    printf("\nHolding master for %d second(s)...\n", hold_sec);
    sleep(hold_sec);
    return 0;
}
