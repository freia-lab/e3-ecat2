// ecat_minimal_pdo.c
// IgH EtherCAT Master, two domains (SM2 outputs, SM3 inputs), domain offsets only.
// No SDO addressing, no PDO remap (device forbids it).

#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <ecrt.h>

#define VENDOR_ID     0x0000006c
#define PRODUCT_CODE  0x0000a72c
#define REVISION      0x00020000

#define NSEC_PER_SEC  1000000000ULL

// PDO extents from your cstruct:
#define OUT_2000_COUNT 200  // 0x2000:01..0x2000:C8
#define OUT_2001_COUNT  50  // 0x2001:01..0x2001:32
#define IN_3000_COUNT  200  // 0x3000:01..0x3000:C8
#define IN_3001_COUNT   50  // 0x3001:01..0x3001:32

// Toggle these if you want to restrict to product filter
#define USE_ID_FILTERS 0

static ec_master_t  *master     = NULL;
static ec_domain_t  *domain_out = NULL; // SM2
static ec_domain_t  *domain_in  = NULL; // SM3
static ec_slave_config_t *sc    = NULL;

static uint8_t *pd_out = NULL;
static uint8_t *pd_in  = NULL;

static volatile int run = 1;

static unsigned int off_out_2000[OUT_2000_COUNT];
static unsigned int off_out_2001[OUT_2001_COUNT];
static unsigned int off_in_3000[IN_3000_COUNT];
static unsigned int off_in_3001[IN_3001_COUNT];

static inline void timespec_add(struct timespec *t, uint64_t add_ns) {
    t->tv_nsec += add_ns;
    while (t->tv_nsec >= (long)NSEC_PER_SEC) {
        t->tv_nsec -= NSEC_PER_SEC;
        t->tv_sec++;
    }
}

static void on_signal(int sig) { (void)sig; run = 0; }

static int reg_single(ec_domain_t *dom, unsigned alias, unsigned position,
                      uint32_t vendor, uint32_t product,
                      uint16_t idx, uint8_t subidx, unsigned int *p_off)
{
    ec_pdo_entry_reg_t r = {0};
    r.alias = alias;
    r.position = position;
#if USE_ID_FILTERS
    r.vendor_id = vendor;
    r.product_code = product;
#else
    r.vendor_id = 0;
    r.product_code = 0;
#endif
    r.index    = idx;
    r.subindex = subidx;
    r.offset   = p_off;
    r.bit_position = NULL;

    ec_pdo_entry_reg_t list[2] = { r, {0} };
    int rc = ecrt_domain_reg_pdo_entry_list(dom, list);
    if (rc) {
        fprintf(stderr, "Failed to register PDO entry %04X:%02X (pos=%u): %s\n",
                idx, subidx, position, strerror(-rc));
    }
    return rc;
}

// Register a few entries first to verify mapping, then the rest.
static int register_pdos(ec_domain_t *dom_out, ec_domain_t *dom_in)
{
    int rc;

    printf("Probing PDO presence with a couple of entries...\n");
    // Probe OUT 0x2000:01 and IN 0x3000:01
    rc = reg_single(dom_out, 0, 0, VENDOR_ID, PRODUCT_CODE, 0x2000, 0x01, &off_out_2000[0]);
    if (rc) return rc;
    rc = reg_single(dom_in,  0, 0, VENDOR_ID, PRODUCT_CODE, 0x3000, 0x01, &off_in_3000[0]);
    if (rc) return rc;

    printf("Probe OK. Registering full PDO lists...\n");

    // OUT 0x2000:02..C8
    for (unsigned i = 1; i < OUT_2000_COUNT; ++i) {
        rc = reg_single(dom_out, 0, 0, VENDOR_ID, PRODUCT_CODE, 0x2000, (uint8_t)(i+1), &off_out_2000[i]);
        if (rc) return rc;
    }
    // OUT 0x2001:01..32
    for (unsigned i = 0; i < OUT_2001_COUNT; ++i) {
        rc = reg_single(dom_out, 0, 0, VENDOR_ID, PRODUCT_CODE, 0x2001, (uint8_t)(i+1), &off_out_2001[i]);
        if (rc) return rc;
    }
    // IN 0x3000:02..C8
    for (unsigned i = 1; i < IN_3000_COUNT; ++i) {
        rc = reg_single(dom_in, 0, 0, VENDOR_ID, PRODUCT_CODE, 0x3000, (uint8_t)(i+1), &off_in_3000[i]);
        if (rc) return rc;
    }
    // IN 0x3001:01..32
    for (unsigned i = 0; i < IN_3001_COUNT; ++i) {
        rc = reg_single(dom_in, 0, 0, VENDOR_ID, PRODUCT_CODE, 0x3001, (uint8_t)(i+1), &off_in_3001[i]);
        if (rc) return rc;
    }

    return 0;
}

#define SUB_TO_IDX(si) ((unsigned)((si) - 1))
#define OFF_OUT_2000(si) (off_out_2000[SUB_TO_IDX(si)])
#define OFF_IN_3000(si)  (off_in_3000 [SUB_TO_IDX(si)])

