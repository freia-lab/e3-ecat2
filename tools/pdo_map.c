#include "pdo_map.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ---- tiny helpers -----------------------------------------------------------
static ec_pdo_entry_info_t*
mk_entries_seq(uint16_t idx, uint8_t first_sub, unsigned count, unsigned bits) {
    ec_pdo_entry_info_t *a = calloc(count, sizeof(*a));
    if (!a) return NULL;
    for (unsigned i = 0; i < count; ++i)
        a[i] = (ec_pdo_entry_info_t){ idx, (uint8_t)(first_sub + i), (uint8_t)bits };
    return a;
}
static ec_pdo_entry_info_t*
mk_entries_list(const uint16_t *idx, const uint8_t *sub, const uint8_t *bits, unsigned n) {
    ec_pdo_entry_info_t *a = calloc(n, sizeof(*a));
    if (!a) return NULL;
    for (unsigned i = 0; i < n; ++i)
        a[i] = (ec_pdo_entry_info_t){ idx[i], sub[i], bits[i] };
    return a;
}
static ec_sync_info_t*
mk_syncs_2x2(const ec_pdo_info_t *rx, unsigned rx_n,
             const ec_pdo_info_t *tx, unsigned tx_n,
             ec_watchdog_mode_t wd_rx, ec_watchdog_mode_t wd_tx) {
    ec_sync_info_t *s = calloc(5, sizeof(*s));
    if (!s) return NULL;
    s[0] = (ec_sync_info_t){0, EC_DIR_OUTPUT, 0, NULL, EC_WD_DISABLE};
    s[1] = (ec_sync_info_t){1, EC_DIR_INPUT,  0, NULL, EC_WD_DISABLE};
    s[2] = (ec_sync_info_t){2, EC_DIR_OUTPUT, rx_n, (ec_pdo_info_t*)rx, wd_rx};
    s[3] = (ec_sync_info_t){3, EC_DIR_INPUT,  tx_n, (ec_pdo_info_t*)tx, wd_tx};
    s[4] = (ec_sync_info_t){0xff};
    return s;
}
static int reg_append(ec_pdo_entry_reg_t *dst, size_t *k,
                      uint16_t alias, uint16_t position,
                      uint32_t vendor, uint32_t product,
                      uint16_t index, uint8_t sub,
                      unsigned int *off, unsigned int *bitpos) {
    dst[*k] = (ec_pdo_entry_reg_t){
        .alias=alias, .position=position,
        .vendor_id=vendor, .product_code=product,
        .index=index, .subindex=sub, .offset=off,
        .bit_position=bitpos
    };
    (*k)++;
    return 0;
}
static void* zcalloc(size_t n, size_t sz) {
    void *p = calloc(n, sz);
    if (p) memset(p, 0, n*sz);
    return p;
}

// ---- profile builders --------------------------------------------------------

