// pdo_map.c — minimal implementation for CIFX RE/ECS (uniform U8 ranges, fixed mapping)
#include "pdo_map.h"

int pdo_map_build_and_apply(ec_slave_config_t *sc, pdo_map_t *m)
{
    if (!sc || !m) return -1;
    memset(m, 0, sizeof(*m));

    // Allocate uniform U8 sequences
    m->rx1600 = calloc(OUT_2000_COUNT, sizeof(*m->rx1600));
    m->rx1601 = calloc(OUT_2001_COUNT, sizeof(*m->rx1601));
    m->tx1a00 = calloc(IN_3000_COUNT,  sizeof(*m->tx1a00));
    m->tx1a01 = calloc(IN_3001_COUNT,  sizeof(*m->tx1a01));
    if (!m->rx1600 || !m->rx1601 || !m->tx1a00 || !m->tx1a01) return -1;

    for (unsigned i=0;i<OUT_2000_COUNT;++i) m->rx1600[i] = (ec_pdo_entry_info_t){0x2000,(uint8_t)(i+1),8};
    for (unsigned i=0;i<OUT_2001_COUNT;++i) m->rx1601[i] = (ec_pdo_entry_info_t){0x2001,(uint8_t)(i+1),8};
    for (unsigned i=0;i<IN_3000_COUNT; ++i) m->tx1a00[i] = (ec_pdo_entry_info_t){0x3000,(uint8_t)(i+1),8};
    for (unsigned i=0;i<IN_3001_COUNT; ++i) m->tx1a01[i] = (ec_pdo_entry_info_t){0x3001,(uint8_t)(i+1),8};

    m->rx_pdos[0] = (ec_pdo_info_t){0x1600, OUT_2000_COUNT, m->rx1600};
    m->rx_pdos[1] = (ec_pdo_info_t){0x1601, OUT_2001_COUNT, m->rx1601};
    m->tx_pdos[0] = (ec_pdo_info_t){0x1A00, IN_3000_COUNT,  m->tx1a00};
    m->tx_pdos[1] = (ec_pdo_info_t){0x1A01, IN_3001_COUNT,  m->tx1a01};

    // SM0/1 mailbox, SM2 outputs, SM3 inputs (+terminator)
    m->syncs = calloc(5, sizeof(*m->syncs));
    if (!m->syncs) return -1;
    m->syncs[0] = (ec_sync_info_t){0, EC_DIR_OUTPUT, 0, NULL, EC_WD_DISABLE};
    m->syncs[1] = (ec_sync_info_t){1, EC_DIR_INPUT,  0, NULL, EC_WD_DISABLE};
    m->syncs[2] = (ec_sync_info_t){2, EC_DIR_OUTPUT, 2, m->rx_pdos, EC_WD_ENABLE};
    m->syncs[3] = (ec_sync_info_t){3, EC_DIR_INPUT,  2, m->tx_pdos, EC_WD_DISABLE};
    m->syncs[4] = (ec_sync_info_t){0xff};

    // Inform master about the exact mapping (no actual remap if identical)
    if (ecrt_slave_config_pdos(sc, EC_END, m->syncs)) return -1;

    return 0;
}

int pdo_map_register(pdo_map_t *m,
                     ec_domain_t *domain_out, ec_domain_t *domain_in,
                     uint16_t alias, uint16_t position)
{
    if (!m || !domain_out || !domain_in) return -1;

    // OUT (0x2000 + 0x2001)
    size_t n_out = OUT_2000_COUNT + OUT_2001_COUNT;
    m->regs_out = calloc(n_out + 1, sizeof(*m->regs_out));
    if (!m->regs_out) return -1;

    size_t k = 0;
    for (unsigned i=0;i<OUT_2000_COUNT;++i)
        m->regs_out[k++] = (ec_pdo_entry_reg_t){
            .alias=alias, .position=position,
            .vendor_id=DEV_VENDOR_ID, .product_code=DEV_PRODUCT_CODE,
            .index=0x2000, .subindex=(uint8_t)(i+1),
            .offset=&m->off_out_2000[i], .bit_position=NULL
        };
    for (unsigned i=0;i<OUT_2001_COUNT;++i)
        m->regs_out[k++] = (ec_pdo_entry_reg_t){
            .alias=alias, .position=position,
            .vendor_id=DEV_VENDOR_ID, .product_code=DEV_PRODUCT_CODE,
            .index=0x2001, .subindex=(uint8_t)(i+1),
            .offset=&m->off_out_2001[i], .bit_position=NULL
        };
    m->regs_out[k] = (ec_pdo_entry_reg_t){0};
    if (ecrt_domain_reg_pdo_entry_list(domain_out, m->regs_out)) return -1;

    // IN (0x3000 + 0x3001)
    size_t n_in = IN_3000_COUNT + IN_3001_COUNT;
    m->regs_in = calloc(n_in + 1, sizeof(*m->regs_in));
    if (!m->regs_in) return -1;

    k = 0;
    for (unsigned i=0;i<IN_3000_COUNT;++i)
        m->regs_in[k++] = (ec_pdo_entry_reg_t){
            .alias=alias, .position=position,
            .vendor_id=DEV_VENDOR_ID, .product_code=DEV_PRODUCT_CODE,
            .index=0x3000, .subindex=(uint8_t)(i+1),
            .offset=&m->off_in_3000[i], .bit_position=NULL
        };
    for (unsigned i=0;i<IN_3001_COUNT;++i)
        m->regs_in[k++] = (ec_pdo_entry_reg_t){
            .alias=alias, .position=position,
            .vendor_id=DEV_VENDOR_ID, .product_code=DEV_PRODUCT_CODE,
            .index=0x3001, .subindex=(uint8_t)(i+1),
            .offset=&m->off_in_3001[i], .bit_position=NULL
        };
    m->regs_in[k] = (ec_pdo_entry_reg_t){0};
    if (ecrt_domain_reg_pdo_entry_list(domain_in, m->regs_in)) return -1;

    return 0;
}

void pdo_map_free(pdo_map_t *m)
{
    if (!m) return;
    free(m->rx1600); free(m->rx1601); free(m->tx1a00); free(m->tx1a01);
    free(m->syncs);  free(m->regs_out); free(m->regs_in);
    memset(m, 0, sizeof(*m));
}
