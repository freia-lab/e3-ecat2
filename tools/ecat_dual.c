// main.c (extract – focus on initialization + cyclic signal use)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <getopt.h>
#include <ecrt.h>
#include "pdo_map.h"

#define VENDOR_ID     0x0000006c
#define PRODUCT_CODE  0x0000a72c
#define NSEC_PER_SEC  1000000000ULL

static ec_master_t  *master     = NULL;
static ec_domain_t  *domain_out = NULL;
static ec_domain_t  *domain_in  = NULL;
static ec_slave_config_t *sc    = NULL;
static uint8_t *pd_out = NULL, *pd_in = NULL;
static volatile int run_loop = 1;

static inline void timespec_add(struct timespec *t, uint64_t ns) {
    t->tv_nsec += ns; while (t->tv_nsec >= (long)NSEC_PER_SEC) { t->tv_nsec -= NSEC_PER_SEC; t->tv_sec++; }
}
static void on_sig(int s){ (void)s; run_loop = 0; }

static pdo_profile_t parse_profile(const char *name) {
    if (!name) return PDO_PROFILE_BASIC;
    if (!strcmp(name,"basic"))  return PDO_PROFILE_BASIC;
    if (!strcmp(name,"fan16"))  return PDO_PROFILE_FAN16;
    if (!strcmp(name,"bits32")) return PDO_PROFILE_BITS32;
    fprintf(stderr, "Unknown profile '%s', defaulting to basic.\n", name);
    return PDO_PROFILE_BASIC;
}

int main(int argc, char **argv)
{
    const uint32_t cycle_ns = 10 * 1000 * 1000; // 10 ms
    uint16_t alias=0, position=0; // 0:0 by default
    const char *profile_name = "basic";

    // CLI
    static struct option opts[] = {
        {"profile",  required_argument, 0, 'P'},
        {"alias",    required_argument, 0, 'a'},
        {"position", required_argument, 0, 'p'},
        {0,0,0,0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "P:a:p:", opts, NULL)) != -1) {
        if (c=='P') profile_name = optarg;
        else if (c=='a') alias = (uint16_t)strtoul(optarg, NULL, 0);
        else if (c=='p') position = (uint16_t)strtoul(optarg, NULL, 0);
    }
    pdo_profile_t profile = parse_profile(profile_name);
    printf("Profile: %s (alias=%u position=%u)\n", profile_name, alias, position);

    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);

    // Master + domains
    master = ecrt_request_master(0);
    if (!master) { fprintf(stderr, "ecrt_request_master failed\n"); return -1; }
    domain_out = ecrt_master_create_domain(master);
    domain_in  = ecrt_master_create_domain(master);
    if (!domain_out || !domain_in) { fprintf(stderr, "create_domain failed\n"); return -1; }

    // Slave config (strict IDs)
    sc = ecrt_master_slave_config(master, alias, position, VENDOR_ID, PRODUCT_CODE);
    if (!sc) { fprintf(stderr, "slave_config failed\n"); return -1; }

#if ECRT_VERSION_MAJOR > 1 || (ECRT_VERSION_MAJOR == 1 && ECRT_VERSION_MINOR >= 6)
    if (ecrt_slave_config_dc(sc, 0x0300, cycle_ns, 440000, 0, 0)) {
        fprintf(stderr, "DC config failed\n"); return -1;
    }
#else
    ecrt_slave_config_dc(sc, 0x0300, cycle_ns, 440000, 0, 0);