// BASIC: conforms to CIFX fixed layout: all U8, 0x1600(200) + 0x1601(50), 0x1A00(200) + 0x1A01(50)
static int build_basic(ec_slave_config_t *sc, pdo_map_t *m) {
    m->rx_a_pdo_idx = 0x1600; m->rx_b_pdo_idx = 0x1601;
    m->tx_a_pdo_idx = 0x1A00; m->tx_b_pdo_idx = 0x1A01;

    // entries
    m->rx_a_count = 200; m->rx_b_count = 50;
    m->tx_a_count = 200; m->tx_b_count = 50;

    m->rx_a_entries = mk_entries_seq(0x2000, 0x01, m->rx_a_count, 8);
    m->rx_b_entries = mk_entries_seq(0x2001, 0x01, m->rx_b_count, 8);
    m->tx_a_entries = mk_entries_seq(0x3000, 0x01, m->tx_a_count, 8);
    m->tx_b_entries = mk_entries_seq(0x3001, 0x01, m->tx_b_count, 8);
    if (!m->rx_a_entries || !m->rx_b_entries || !m->tx_a_entries || !m->tx_b_entries) return -1;

    m->rx_pdos[0] = (ec_pdo_info_t){ m->rx_a_pdo_idx, (unsigned)m->rx_a_count, m->rx_a_entries };
    m->rx_pdos[1] = (ec_pdo_info_t){ m->rx_b_pdo_idx, (unsigned)m->rx_b_count, m->rx_b_entries };
    m->tx_pdos[0] = (ec_pdo_info_t){ m->tx_a_pdo_idx, (unsigned)m->tx_a_count, m->tx_a_entries };
    m->tx_pdos[1] = (ec_pdo_info_t){ m->tx_b_pdo_idx, (unsigned)m->tx_b_count, m->tx_b_entries };

    m->syncs = mk_syncs_2x2(m->rx_pdos, 2, m->tx_pdos, 2, EC_WD_ENABLE, EC_WD_DISABLE);
    if (!m->syncs) return -1;

    // Named signals (indices as used in your working app)
    m->signals[SIG_HEARTBEAT]       = (typeof(m->signals[0])){ "heartbeat",    MAP_DIR_OUT, 0x2000, 0x3B, 16, NULL, NULL };
    m->signals[SIG_REMOTE_LOCAL]    = (typeof(m->signals[0])){ "remote_local", MAP_DIR_OUT, 0x2000, 0x3D, 16, NULL, NULL };
    m->signals[SIG_GEN_STAT0]       = (typeof(m->signals[0])){ "gen_stat0",    MAP_DIR_IN,  0x3000, 0x01,  8, NULL, NULL };
    m->signals[SIG_GEN_STAT1]       = (typeof(m->signals[0])){ "gen_stat1",    MAP_DIR_IN,  0x3000, 0x02,  8, NULL, NULL };
    m->signals[SIG_ROOF_FAN_SPEED]  = (typeof(m->signals[0])){ "roof_fan",     MAP_DIR_IN,  0x3000, 0x05, 16, NULL, NULL };
    m->signals[SIG_FAN_LEFT1_SPEED] = (typeof(m->signals[0])){ "fan_left1",    MAP_DIR_IN,  0x3000, 0x09, 16, NULL, NULL };
    m->signals[SIG_FAN_LEFT2_SPEED] = (typeof(m->signals[0])){ "fan_left2",    MAP_DIR_IN,  0x3000, 0x0B, 16, NULL, NULL };

    return 0;
}

// FAN16: demo profile using true 16-bit entries for the same logical signals.
// (Requires a slave that allows PDO configuration mapping these fields as 16-bit.)
static int build_fan16(ec_slave_config_t *sc, pdo_map_t *m) {
    m->rx_a_pdo_idx = 0x1600; m->rx_b_pdo_idx = 0; // single PDO in this demo
    m->tx_a_pdo_idx = 0x1A00; m->tx_b_pdo_idx = 0;

    // Rx (outputs): heartbeat (0x2000:3B, 16), remote/local (0x2000:3D, 16)
    {
        const uint16_t idx[] = {0x2000, 0x2000};
        const uint8_t  sub[] = {0x3B,   0x3D};
        const uint8_t  bits[]= {16,     16};
        m->rx_a_count   = 2; m->rx_b_count = 0;
        m->rx_a_entries = mk_entries_list(idx, sub, bits, 2);
        m->rx_b_entries = NULL;
    }
    // Tx (inputs): roof, left1, left2 as 16-bit
    {
        const uint16_t idx[] = {0x3000, 0x3000, 0x3000};
        const uint8_t  sub[] = {0x05,   0x09,   0x0B  };
        const uint8_t  bits[]= {16,     16,     16    };
        m->tx_a_count   = 3; m->tx_b_count = 0;
        m->tx_a_entries = mk_entries_list(idx, sub, bits, 3);
        m->tx_b_entries = NULL;
    }
    if (!m->rx_a_entries || !m->tx_a_entries) return -1;

    m->rx_pdos[0] = (ec_pdo_info_t){ m->rx_a_pdo_idx, (unsigned)m->rx_a_count, m->rx_a_entries };
    m->tx_pdos[0] = (ec_pdo_info_t){ m->tx_a_pdo_idx, (unsigned)m->tx_a_count, m->tx_a_entries };
    m->rx_pdos[1] = (ec_pdo_info_t){0}; m->tx_pdos[1] = (ec_pdo_info_t){0};

    m->syncs = mk_syncs_2x2(m->rx_pdos, 1, m->tx_pdos, 1, EC_WD_ENABLE, EC_WD_DISABLE);
    if (!m->syncs) return -1;

    m->signals[SIG_HEARTBEAT]       = (typeof(m->signals[0])){ "heartbeat",    MAP_DIR_OUT, 0x2000, 0x3B, 16, NULL, NULL };
    m->signals[SIG_REMOTE_LOCAL]    = (typeof(m->signals[0])){ "remote_local", MAP_DIR_OUT, 0x2000, 0x3D, 16, NULL, NULL };
    m->signals[SIG_GEN_STAT0]       = (typeof(m->signals[0])){ "gen_stat0",    MAP_DIR_IN,  0x3000, 0x01,  8, NULL, NULL }; // (optional)
    m->signals[SIG_GEN_STAT1]       = (typeof(m->signals[0])){ "gen_stat1",    MAP_DIR_IN,  0x3000, 0x02,  8, NULL, NULL };
    m->signals[SIG_ROOF_FAN_SPEED]  = (typeof(m->signals[0])){ "roof_fan",     MAP_DIR_IN,  0x3000, 0x05, 16, NULL, NULL };
    m->signals[SIG_FAN_LEFT1_SPEED] = (typeof(m->signals[0])){ "fan_left1",    MAP_DIR_IN,  0x3000, 0x09, 16, NULL, NULL };
    m->signals[SIG_FAN_LEFT2_SPEED] = (typeof(m->signals[0])){ "fan_left2",    MAP_DIR_IN,  0x3000, 0x0B, 16, NULL, NULL };

    return 0;
}