int main(void)
{
    const uint32_t cycle_ns = 10 * 1000 * 1000; // 10 ms
    const uint16_t LOCAL  = 0xAAAA;
    const uint16_t REMOTE = 0xBBBB;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    master = ecrt_request_master(0);
    if (!master) { fprintf(stderr, "Failed to request master.\n"); return -1; }

    domain_out = ecrt_master_create_domain(master);
    domain_in  = ecrt_master_create_domain(master);
    if (!domain_out || !domain_in) {
        fprintf(stderr, "Failed to create domains.\n");
        return -1;
    }

    // Configure slave at position 0; do NOT touch PDO mapping.
    sc = ecrt_master_slave_config(master, 0, 0, VENDOR_ID, PRODUCT_CODE);
    if (!sc) {
        fprintf(stderr, "Failed to get slave configuration.\n");
        return -1;
    }

    // IMPORTANT: Do NOT call ecrt_slave_config_pdos() — device forbids remap.

#if ECRT_VERSION_MAJOR > 1 || (ECRT_VERSION_MAJOR == 1 && ECRT_VERSION_MINOR >= 6)
    if (ecrt_slave_config_dc(sc, 0x0300, cycle_ns, 440000, 0, 0)) {
        fprintf(stderr, "Failed to configure DC.\n");
        return -1;
    }
#else
    ecrt_slave_config_dc(sc, 0x0300, cycle_ns, 440000, 0, 0);
#endif

    // Register PDO entries by position (0), with a probe first.
    if (register_pdos(domain_out, domain_in)) {
        fprintf(stderr, "PDO entry registration failed during probe/full registration.\n");
        return -1;
    }

    if (ecrt_master_activate(master)) {
        fprintf(stderr, "Failed to activate master.\n");
        return -1;
    }

    pd_out = ecrt_domain_data(domain_out);
    pd_in  = ecrt_domain_data(domain_in);
    if (!pd_out || !pd_in) {
        fprintf(stderr, "Failed to get domain PD pointers.\n");
        return -1;
    }

    printf("Domain OUT size: %zu, Domain IN size: %zu\n",
           ecrt_domain_size(domain_out), ecrt_domain_size(domain_in));

    struct timespec wakeup_time;
    clock_gettime(CLOCK_MONOTONIC, &wakeup_time);
    printf("Starting cyclic task...\n");

    ec_domain_state_t ds_out, ds_in;
    ec_master_state_t ms;

    unsigned diag = 0;
    const unsigned max_diag = 100;
    uint16_t loopcounter = 0;

    while (run) {
        timespec_add(&wakeup_time, cycle_ns);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wakeup_time, NULL);

        ecrt_master_receive(master);
        ecrt_domain_process(domain_out);
        ecrt_domain_process(domain_in);

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t app_time = (uint64_t)now.tv_sec * NSEC_PER_SEC + now.tv_nsec;
        ecrt_master_application_time(master, app_time);
        ecrt_master_sync_reference_clock(master);
        ecrt_master_sync_slave_clocks(master);

        // --- Write ---
        uint16_t heartbeat = loopcounter++;
        EC_WRITE_U16(pd_out + OFF_OUT_2000(0x3B), heartbeat);

        uint16_t control = 0;
        if (loopcounter > 1000 && loopcounter < 1010)      control = REMOTE;
        else if (loopcounter > 2000 && loopcounter < 2010) control = LOCAL;
        EC_WRITE_U16(pd_out + OFF_OUT_2000(0x3D), control);

        // --- Read ---
        uint8_t  gen0 = EC_READ_U8 (pd_in + OFF_IN_3000(0x01));
        uint8_t  gen1 = EC_READ_U8 (pd_in + OFF_IN_3000(0x02));
        uint16_t roof = EC_READ_U16(pd_in + OFF_IN_3000(0x05));
        uint16_t l1   = EC_READ_U16(pd_in + OFF_IN_3000(0x09));
        uint16_t l2   = EC_READ_U16(pd_in + OFF_IN_3000(0x0B));

        ecrt_domain_queue(domain_out);
        ecrt_domain_queue(domain_in);
        ecrt_master_send(master);

        if (++diag >= max_diag) {
            diag = 0;
            printf("loop: %u, gen0=0x%02X gen1=0x%02X | fan roof=%u, l1=%u, l2=%u\n",
                   loopcounter, gen0, gen1, roof, l1, l2);
#ifdef DEBUG
            ecrt_domain_state(domain_out, &ds_out);
            ecrt_domain_state(domain_in,  &ds_in);
            ecrt_master_state(master, &ms);
            printf("OUT WKC=%u state=%u | IN WKC=%u state=%u | AL=%u slaves=%u link=%u\n",
                   ds_out.working_counter, ds_out.wc_state,
                   ds_in.working_counter,  ds_in.wc_state,
                   ms.al_states, ms.slaves_responding, ms.link_up);
#endif
        }
    }

    ecrt_release_master(master);
    return 0;
}
