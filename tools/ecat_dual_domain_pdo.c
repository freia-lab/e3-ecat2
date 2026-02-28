// ecat_dual_domain_pdo.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <ecrt.h>

#define VENDOR_ID     0x0000006c
#define PRODUCT_CODE  0x0000a72c
#define NSEC_PER_SEC  1000000000ULL

// Byte counts per your bus dump:
#define OUT_2000_COUNT 200  // 0x2000:01..0x2000:C8
#define OUT_2001_COUNT  50  // 0x2001:01..0x2001:32
#define IN_3000_COUNT  200  // 0x3000:01..0x3000:C8
#define IN_3001_COUNT   50  // 0x3001:01..0x3001:32

static volatile int run = 1;
static inline void timespec_add(struct timespec *t, uint64_t add_ns) {
    t->tv_nsec += add_ns;
    while (t->tv_nsec >= (long)NSEC_PER_SEC) { t->tv_nsec -= NSEC_PER_SEC; t->tv_sec++; }
}
static void on_signal(int s) { (void)s; run = 0; }

// Master objects
static ec_master_t  *master     = NULL;
static ec_domain_t  *domain_out = NULL; // SM2: outputs (RxPDO)
static ec_domain_t  *domain_in  = NULL; // SM3: inputs  (TxPDO)
static ec_slave_config_t *sc    = NULL;
static uint8_t *pd_out = NULL, *pd_in = NULL;

// Offsets
static unsigned int off_out_2000[OUT_2000_COUNT];
static unsigned int off_out_2001[OUT_2001_COUNT];
static unsigned int off_in_3000[IN_3000_COUNT];
static unsigned int off_in_3001[IN_3001_COUNT];

#define SUB_TO_IDX(si) ((unsigned)((si) - 1))
#define OFF_OUT_2000(si) (off_out_2000[SUB_TO_IDX(si)])
#define OFF_IN_3000(si)  (off_in_3000 [SUB_TO_IDX(si)])

// Build PDO description dynamically so we can call ecrt_slave_config_pdos().
static int build_and_apply_pdo_mapping(ec_slave_config_t *sc)
{
    // Allocate PDO entry arrays
    ec_pdo_entry_info_t *rx1600 = calloc(OUT_2000_COUNT, sizeof(*rx1600));
    ec_pdo_entry_info_t *rx1601 = calloc(OUT_2001_COUNT, sizeof(*rx1601));
    ec_pdo_entry_info_t *tx1a00 = calloc(IN_3000_COUNT, sizeof(*tx1a00));
    ec_pdo_entry_info_t *tx1a01 = calloc(IN_3001_COUNT, sizeof(*tx1a01));
    if (!rx1600 || !rx1601 || !tx1a00 || !tx1a01) return -1;

    for (unsigned i = 0; i < OUT_2000_COUNT; ++i) { rx1600[i] = (ec_pdo_entry_info_t){0x2000, (uint8_t)(i+1), 8}; }
    for (unsigned i = 0; i < OUT_2001_COUNT; ++i) { rx1601[i] = (ec_pdo_entry_info_t){0x2001, (uint8_t)(i+1), 8}; }
    for (unsigned i = 0; i < IN_3000_COUNT;  ++i) { tx1a00[i] = (ec_pdo_entry_info_t){0x3000, (uint8_t)(i+1), 8}; }
    for (unsigned i = 0; i < IN_3001_COUNT;  ++i) { tx1a01[i] = (ec_pdo_entry_info_t){0x3001, (uint8_t)(i+1), 8}; }

    ec_pdo_info_t rxpdos[2] = {
        {0x1600, OUT_2000_COUNT, rx1600},
        {0x1601, OUT_2001_COUNT, rx1601},
    };
    ec_pdo_info_t txpdos[2] = {
        {0x1A00, IN_3000_COUNT,  tx1a00},
        {0x1A01, IN_3001_COUNT,  tx1a01},
    };

    ec_sync_info_t syncs[5];
    memset(syncs, 0, sizeof(syncs));
    // SM0/1: mailboxes (unchanged)
    syncs[0] = (ec_sync_info_t){0, EC_DIR_OUTPUT, 0, NULL, EC_WD_DISABLE};
    syncs[1] = (ec_sync_info_t){1, EC_DIR_INPUT,  0, NULL, EC_WD_DISABLE};
    // SM2: RxPDO (outputs)
    syncs[2] = (ec_sync_info_t){2, EC_DIR_OUTPUT, 2, rxpdos, EC_WD_ENABLE};
    // SM3: TxPDO (inputs)
    syncs[3] = (ec_sync_info_t){3, EC_DIR_INPUT,  2, txpdos, EC_WD_DISABLE};
    // terminator
    syncs[4] = (ec_sync_info_t){0xff};

    // **Critical**: Tell the master the exact PDO mapping (matches your bus dump).
    if (ecrt_slave_config_pdos(sc, EC_END, syncs)) {
        fprintf(stderr, "ecrt_slave_config_pdos() failed. Check mapping.\n");
        return -1;
    }
    // keep arrays around until activation (simple demo keeps them for process lifetime)
    return 0;
}