// BITS32: demo profile with some 1-bit entries (e.g., digital status)
// (Requires a slave/ESI that defines 1-bit PDO entries.)
static int build_bits32(ec_slave_config_t *sc, pdo_map_t *m) {
    m->rx_a_pdo_idx = 0x1600; m->rx_b_pdo_idx = 0x1601;
    m->tx_a_pdo_idx = 0x1A00; m->tx_b_pdo_idx = 0x1A01;

    // Rx: keep heartbeat/remote as U16 in 1600; nothing in 1601
    {
        const uint16_t idx[] = {0x2000, 0x2000};
        const uint8_t  sub[] = {0x3B,   0x3D};
        const uint8_t  bits[]= {16,     16};
        m->rx_a_count   = 2;  m->rx_b_count = 0;
        m->rx_a_entries = mk_entries_list(idx, sub, bits, 2);
        m->rx_b_entries = NULL;
    }
    // Tx: first PDO has bit-level flags at 0x3005:01..0x3005:08 (1-bit each),
    //     second PDO holds three 16-bit fan speeds (roof/left1/left2).
    {
        const unsigned n_bits = 8;
        uint16_t idx_bits[8]; uint8_t sub_bits[8]; uint8_t len_bits[8];
        for (unsigned i=0;i<n_bits;++i){ idx_bits[i]=0x3005; sub_bits[i]=(uint8_t)(1+i); len_bits[i]=1; }
        m->tx_a_entries = mk_entries_list(idx_bits, sub_bits, len_bits, n_bits);
        m->tx_a_count   = n_bits;

        const uint16_t idx16[] = {0x3000, 0x3000, 0x3000};
        const uint8_t  sub16[] = {0x05,   0x09,   0x0B  };
        const uint8_t  len16[] = {16,     16,     16    };
        m->tx_b_entries = mk_entries_list(idx16, sub16, len16, 3);
        m->tx_b_count   = 3;
    }
    if (!m->rx_a_entries || !m->tx_a_entries || !m->tx_b_entries) return -1;

    m->rx_pdos[0] = (ec_pdo_info_t){ m->rx_a_pdo_idx, (unsigned)m->rx_a_count, m->rx_a_entries };
    m->rx_pdos[1] = (ec_pdo_info_t){0};
    m->tx_pdos[0] = (ec_pdo_info_t){ m->tx_a_pdo_idx, (unsigned)m->tx_a_count, m->tx_a_entries };
    m->tx_pdos[1] = (ec_pdo_info_t){ m->tx_b_pdo_idx, (unsigned)m->tx_b_count, m->tx_b_entries };

    m->syncs = mk_syncs_2x2(m->rx_pdos, 1, m->tx_pdos, 2, EC_WD_ENABLE, EC_WD_DISABLE);
    if (!m->syncs) return -1;

    m->signals[SIG_HEARTBEAT]       = (typeof(m->signals[0])){ "heartbeat",    MAP_DIR_OUT, 0x2000, 0x3B, 16, NULL, NULL };
    m->signals[SIG_REMOTE_LOCAL]    = (typeof(m->signals[0])){ "remote_local", MAP_DIR_OUT, 0x2000, 0x3D, 16, NULL, NULL };
    m->signals[SIG_GEN_STAT0]       = (typeof(m->signals[0])){ "gen_stat0",    MAP_DIR_IN,  0x3005, 0x01,  1, NULL, NULL };
    m->signals[SIG_GEN_STAT1]       = (typeof(m->signals[0])){ "gen_stat1",    MAP_DIR_IN,  0x3005, 0x02,  1, NULL, NULL };
    m->signals[SIG_ROOF_FAN_SPEED]  = (typeof(m->signals[0])){ "roof_fan",     MAP_DIR_IN,  0x3000, 0x05, 16, NULL, NULL };
    m->signals[SIG_FAN_LEFT1_SPEED] = (typeof(m->signals[0])){ "fan_left1",    MAP_DIR_IN,  0x3000, 0x09, 16, NULL, NULL };
    m->signals[SIG_FAN_LEFT2_SPEED] = (typeof(m->signals[0])){ "fan_left2",    MAP_DIR_IN,  0x3000, 0x0B, 16, NULL, NULL };

    return 0;
}

