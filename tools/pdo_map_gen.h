#pragma once
#include <stdint.h>
#include <stddef.h>
#include <ecrt.h>

// -------- Profiles you can choose at runtime ----------
typedef enum {
    PDO_PROFILE_BASIC = 0,  // your CIFX mapping: 200+50 OUT, 200+50 IN (all U8)
    PDO_PROFILE_FAN16,      // demo: selected fields as true U16s
    PDO_PROFILE_BITS32      // demo: bit-level entries (1-bit signals)
} pdo_profile_t;

// -------- Named signals your app can reference ----------
typedef enum {
    SIG_HEARTBEAT = 0,      // OUT: heartbeat
    SIG_REMOTE_LOCAL,       // OUT: control word (REMOTE/LOCAL)
    SIG_GEN_STAT0,          // IN : general status 0 (U8)
    SIG_GEN_STAT1,          // IN : general status 1 (U8)
    SIG_ROOF_FAN_SPEED,     // IN : roof fan (often U16)
    SIG_FAN_LEFT1_SPEED,    // IN : left fan 1 (U16)
    SIG_FAN_LEFT2_SPEED,    // IN : left fan 2 (U16)
    SIG__COUNT
} pdo_signal_id_t;

// Direction helper
typedef enum { MAP_DIR_OUT = 0, MAP_DIR_IN = 1 } map_dir_t;

// -------- Map object you manage per application -----------
typedef struct pdo_map_s {
    // Chosen profile
    pdo_profile_t profile;

    // --- Internal: PDO description arrays kept alive until activation ---
    // RxPDO (outputs to slave → SM2)
    ec_pdo_entry_info_t *rx_a_entries; size_t rx_a_count; uint16_t rx_a_pdo_idx; // e.g., 0x1600
    ec_pdo_entry_info_t *rx_b_entries; size_t rx_b_count; uint16_t rx_b_pdo_idx; // e.g., 0x1601
    ec_pdo_info_t rx_pdos[2];

    // TxPDO (inputs from slave → SM3)
    ec_pdo_entry_info_t *tx_a_entries; size_t tx_a_count; uint16_t tx_a_pdo_idx; // e.g., 0x1A00
    ec_pdo_entry_info_t *tx_b_entries; size_t tx_b_count; uint16_t tx_b_pdo_idx; // e.g., 0x1A01
    ec_pdo_info_t tx_pdos[2];

    ec_sync_info_t *syncs; // SM0..SM3 (+ terminator)

    // --- Internal: registration lists for domains (NULL-terminated) ---
    ec_pdo_entry_reg_t *regs_out; size_t regs_out_count;
    ec_pdo_entry_reg_t *regs_in;  size_t regs_in_count;

    // --- Offsets captured by registration (arrays owned by this map) ---
    unsigned int *off_out_a; size_t off_out_a_count; // mirrors rx_a_entries
    unsigned int *off_out_b; size_t off_out_b_count; // mirrors rx_b_entries
    unsigned int *off_in_a;  size_t off_in_a_count;  // mirrors tx_a_entries
    unsigned int *off_in_b;  size_t off_in_b_count;  // mirrors tx_b_entries

    // Optional bit positions (only used for bit-level entries)
    unsigned int *bitpos_out_a; unsigned int *bitpos_out_b;
    unsigned int *bitpos_in_a;  unsigned int *bitpos_in_b;

    // Named signal registry (filled per profile)
    struct {
        const char  *name;
        map_dir_t    dir;
        uint16_t     index;
        uint8_t      sub;
        uint8_t      bits;        // length (8/16/32 or 1 for bit)
        // Resolved after registration:
        unsigned int *offset_ptr; // points into the off_* arrays
        unsigned int *bitpos_ptr; // optional (only for bits)
    } signals[SIG__COUNT];
} pdo_map_t;


// ========= API =============

// Create PDO mapping for a profile and apply to slave config.
// - Does NOT activate master.
// - For 'basic', this mirrors the slave's fixed mapping (no remap attempt).
// - For 'fan16'/'bits32', this configures PDOs (requires slave support).
int pdo_map_create_and_apply(ec_slave_config_t *sc,
                             pdo_profile_t profile,
                             pdo_map_t *out_map);

// Register PDO entries with the two domains and resolve offsets.
// You must pass the addressing you used for ecrt_master_slave_config.
int pdo_map_register_domains(pdo_map_t *map,
                             ec_domain_t *domain_out, ec_domain_t *domain_in,
                             uint16_t alias, uint16_t position,
                             uint32_t vendor_id, uint32_t product_code);

// Resolve a named signal (offset + optional bit-position + bit-length).
// Returns 0 on success; non-zero if the signal isn't present in this profile.
int pdo_map_get_signal(const pdo_map_t *map, pdo_signal_id_t id,
                       map_dir_t *dir, unsigned int **offset_ptr,
                       unsigned int **bitpos_ptr, uint8_t *bits);

// Free all heap allocations inside the map (safe to call once after shutdown).
void pdo_map_free(pdo_map_t *map);
