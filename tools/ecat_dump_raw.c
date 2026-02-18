#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <ecrt.h>

/* Device-specific */
#define SM2_BYTES 62
#define SM3_BYTES 234 /* only logical table size; true SM3 region may be larger */
#define MAX_SM3_SCAN 512   /* dump up to 512 bytes if they exist */

/* Dumps raw memory with ASCII view */
static void hex_ascii_dump(const uint8_t *buf, int size)
{
    for (int i = 0; i < size; i += 16) {
        printf("%04x : ", i);
        for (int j = 0; j < 16 && i+j < size; j++)
            printf("%02x ", buf[i+j]);
        printf(" | ");
        for (int j = 0; j < 16 && i+j < size; j++) {
            uint8_t c = buf[i+j];
            printf("%c", (c >= 32 && c <= 126) ? c : '.');
        }
        printf("\n");
    }
}

int main(void)
{
    ec_master_t *master = ecrt_request_master(0);
    if (!master) {
        fprintf(stderr, "ecrt_request_master failed\n");
        return 1;
    }

    ec_slave_config_t *sc = ecrt_master_slave_config(master,
        0, 0, 0x0000006c, 0x0000a72c);
    if (!sc) { fprintf(stderr, "slave_config failed\n"); return 1; }

    /* SM2 PDO entries: 62 bytes */
    ec_pdo_entry_info_t sm2e[SM2_BYTES];
    for (int i=0;i<SM2_BYTES;i++) sm2e[i] = (ec_pdo_entry_info_t){0x7000,(uint8_t)(i+1),8};
    ec_pdo_info_t sm2pdo = {0x1600, SM2_BYTES, sm2e};

    /* SM3 PDO entries: we artificially register up to MAX_SM3_SCAN */
    int scan_bytes = MAX_SM3_SCAN;
    ec_pdo_entry_info_t *sm3e = calloc(scan_bytes, sizeof(*sm3e));
    for (int i=0;i<scan_bytes;i++) sm3e[i]=(ec_pdo_entry_info_t){0x6000,(uint8_t)(i+1),8};
    ec_pdo_info_t sm3pdo = {0x1A00, scan_bytes, sm3e};

    ec_sync_info_t syncs[] = {
        {0, EC_DIR_OUTPUT, 0, NULL,     EC_WD_DISABLE},
        {1, EC_DIR_INPUT,  0, NULL,     EC_WD_DISABLE},
        {2, EC_DIR_OUTPUT, 1, &sm2pdo,  EC_WD_ENABLE},
        {3, EC_DIR_INPUT,  1, &sm3pdo,  EC_WD_DISABLE},
        {0xff}
    };

    if (ecrt_slave_config_pdos(sc, EC_END, syncs)) {
        fprintf(stderr,"config_pdos failed\n");
        return 1;
    }

    ec_domain_t *domain = ecrt_master_create_domain(master);
    if (!domain) {
        fprintf(stderr, "create_domain failed\n");
        return 1;
    }

    /* Register ALL entries: SM2 first, then MAX_SM3_SCAN */
    int total = SM2_BYTES + scan_bytes;
    ec_pdo_entry_reg_t *regs = calloc(total+1, sizeof(*regs));
    unsigned *offs = calloc(total, sizeof(unsigned));
    int ix=0;

    for (int i=0;i<SM2_BYTES;i++,ix++) {
        regs[ix]=(ec_pdo_entry_reg_t){
            .alias=0,.position=0,
            .vendor_id=0x0000006c,.product_code=0x0000a72c,
            .index=0x7000,.subindex=(uint8_t)(i+1),
            .offset=&offs[ix]
        };
    }
    for (int i=0;i<scan_bytes;i++,ix++) {
        regs[ix]=(ec_pdo_entry_reg_t){
            .alias=0,.position=0,
            .vendor_id=0x0000006c,.product_code=0x0000a72c,
            .index=0x6000,.subindex=(uint8_t)(i+1),
            .offset=&offs[ix]
        };
    }

    if (ecrt_domain_reg_pdo_entry_list(domain, regs)) {
        fprintf(stderr, "reg_pdo_entry_list failed\n");
        return 1;
    }

    if (ecrt_master_activate(master)) {
        fprintf(stderr, "activate failed\n");
        return 1;
    }

    uint8_t *dom = ecrt_domain_data(domain);
    if (!dom) {
        fprintf(stderr, "domain_data NULL\n");
        return 1;
    }

    /* Wait for valid data */
    usleep(20000);
    ecrt_master_receive(master);
    ecrt_domain_process(domain);

    /* Determine SM3 domain offset range */
    unsigned sm3_start = offs[SM2_BYTES + 0];
    unsigned sm3_end   = offs[SM2_BYTES + scan_bytes - 1];

    printf("=== SM3 raw region (domain offsets %u..%u) ===\n",
           sm3_start, sm3_end);

    printf("Dumping %d bytes from domain offset %u:\n", scan_bytes, sm3_start);
    hex_ascii_dump(dom + sm3_start, scan_bytes);

    return 0;
}