// ---- API impl ---------------------------------------------------------------

int pdo_map_create_and_apply(ec_slave_config_t *sc,
                             pdo_profile_t profile,
                             pdo_map_t *m)
{
    memset(m, 0, sizeof(*m));
    m->profile = profile;

    int rc;
    switch (profile) {
    case PDO_PROFILE_BASIC: rc = build_basic(sc, m);  break;
    case PDO_PROFILE_FAN16: rc = build_fan16(sc, m);  break;
    case PDO_PROFILE_BITS32:rc = build_bits32(sc, m); break;
    default: return -1;
    }
    if (rc) return rc;

    // Push mapping into the slave config (does not force remap if identical)
    // Note: for BASIC on a fixed-mapping slave, this just informs the master about the map.
    if (ecrt_slave_config_pdos(sc, EC_END, m->syncs)) {
        fprintf(stderr, "ecrt_slave_config_pdos() failed. Check if the slave supports this mapping.\n");
        return -1;
    }

    // Prepare offset arrays for registration
    m->off_out_a = zcalloc(m->rx_a_count, sizeof(unsigned int));
    m->off_out_b = zcalloc(m->rx_b_count, sizeof(unsigned int));
    m->off_in_a  = zcalloc(m->tx_a_count, sizeof(unsigned int));
    m->off_in_b  = zcalloc(m->tx_b_count, sizeof(unsigned int));

    // Bit position arrays only for bit-length entries
    if (m->tx_a_entries) {
        int need_bits = 0;
        for (size_t i=0;i<m->tx_a_count;++i) if (m->tx_a_entries[i].bit_length < 8) { need_bits = 1; break; }
        if (need_bits) m->bitpos_in_a = zcalloc(m->tx_a_count, sizeof(unsigned int));
    }
    if (m->tx_b_entries) {
        int need_bits = 0;
        for (size_t i=0;i<m->tx_b_count;++i) if (m->tx_b_entries[i].bit_length < 8) { need_bits = 1; break; }
        if (need_bits) m->bitpos_in_b = zcalloc(m->tx_b_count, sizeof(unsigned int));
    }

    return 0;
}

