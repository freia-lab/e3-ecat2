// pdo_map.h — minimal helper for CIFX RE/ECS (uniform U8 ranges, fixed mapping)
#pragma once
#include <ecrt.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Device IDs (keep strict to avoid mismatches)
#define DEV_VENDOR_ID     0x0000006c
#define DEV_PRODUCT_CODE  0x0000a72c

// Default PDO extents from your device:
//   Rx: 0x1600 (200 x 8-bit, 0x2000:01..C8) + 0x1601 (50 x 8-bit, 0x2001:01..32)
//   Tx: 0x1A00 (200 x 8-bit, 0x3000:01..C8) + 0x1A01 (50 x 8-bit, 0x3001:01..32)
#define OUT_2000_COUNT 200
#define OUT_2001_COUNT  50
#define IN_3000_COUNT  200
#define IN_3001_COUNT   50

typedef struct {
    // PDO entry arrays (kept alive until after activation)
    ec_pdo_entry_info_t *rx1600, *rx1601; // outputs
    ec_pdo_entry_info_t *tx1a00, *tx1a01; // inputs
    ec_pdo_info_t rx_pdos[2];
    ec_pdo_info_t tx_pdos[2];
    ec_sync_info_t *syncs;

    // Registration lists (NULL‑terminated arrays)
    ec_pdo_entry_reg_t *regs_out;
    ec_pdo_entry_reg_t *regs_in;

    // Offsets for each entry (domain-local byte offsets)
    unsigned int off_out_2000[OUT_2000_COUNT];
    unsigned int off_out_2001[OUT_2001_COUNT];
    unsigned int off_in_3000[IN_3000_COUNT];
    unsigned int off_in_3001[IN_3001_COUNT];
} pdo_map_t;

// Build mapping (matches device’s fixed PDOs) and inform master config.
// Returns 0 on success, non‑zero on failure.
int pdo_map_build_and_apply(ec_slave_config_t *sc, pdo_map_t *m);

// Register PDO entries into two domains (SM2 → domain_out, SM3 → domain_in).
// Returns 0 on success, non‑zero on failure.
int pdo_map_register(pdo_map_t *m,
                     ec_domain_t *domain_out, ec_domain_t *domain_in,
                     uint16_t alias, uint16_t position);

// Free heap allocations inside the map (safe at shutdown).
void pdo_map_free(pdo_map_t *m);

// Access helpers (subindex is 1‑based, same as your device)
#define SUB_TO_IDX(si) ((unsigned)((si)-1))
#define OFF_OUT_2000(m, si) ((m)->off_out_2000[SUB_TO_IDX(si)])
#define OFF_IN_3000(m, si)  ((m)->off_in_3000 [SUB_TO_IDX(si)])
