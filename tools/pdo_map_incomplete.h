#pragma once
#include <ecrt.h>

/* Vendor/Product (you confirmed these) */
#define VENDOR_ID     0x0000006c
#define PRODUCT_CODE  0x0000a72c

/* Position in the bus (change if your slave is not at 0) */
#define SLAVE_POS     0

/* -------------------------------------------------------------------------
   Paste the four PDO entry lists from your 1.5.2 `ethercat cstruct` here.
   IMPORTANT: Keep the exact order printed by the cstruct for each PDO.

   Example syntax for each line:
       {0x3000, 0x01, 8}, {0x3000, 0x02, 16}, ...

   DO NOT add or remove entries. DO NOT change bit lengths.
   ------------------------------------------------------------------------- */

/* ---- RxPDO 0x1600 entries (PASTE EXACTLY from cstruct) ---- */
static const ec_pdo_entry_info_t RXPDO_1600_ENTRIES[] = {
    /* TODO: paste your 0x1600 entry list here */
};

/* ---- RxPDO 0x1601 entries (PASTE EXACTLY from cstruct) ---- */
static const ec_pdo_entry_info_t RXPDO_1601_ENTRIES[] = {
    /* TODO: paste your 0x1601 entry list here */
};

/* ---- TxPDO 0x1A00 entries (PASTE EXACTLY from cstruct) ---- */
static const ec_pdo_entry_info_t TXPDO_1A00_ENTRIES[] = {
    /* TODO: paste your 0x1A00 entry list here */
};

/* ---- TxPDO 0x1A01 entries (PASTE EXACTLY from cstruct) ---- */
static const ec_pdo_entry_info_t TXPDO_1A01_ENTRIES[] = {
    /* TODO: paste your 0x1A01 entry list here */
};

/* -------------------------------------------------------------------------
   Derive counts automatically from the arrays you pasted.
   ------------------------------------------------------------------------- */
#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

enum {
    RXPDO_1600_COUNT = ARRAY_LEN(RXPDO_1600_ENTRIES),
    RXPDO_1601_COUNT = ARRAY_LEN(RXPDO_1601_ENTRIES),
    TXPDO_1A00_COUNT = ARRAY_LEN(TXPDO_1A00_ENTRIES),
    TXPDO_1A01_COUNT = ARRAY_LEN(TXPDO_1A01_ENTRIES),
};

/* Optional compile-time guard if your compiler supports C11 */
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(RXPDO_1600_COUNT > 0, "0x1600 entries missing");
_Static_assert(RXPDO_1601_COUNT > 0, "0x1601 entries missing");
_Static_assert(TXPDO_1A00_COUNT > 0, "0x1A00 entries missing");
_Static_assert(TXPDO_1A01_COUNT > 0, "0x1A01 entries missing");
#endif

/* -------------------------------------------------------------------------
   Build the PDOs and Sync Manager description using your arrays.
   ------------------------------------------------------------------------- */
static const ec_pdo_info_t RX_PDOS[] = {
    { 0x1600, RXPDO_1600_COUNT, RXPDO_1600_ENTRIES },
    { 0x1601, RXPDO_1601_COUNT, RXPDO_1601_ENTRIES },
};

static const ec_pdo_info_t TX_PDOS[] = {
    { 0x1A00, TXPDO_1A00_COUNT, TXPDO_1A00_ENTRIES },
    { 0x1A01, TXPDO_1A01_COUNT, TXPDO_1A01_ENTRIES },
};

/*  SM0/SM1 = mailbox; SM2=RxPDOs; SM3=TxPDOs  */
static const ec_sync_info_t SLAVE_SYNC_INFO[] = {
    { 0, EC_DIR_OUTPUT, 0, NULL,          EC_WD_DISABLE },
    { 1, EC_DIR_INPUT,  0, NULL,          EC_WD_DISABLE },
    { 2, EC_DIR_OUTPUT, (uint8_t)(sizeof(RX_PDOS)/sizeof(RX_PDOS[0])),
                        RX_PDOS,          EC_WD_DISABLE },
    { 3, EC_DIR_INPUT,  (uint8_t)(sizeof(TX_PDOS)/sizeof(TX_PDOS[0])),
                        TX_PDOS,          EC_WD_DISABLE },
    { 0xFF, 0, 0, NULL, EC_WD_DISABLE }
};

/* -------------------------------------------------------------------------
   Helper to register all entries in a consistent order:
   RX first (SM2), then TX (SM3). This ensures TX offsets follow RX offsets.
   ------------------------------------------------------------------------- */
static inline int build_entry_regs(ec_pdo_entry_reg_t **out_regs,
                                   unsigned **out_offsets, int *out_total)
{
    /* Count total entries */
    int total = 0;
    for (int p=0; p<(int)(sizeof(RX_PDOS)/sizeof(RX_PDOS[0])); p++)
        total += RX_PDOS[p].n_entries;
    for (int p=0; p<(int)(sizeof(TX_PDOS)/sizeof(TX_PDOS[0])); p++)
        total += TX_PDOS[p].n_entries;

    ec_pdo_entry_reg_t *regs = calloc((size_t)total + 1, sizeof(*regs));
    unsigned *offs = calloc((size_t)total, sizeof(*offs));
    if (!regs || !offs) return -1;

    int k = 0;

    /* RX entries */
    for (int p=0; p<(int)(sizeof(RX_PDOS)/sizeof(RX_PDOS[0])); p++) {
        const ec_pdo_info_t *pdo = &RX_PDOS[p];
        for (int e=0; e<pdo->n_entries; e++, k++) {
            regs[k].alias        = 0;
            regs[k].position     = SLAVE_POS;
            regs[k].vendor_id    = VENDOR_ID;
            regs[k].product_code = PRODUCT_CODE;
            regs[k].index        = pdo->entries[e].index;
            regs[k].subindex     = pdo->entries[e].subindex;
            regs[k].offset       = &offs[k];
        }
    }

    /* TX entries */
    for (int p=0; p<(int)(sizeof(TX_PDOS)/sizeof(TX_PDOS[0])); p++) {
        const ec_pdo_info_t *pdo = &TX_PDOS[p];
        for (int e=0; e<pdo->n_entries; e++, k++) {
            regs[k].alias        = 0;
            regs[k].position     = SLAVE_POS;
            regs[k].vendor_id    = VENDOR_ID;
            regs[k].product_code = PRODUCT_CODE;
            regs[k].index        = pdo->entries[e].index;
            regs[k].subindex     = pdo->entries[e].subindex;
            regs[k].offset       = &offs[k];
        }
    }

    regs[k] = (ec_pdo_entry_reg_t){0}; /* terminator */

    *out_regs    = regs;
    *out_offsets = offs;
    *out_total   = total;
    return 0;
}

/* Helper: total RX entries only (to find first TX offset later) */
static inline int total_rx_entries(void)
{
    int n = 0;
    for (int p=0; p<(int)(sizeof(RX_PDOS)/sizeof(RX_PDOS[0])); p++)
        n += RX_PDOS[p].n_entries;
    return n;
}