int pdo_map_register_domains(pdo_map_t *m,
                             ec_domain_t *domain_out, ec_domain_t *domain_in,
                             uint16_t alias, uint16_t position,
                             uint32_t vendor_id, uint32_t product_code)
{
    // Build OUT list (RxPDO entries)
    size_t n_out = m->rx_a_count + m->rx_b_count;
    m->regs_out = calloc(n_out + 1, sizeof(*m->regs_out));
    if (!m->regs_out) return -1;

    size_t k = 0;
    for (size_t i=0; i<m->rx_a_count; ++i)
        reg_append(m->regs_out, &k, alias, position, vendor_id, product_code,
                   m->rx_a_entries[i].index, m->rx_a_entries[i].subindex,
                   &m->off_out_a[i], NULL);
    for (size_t i=0; i<m->rx_b_count; ++i)
        reg_append(m->regs_out, &k, alias, position, vendor_id, product_code,
                   m->rx_b_entries[i].index, m->rx_b_entries[i].subindex,
                   &m->off_out_b[i], NULL);
    m->regs_out[k] = (ec_pdo_entry_reg_t){};
    m->regs_out_count = k;

    if (ecrt_domain_reg_pdo_entry_list(domain_out, m->regs_out)) {
        fprintf(stderr, "PDO entry registration (OUT) failed.\n");
        return -1;
    }

    // Build IN list (TxPDO entries)
    size_t n_in = m->tx_a_count + m->tx_b_count;
    m->regs_in = calloc(n_in + 1, sizeof(*m->regs_in));
    if (!m->regs_in) return -1;

    k = 0;
    for (size_t i=0; i<m->tx_a_count; ++i)
        reg_append(m->regs_in, &k, alias, position, vendor_id, product_code,
                   m->tx_a_entries[i].index, m->tx_a_entries[i].subindex,
                   &m->off_in_a[i],
                   (m->bitpos_in_a ? &m->bitpos_in_a[i] : NULL));
    for (size_t i=0; i<m->tx_b_count; ++i)
        reg_append(m->regs_in, &k, alias, position, vendor_id, product_code,
                   m->tx_b_entries[i].index, m->tx_b_entries[i].subindex,
                   &m->off_in_b[i],
                   (m->bitpos_in_b ? &m->bitpos_in_b[i] : NULL));
    m->regs_in[k] = (ec_pdo_entry_reg_t){};
    m->regs_in_count = k;

    if (ecrt_domain_reg_pdo_entry_list(domain_in, m->regs_in)) {
        fprintf(stderr, "PDO entry registration (IN) failed.\n");
        return -1;
    }

    // Resolve named signals → pointers into the offset/bitpos arrays
    for (int s = 0; s < SIG__COUNT; ++s) {
        if (!m->signals[s].name) continue;
        const map_dir_t dir = m->signals[s].dir;
        const uint16_t ix   = m->signals[s].index;
        const uint8_t  sb   = m->signals[s].sub;
        // linear scan (counts are small for demo; you can index by map if needed)
        if (dir == MAP_DIR_OUT) {
            for (size_t i=0;i<m->rx_a_count;++i)
                if (m->rx_a_entries[i].index==ix && m->rx_a_entries[i].subindex==sb) {
                    m->signals[s].offset_ptr = &m->off_out_a[i]; break;
                }
            if (!m->signals[s].offset_ptr)
            for (size_t i=0;i<m->rx_b_count;++i)
                if (m->rx_b_entries[i].index==ix && m->rx_b_entries[i].subindex==sb) {
                    m->signals[s].offset_ptr = &m->off_out_b[i]; break;
                }
        } else {
            for (size_t i=0;i<m->tx_a_count;++i)
                if (m->tx_a_entries[i].index==ix && m->tx_a_entries[i].subindex==sb) {
                    m->signals[s].offset_ptr = &m->off_in_a[i];
                    if (m->bitpos_in_a) m->signals[s].bitpos_ptr = &m->bitpos_in_a[i];
                    break;
                }
            if (!m->signals[s].offset_ptr)
            for (size_t i=0;i<m->tx_b_count;++i)
                if (m->tx_b_entries[i].index==ix && m->tx_b_entries[i].subindex==sb) {
                    m->signals[s].offset_ptr = &m->off_in_b[i];
                    if (m->bitpos_in_b) m->signals[s].bitpos_ptr = &m->bitpos_in_b[i];
                    break;
                }
        }
        // bits length is already set in the signal table by the builder
    }

    return 0;
}

int pdo_map_get_signal(const pdo_map_t *m, pdo_signal_id_t id,
                       map_dir_t *dir, unsigned int **off,
                       unsigned int **bitpos, uint8_t *bits)
{
    if (id < 0 || id >= SIG__COUNT) return -1;
    const typeof(m->signals[0]) *s = &m->signals[id];
    if (!s->name || !s->offset_ptr) return -2;
    if (dir)    *dir    = s->dir;
    if (off)    *off    = (unsigned int*)s->offset_ptr;
    if (bitpos) *bitpos = (unsigned int*)s->bitpos_ptr;
    if (bits)   *bits   = s->bits;
    return 0;
}

void pdo_map_free(pdo_map_t *m)
{
    if (!m) return;
    free(m->rx_a_entries); free(m->rx_b_entries);
    free(m->tx_a_entries); free(m->tx_b_entries);
    free(m->syncs);

    free(m->regs_out); free(m->regs_in);

    free(m->off_out_a); free(m->off_out_b);
    free(m->off_in_a);  free(m->off_in_b);

    free(m->bitpos_in_a); free(m->bitpos_in_b);

    memset(m, 0, sizeof(*m));
}
