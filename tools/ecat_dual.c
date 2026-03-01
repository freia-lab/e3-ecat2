// ecat_dual.c — two domains (SM2/SM3), CIFX RE/ECS (uniform U8), PDO/SM offsets only
#include <stdio.h>
#include <stdint.h>
#include <signal.h>
#include <time.h>
#include <ecrt.h>
#include "pdo_map.h"

#define NSEC_PER_SEC 1000000000ULL
static inline void timespec_add(struct timespec *t, uint64_t add_ns) {
    t->tv_nsec += add_ns; while (t->tv_nsec >= (long)NSEC_PER_SEC) { t->tv_nsec -= NSEC_PER_SEC; t->tv_sec++; }
}

static volatile int run_loop = 1;
static void on_sig(int){ run_loop = 0; }

int main(void)
{
    const uint32_t cycle_ns = 10 * 1000 * 1000; // 10 ms
    const uint16_t LOCAL  = 0xAAAA, REMOTE = 0xBBBB;

    ec_master_t  *master     = NULL;
    ec_domain_t  *domain_out = NULL; // SM2
    ec_domain_t  *domain_in  = NULL; // SM3
    ec_slave_config_t *sc    = NULL;
    uint8_t *pd_out = NULL, *pd_in = NULL;

    // For your bus: alias=0, position=0
    const uint16_t alias=0, position=0;

    // — Init —
    signal(SIGINT,  on_sig);
    signal(SIGTERM, on_sig);

    master = ecrt_request_master(0);
    if (!master) { fprintf(stderr, "ecrt_request_master failed\n"); return -1; }

    domain_out = ecrt_master_create_domain(master);
    domain_in  = ecrt_master_create_domain(master);
    if (!domain_out || !domain_in) { fprintf(stderr, "create_domain failed\n"); return -1; }

    sc = ecrt_master_slave_config(master, alias, position, DEV_VENDOR_ID, DEV_PRODUCT_CODE);
    if (!sc) { fprintf(stderr, "slave_config failed\n"); return -1; }

#if ECRT_VERSION_MAJOR > 1 || (ECRT_VERSION_MAJOR == 1 && ECRT_VERSION_MINOR >= 6)
    if (ecrt_slave_config_dc(sc, 0x0300, cycle_ns, 440000, 0, 0)) {
        fprintf(stderr, "DC config failed\n"); return -1;
    }
#else
    ecrt_slave_config_dc(sc, 0x0300, cycle_ns, 440000, 0, 0);
#endif

    // — Build “basic” mapping (matches fixed PDOs) and register into two domains —
    pdo_map_t map;
    if (pdo_map_build_and_apply(sc, &map)) { fprintf(stderr, "pdo_map_build_and_apply failed\n"); return -1; }
    if (pdo_map_register(&map, domain_out, domain_in, alias, position)) {
        fprintf(stderr, "pdo_map_register failed\n"); return -1;
    }

    // — Activate and get PD pointers —
    if (ecrt_master_activate(master)) { fprintf(stderr, "activate failed\n"); return -1; }
    pd_out = ecrt_domain_data(domain_out);
    pd_in  = ecrt_domain_data(domain_in);
    if (!pd_out || !pd_in) { fprintf(stderr, "domain data NULL\n"); return -1; }

    printf("Domain OUT size: %zu, Domain IN size: %zu\n",
           ecrt_domain_size(domain_out), ecrt_domain_size(domain_in));

    // — Cyclic task —
    struct timespec wake; clock_gettime(CLOCK_MONOTONIC, &wake);
    printf("Starting cyclic task...\n");
    uint16_t loop=0; unsigned diag=0;

    while (run_loop) {
        timespec_add(&wake, cycle_ns);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wake, NULL);

        ecrt_master_receive(master);
        ecrt_domain_process(domain_out);
        ecrt_domain_process(domain_in);

        // App/DC time
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t app_time = (uint64_t)now.tv_sec * NSEC_PER_SEC + now.tv_nsec;
        ecrt_master_application_time(master, app_time);
        ecrt_master_sync_reference_clock(master);
        ecrt_master_sync_slave_clocks(master);

        // --- Writes (SM2/RxPDO) ---
        EC_WRITE_U16(pd_out + OFF_OUT_2000(&map, 0x3B), loop++); // heartbeat
        uint16_t ctrl = 0;
        if (loop > 1000 && loop < 1010)      ctrl = REMOTE;
        else if (loop > 2000 && loop < 2010) ctrl = LOCAL;
        EC_WRITE_U16(pd_out + OFF_OUT_2000(&map, 0x3D), ctrl);

        // --- Reads (SM3/TxPDO) ---
        uint8_t  gen0 = EC_READ_U8 (pd_in + OFF_IN_3000(&map, 0x01));
        uint8_t  gen1 = EC_READ_U8 (pd_in + OFF_IN_3000(&map, 0x02));
        uint16_t roof = EC_READ_U16(pd_in + OFF_IN_3000(&map, 0x05));
        uint16_t l1   = EC_READ_U16(pd_in + OFF_IN_3000(&map, 0x09));
        uint16_t l2   = EC_READ_U16(pd_in + OFF_IN_3000(&map, 0x0B));

        ecrt_domain_queue(domain_out);
        ecrt_domain_queue(domain_in);
        ecrt_master_send(master);

        if (++diag >= 100) {
            diag = 0;
            printf("loop=%u gen0=0x%02X gen1=0x%02X roof=%u l1=%u l2=%u\n",
                   loop, gen0, gen1, roof, l1, l2);
        }
    }

    pdo_map_free(&map);
    ecrt_release_master(master);
    return 0;
}