#endif

    // === Build & apply PDOs for chosen profile, then register domains ===
    pdo_map_t map;
    if (pdo_map_create_and_apply(sc, profile, &map)) return -1;
    if (pdo_map_register_domains(&map, domain_out, domain_in, alias, position, VENDOR_ID, PRODUCT_CODE)) return -1;

    // Activate, get PD pointers
    if (ecrt_master_activate(master)) { fprintf(stderr, "activate failed\n"); return -1; }
    pd_out = ecrt_domain_data(domain_out);
    pd_in  = ecrt_domain_data(domain_in);
    if (!pd_out || !pd_in) { fprintf(stderr, "domain data NULL\n"); return -1; }

    printf("Domain OUT size: %zu, Domain IN size: %zu\n",
           ecrt_domain_size(domain_out), ecrt_domain_size(domain_in));

    // Resolve named signal offsets once (works for all profiles)
    map_dir_t dir;
    unsigned int *off_hb, *off_ctrl, *off_g0, *off_g1, *off_roof, *off_l1, *off_l2;
    unsigned int *bp_dummy; uint8_t bits;

    if (pdo_map_get_signal(&map, SIG_HEARTBEAT, &dir, &off_hb, &bp_dummy, &bits)) off_hb=NULL;
    if (pdo_map_get_signal(&map, SIG_REMOTE_LOCAL, &dir, &off_ctrl, &bp_dummy, &bits)) off_ctrl=NULL;
    if (pdo_map_get_signal(&map, SIG_GEN_STAT0, &dir, &off_g0, &bp_dummy, &bits)) off_g0=NULL;
    if (pdo_map_get_signal(&map, SIG_GEN_STAT1, &dir, &off_g1, &bp_dummy, &bits)) off_g1=NULL;
    if (pdo_map_get_signal(&map, SIG_ROOF_FAN_SPEED, &dir, &off_roof, &bp_dummy, &bits)) off_roof=NULL;
    if (pdo_map_get_signal(&map, SIG_FAN_LEFT1_SPEED,&dir, &off_l1, &bp_dummy, &bits)) off_l1=NULL;
    if (pdo_map_get_signal(&map, SIG_FAN_LEFT2_SPEED,&dir, &off_l2, &bp_dummy, &bits)) off_l2=NULL;

    // Cyclic
    struct timespec wake; clock_gettime(CLOCK_MONOTONIC, &wake);
    printf("Starting cyclic task...\n");
    uint16_t loop=0; unsigned diag=0;

    while (run_loop) {
        timespec_add(&wake, cycle_ns);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wake, NULL);

        ecrt_master_receive(master);
        ecrt_domain_process(domain_out);
        ecrt_domain_process(domain_in);

        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t app_time = (uint64_t)now.tv_sec * NSEC_PER_SEC + now.tv_nsec;
        ecrt_master_application_time(master, app_time);
        ecrt_master_sync_reference_clock(master);
        ecrt_master_sync_slave_clocks(master);

        // --- Writes ---
        if (off_hb)   EC_WRITE_U16(pd_out + *off_hb, loop++);
        if (off_ctrl) {
            uint16_t ctrl = 0;
            if (loop > 1000 && loop < 1010)      ctrl = 0xBBBB;
            else if (loop > 2000 && loop < 2010) ctrl = 0xAAAA;
            EC_WRITE_U16(pd_out + *off_ctrl, ctrl);
        }

        // --- Reads ---
        uint8_t  gen0=0, gen1=0; uint16_t roof=0,l1=0,l2=0;
        if (off_g0)   gen0 = EC_READ_U8 (pd_in + *off_g0);
        if (off_g1)   gen1 = EC_READ_U8 (pd_in + *off_g1);
        if (off_roof) roof = EC_READ_U16(pd_in + *off_roof);
        if (off_l1)   l1   = EC_READ_U16(pd_in + *off_l1);
        if (off_l2)   l2   = EC_READ_U16(pd_in + *off_l2);

        ecrt_domain_queue(domain_out);
        ecrt_domain_queue(domain_in);
        ecrt_master_send(master);

        if (++diag >= 100) {
            diag = 0;
            printf("[p=%s] loop=%u gen0=0x%02X gen1=0x%02X roof=%u l1=%u l2=%u\n",
                   profile_name, loop, gen0, gen1, roof, l1, l2);
        }
    }

    pdo_map_free(&map);
    ecrt_release_master(master);
    return 0;
}
