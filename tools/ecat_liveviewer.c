/****************************************************************************
 * ecat_liveviewer.c — static PDO mapping (no SDO)
 *
 * Build:
 *   gcc ecat_liveviewer.c -o ecat_liveviewer -lethercat -ljansson
 *
 * Run:
 *   sudo ./ecat_liveviewer ecat_pdo_config.json
 ****************************************************************************/

#include <ecrt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <jansson.h>

#include "pdo_map.h"

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
        json_t *f  = json_array_get(sm3, i);
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

/* ---------------------------------- MAIN ---------------------------------- */

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: sudo %s ecat_pdo_config.json\n", argv[0]);
        return 1;
    }

    /* Load SM3 fields */
    field_t *fields; int nfields;
    if (load_fields(argv[1], &fields, &nfields)) return 1;

    /* Master + domain */
    ec_master_t *master = ecrt_request_master(0);
    if (!master) { fprintf(stderr,"request_master failed\n"); return 1; }

    ec_domain_t *domain = ecrt_master_create_domain(master);
    if (!domain) { fprintf(stderr,"create_domain failed\n"); return 1; }

    /* Slave config (static PDOs) */
    ec_slave_config_t *sc =
        ecrt_master_slave_config(master, 0 /*alias*/, SLAVE_POS,
                                 VENDOR_ID, PRODUCT_CODE);
    if (!sc) { fprintf(stderr,"slave_config failed\n"); return 1; }

    if (ecrt_slave_config_pdos(sc, EC_END, SLAVE_SYNC_INFO)) {
        fprintf(stderr, "slave_config_pdos failed\n"); return 1;
    }

    /* Register entries → offsets */
    ec_pdo_entry_reg_t *regs = NULL;
    unsigned *entry_offsets  = NULL;
    int total_entries        = 0;

    if (build_entry_regs(&regs, &entry_offsets, &total_entries)) {
        fprintf(stderr,"build_entry_regs failed\n"); return 1;
    }

    if (ecrt_domain_reg_pdo_entry_list(domain, regs)) {
        fprintf(stderr,"domain_reg failed\n"); return 1;
    }

    /* Activate and get PD pointer */
    if (ecrt_master_activate(master)) {
        fprintf(stderr, "master_activate failed\n"); return 1;
    }
    uint8_t *pd = ecrt_domain_data(domain);

    /* First TX entry offset = after all RX entries (we registered RX then TX) */
    const int sm3_base = entry_offsets[total_rx_entries()];

    /* ----------------------------- Cyclic loop ---------------------------- */
    while (1) {
        ecrt_master_receive(master);
        ecrt_domain_process(domain);

        system("clear");
        printf("=== Live SM3 Viewer ===\n");

        for (int i=0; i<nfields; i++) {
            int off = sm3_base + fields[i].offset;
            uint32_t v = 0;

            if (fields[i].type == 1)      v = pd[off];
            else if (fields[i].type == 2) v = pd[off] | (pd[off+1] << 8);
            else                          v = pd[off] |
                                              (pd[off+1] << 8) |
                                              (pd[off+2] << 16) |
                                              (pd[off+3] << 24);

            printf("%-30s : %u\n", fields[i].name, v);
        }

        ecrt_domain_queue(domain);
        ecrt_master_send(master);

        usleep(100000);
    }

    return 0;
}