static int register_domain_entries(void)
{
    // Only OUT PDO entries → domain_out
    ec_pdo_entry_reg_t regs_out[OUT_2000_COUNT + OUT_2001_COUNT + 1];
    size_t k = 0;
    for (unsigned i = 0; i < OUT_2000_COUNT; ++i) {
        regs_out[k++] = (ec_pdo_entry_reg_t){ .alias=0,.position=0,.vendor_id=VENDOR_ID,.product_code=PRODUCT_CODE,
            .index=0x2000,.subindex=(uint8_t)(i+1), .offset=&off_out_2000[i] };
    }
    for (unsigned i = 0; i < OUT_2001_COUNT; ++i) {
        regs_out[k++] = (ec_pdo_entry_reg_t){ .alias=0,.position=0,.vendor_id=VENDOR_ID,.product_code=PRODUCT_CODE,
            .index=0x2001,.subindex=(uint8_t)(i+1), .offset=&off_out_2001[i] };
    }
    regs_out[k] = (ec_pdo_entry_reg_t){}; // terminator

    if (ecrt_domain_reg_pdo_entry_list(domain_out, regs_out)) {
        fprintf(stderr, "PDO entry registration for domain_out failed!\n");
        return -1;
    }

    // Only IN PDO entries → domain_in
    ec_pdo_entry_reg_t regs_in[IN_3000_COUNT + IN_3001_COUNT + 1];
    k = 0;
    for (unsigned i = 0; i < IN_3000_COUNT; ++i) {
        regs_in[k++] = (ec_pdo_entry_reg_t){ .alias=0,.position=0,.vendor_id=VENDOR_ID,.product_code=PRODUCT_CODE,
            .index=0x3000,.subindex=(uint8_t)(i+1), .offset=&off_in_3000[i] };
    }
    for (unsigned i = 0; i < IN_3001_COUNT; ++i) {
        regs_in[k++] = (ec_pdo_entry_reg_t){ .alias=0,.position=0,.vendor_id=VENDOR_ID,.product_code=PRODUCT_CODE,
            .index=0x3001,.subindex=(uint8_t)(i+1), .offset=&off_in_3001[i] };
    }
    regs_in[k] = (ec_pdo_entry_reg_t){};

    if (ecrt_domain_reg_pdo_entry_list(domain_in, regs_in)) {
        fprintf(stderr, "PDO entry registration for domain_in failed!\n");
        return -1;
    }
    return 0;
}

int main(void)
{
    const uint32_t cycle_ns = 10 * 1000 * 1000; // 10 ms
    const uint16_t LOCAL  = 0xAAAA, REMOTE = 0xBBBB;

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    master = ecrt_request_master(0);
    if (!master) { fprintf(stderr, "ecrt_request_master failed\n"); return -1; }

    domain_out = ecrt_master_create_domain(master);
    domain_in  = ecrt_master_create_domain(master);
    if (!domain_out || !domain_in) { fprintf(stderr, "create_domain failed\n"); return -1; }

    // Configure slave at position 0 with strict identity
    sc = ecrt_master_slave_config(master, 0, 0, VENDOR_ID, PRODUCT_CODE);
    if (!sc) { fprintf(stderr, "slave_config failed\n"); return -1; }

    // (optional) DC
#if ECRT_VERSION_MAJOR > 1 || (ECRT_VERSION_MAJOR == 1 && ECRT_VERSION_MINOR >= 6)
    if (ecrt_slave_config_dc(sc, 0x0300, cycle_ns, 440000, 0, 0)) {
        fprintf(stderr, "DC config failed\n"); return -1;
    }
#else
    ecrt_slave_config_dc(sc, 0x0300, cycle_ns, 440000, 0, 0);
#endif

    // **Tell the master the PDO layout** (matches the slave's fixed mapping).
    if (build_and_apply_pdo_mapping(sc)) return -1;

    // **Now** register PDO entries into the two domains.
    if (register_domain_entries()) return -1;

    if (ecrt_master_activate(master)) { fprintf(stderr, "activate failed\n"); return -1; }
    pd_out = ecrt_domain_data(domain_out);
    pd_in  = ecrt_domain_data(domain_in);
    if (!pd_out || !pd_in) { fprintf(stderr, "domain data NULL\n"); return -1; }

    printf("Domain OUT size: %zu, Domain IN size: %zu\n",
           ecrt_domain_size(domain_out), ecrt_domain_size(domain_in));

    // Cycle
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    printf("Starting cyclic task...\n");
    uint16_t loop = 0; unsigned diag = 0;

    while (run) {
        timespec_add(&t0, cycle_ns);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t0, NULL);

        ecrt_master_receive(master);
        ecrt_domain_process(domain_out);
        ecrt_domain_process(domain_in);

        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t app_time = (uint64_t)now.tv_sec * NSEC_PER_SEC + now.tv_nsec;
        ecrt_master_application_time(master, app_time);
        ecrt_master_sync_reference_clock(master);
        ecrt_master_sync_slave_clocks(master);

        // --- Writes (SM2/RxPDO) ---
        EC_WRITE_U16(pd_out + OFF_OUT_2000(0x3B), loop++);  // heartbeat
        uint16_t ctrl = 0;
        if (loop > 1000 && loop < 1010) ctrl = REMOTE;
        else if (loop > 2000 && loop < 2010) ctrl = LOCAL;
        EC_WRITE_U16(pd_out + OFF_OUT_2000(0x3D), ctrl);

        // --- Reads (SM3/TxPDO) ---
        uint8_t  gen0 = EC_READ_U8 (pd_in + OFF_IN_3000(0x01));
        uint8_t  gen1 = EC_READ_U8 (pd_in + OFF_IN_3000(0x02));
        uint16_t roof = EC_READ_U16(pd_in + OFF_IN_3000(0x05));
        uint16_t l1   = EC_READ_U16(pd_in + OFF_IN_3000(0x09));
        uint16_t l2   = EC_READ_U16(pd_in + OFF_IN_3000(0x0B));

        ecrt_domain_queue(domain_out);
        ecrt_domain_queue(domain_in);
        ecrt_master_send(master);

        if (++diag >= 100) {
            diag = 0;
            printf("loop=%u gen0=0x%02X gen1=0x%02X roof=%u l1=%u l2=%u\n",
                   loop, gen0, gen1, roof, l1, l2);
        }
    }

    ecrt_release_master(master);
    return 0;
}
