#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <jansson.h>
#include <ecrt.h>
#include <ncurses.h>

/* ============================================================
   Device-specific sizes (from your documentation):
   SM2 (Outputs -> slave):  62 bytes
   SM3 (Inputs  <- slave): 234 bytes
   ============================================================ */
#define SM2_BYTES      62
#define SM3_BYTES     234
#define TOTAL_BYTES  (SM2_BYTES + SM3_BYTES)

/* Field types supported in JSON ("u8", "u16", "u32") */
typedef enum { T_U8, T_U16, T_U32 } ftype_t;

/* A named field from JSON (SM3 only) */
typedef struct {
    char     name[128];        /* display name */
    int      logical_offset;   /* byte offset within SM3 (0..233) */
    ftype_t  type;             /* u8/u16/u32 */
    int      valid;            /* 1 if mapped within range, 0 otherwise */
    unsigned dom_off[4];       /* real domain byte offsets for up to 4 bytes (u32) */
} field_t;

typedef struct {
    field_t *arr;
    size_t   count;
} field_list_t;

/* ---------- JSON loader: reads fields.slave0.sm3 ---------- */
static int load_sm3_fields_from_json(const char *path, field_list_t *out)
{
    json_error_t err;
    json_t *root = json_load_file(path, 0, &err);
    if (!root) {
        fprintf(stderr, "JSON error: %s (line %d)\n", err.text, err.line);
        return -1;
    }

    json_t *fields = json_object_get(root, "fields");
    if (!fields) { json_decref(root); return -1; }

    json_t *slave0 = json_object_get(fields, "slave0");
    if (!slave0) { json_decref(root); return -1; }

    json_t *sm3 = json_object_get(slave0, "sm3");
    if (!sm3 || !json_is_array(sm3)) { json_decref(root); return -1; }

    size_t n = json_array_size(sm3);
    out->arr   = (field_t*)calloc(n, sizeof(field_t));
    out->count = n;

    for (size_t i = 0; i < n; i++) {
        json_t *jf = json_array_get(sm3, i);
        if (!jf) continue;

        const char *nm = json_string_value(json_object_get(jf, "name"));
        json_t    *joff = json_object_get(jf, "offset");
        const char *ty = json_string_value(json_object_get(jf, "type"));

        strncpy(out->arr[i].name, nm ? nm : "?", sizeof(out->arr[i].name)-1);
        out->arr[i].logical_offset = json_is_integer(joff) ? (int)json_integer_value(joff) : 0;

        if      (ty && !strcasecmp(ty, "u16")) out->arr[i].type = T_U16;
        else if (ty && !strcasecmp(ty, "u32")) out->arr[i].type = T_U32;
        else                                   out->arr[i].type = T_U8;

        out->arr[i].valid = 0;
        for (int k=0;k<4;k++) out->arr[i].dom_off[k] = (unsigned)~0u; /* invalid sentinel */
    }

    json_decref(root);
    return 0;
}

/* ---------- EtherCAT: configure slave + PDOs + domain, capture offsets ---------- */
static int setup_and_register(ec_master_t **out_master,
                              ec_domain_t **out_domain,
                              unsigned    **out_offsets)
{
    ec_master_t *master = ecrt_request_master(0);
    if (!master) { fprintf(stderr, "request_master failed\n"); return -1; }

    /* Your device: alias=0, position=0, vendor=0x6c, product=0xa72c */
    ec_slave_config_t *sc = ecrt_master_slave_config(master, 0, 0, 0x0000006c, 0x0000a72c);
    if (!sc) { fprintf(stderr, "slave_config failed\n"); return -1; }

    /* Build PDO entry lists (8-bit per subindex for both SMs) */
    ec_pdo_entry_info_t *sm2_e = calloc(SM2_BYTES, sizeof(*sm2_e));
    ec_pdo_entry_info_t *sm3_e = calloc(SM3_BYTES, sizeof(*sm3_e));
    if (!sm2_e || !sm3_e) { fprintf(stderr, "calloc entries failed\n"); return -1; }

    for (int i=0;i<SM2_BYTES;i++) sm2_e[i] = (ec_pdo_entry_info_t){ 0x7000, (uint8_t)(i+1), 8 };
    for (int i=0;i<SM3_BYTES;i++) sm3_e[i] = (ec_pdo_entry_info_t){ 0x6000, (uint8_t)(i+1), 8 };

    ec_pdo_info_t sm2_pdos[] = { { 0x1600, SM2_BYTES, sm2_e } };
    ec_pdo_info_t sm3_pdos[] = { { 0x1A00, SM3_BYTES, sm3_e } };

    ec_sync_info_t syncs[] = {
        {0, EC_DIR_OUTPUT, 0, NULL,     EC_WD_DISABLE},
        {1, EC_DIR_INPUT,  0, NULL,     EC_WD_DISABLE},
        {2, EC_DIR_OUTPUT, 1, sm2_pdos, EC_WD_ENABLE},
        {3, EC_DIR_INPUT,  1, sm3_pdos, EC_WD_DISABLE},
        {0xff}
    };

    if (ecrt_slave_config_pdos(sc, EC_END, syncs)) { fprintf(stderr, "config_pdos failed\n"); return -1; }

    ec_domain_t *domain = ecrt_master_create_domain(master);
    if (!domain) { fprintf(stderr, "create_domain failed\n"); return -1; }

    ec_pdo_entry_reg_t *regs = calloc(TOTAL_BYTES + 1, sizeof(*regs));
    unsigned *offs = (unsigned*)calloc(TOTAL_BYTES, sizeof(unsigned));
    if (!regs || !offs) { fprintf(stderr, "calloc regs/offs failed\n"); return -1; }

    int ix = 0;
    /* Register SM2 entries (not shown in viewer, but included for correctness) */
    for (int i=0;i<SM2_BYTES;i++, ix++) {
        regs[ix] = (ec_pdo_entry_reg_t){
            .alias=0, .position=0,
            .vendor_id=0x0000006c, .product_code=0x0000a72c,
            .index=0x7000, .subindex=(uint8_t)(i+1),
            .offset=&offs[ix]
        };
    }
    /* Register SM3 entries */
    for (int i=0;i<SM3_BYTES;i++, ix++) {
        regs[ix] = (ec_pdo_entry_reg_t){
            .alias=0, .position=0,
            .vendor_id=0x0000006c, .product_code=0x0000a72c,
            .index=0x6000, .subindex=(uint8_t)(i+1),
            .offset=&offs[ix]
        };
    }
    /* Terminator already zeroed */

    if (ecrt_domain_reg_pdo_entry_list(domain, regs)) { fprintf(stderr, "domain_reg_pdo_entry_list failed\n"); return -1; }
    if (ecrt_master_activate(master))                 { fprintf(stderr, "master_activate failed\n"); return -1; }

    *out_master  = master;
    *out_domain  = domain;
    *out_offsets = offs;
    return 0;
}

/* ---------- Map fields to their real domain offsets (byte-by-byte) ---------- */
static void map_fields_to_domain(field_list_t *fl, unsigned *all_offsets)
{
    /* Build an index: SM3 logical byte i -> domain offset for that byte */
    /* NB: Registration order = [ SM2 (0..SM2_BYTES-1) , SM3 (0..SM3_BYTES-1) ] */
    unsigned sm3_dom_off[SM3_BYTES];
    for (int i=0;i<SM3_BYTES;i++) {
        int reg_index = SM2_BYTES + i;
        sm3_dom_off[i] = all_offsets[reg_index];
    }

    /* For each field, resolve every constituent byte via sm3_dom_off */
    for (size_t f=0; f<fl->count; f++) {
        int base = fl->arr[f].logical_offset;
        int need = (fl->arr[f].type == T_U8) ? 1 : (fl->arr[f].type == T_U16 ? 2 : 4);

        /* Bounds check on SM3 logical offsets */
        if (base < 0 || base + need > SM3_BYTES) {
            fl->arr[f].valid = 0;
            continue;
        }

        for (int k=0; k<need; k++) {
            fl->arr[f].dom_off[k] = sm3_dom_off[base + k];
	    fprintf(stderr, "%s %i %i\n", fl->arr[f].name, fl->arr[f].dom_off[k], fl->arr[f].logical_offset);
        }
        fl->arr[f].valid = 1;
    }
}

/* ---------- Safe readers that use per-byte mapping ---------- */
static uint32_t read_u8(const uint8_t *dom, const field_t *fld) {
    return dom[fld->dom_off[0]];
}
static uint32_t read_u16_le(const uint8_t *dom, const field_t *fld) {
    return  dom[fld->dom_off[0]]
          | (dom[fld->dom_off[1]] << 8);
}
static uint32_t read_u32_le(const uint8_t *dom, const field_t *fld) {
    return  dom[fld->dom_off[0]]
          | (dom[fld->dom_off[1]] << 8)
          | (dom[fld->dom_off[2]] << 16)
          | (dom[fld->dom_off[3]] << 24);
}
static uint32_t read_field_value(const uint8_t *dom, const field_t *fld) {
    if (!fld->valid) return 0;
    switch (fld->type) {
        case T_U8:  return read_u8(dom, fld);
        case T_U16: return read_u16_le(dom, fld);
        case T_U32: return read_u32_le(dom, fld);
        default:    return 0;
    }
}

/* ============================== MAIN =============================== */
int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: sudo %s ecat_pdo_config.json\n", argv[0]);
        return 1;
    }

    /* 1) Load named SM3 fields from JSON */
    field_list_t fl = {0};
    if (load_sm3_fields_from_json(argv[1], &fl)) {
        fprintf(stderr, "Failed to load SM3 fields from JSON.\n");
        return 1;
    }

    /* 2) Configure EtherCAT, register PDO entries, get domain offsets */
    ec_master_t *master = NULL;
    ec_domain_t *domain = NULL;
    unsigned    *all_offsets = NULL;

    if (setup_and_register(&master, &domain, &all_offsets)) {
        fprintf(stderr, "EtherCAT setup failed.\n");
        return 1;
    }

    uint8_t *dom = ecrt_domain_data(domain);
    if (!dom) {
        fprintf(stderr, "domain_data is NULL.\n");
        return 1;
    }

    /* 3) Map each field’s logical SM3 bytes to REAL domain offsets */
    map_fields_to_domain(&fl, all_offsets);

    /* 4) ncurses TUI */
    initscr(); noecho(); cbreak(); curs_set(0); timeout(0);
    if (has_colors()) { start_color(); init_pair(1, COLOR_GREEN, COLOR_BLACK); init_pair(2, COLOR_YELLOW, COLOR_BLACK); }

    uint32_t *last = (uint32_t*)calloc(fl.count, sizeof(uint32_t));
    int ch;
    while ((ch = getch()) != 'q') {
        ecrt_master_receive(master);
        ecrt_domain_process(domain);

        attron(COLOR_PAIR(2));
        mvprintw(0, 0, "CIFX RE/ECS Live Viewer  |  SM3 fields  |  q: quit");
        attroff(COLOR_PAIR(2));

        for (size_t i=0; i<fl.count; i++) {
            int row = (int)i + 2;

            if (!fl.arr[i].valid) {
                mvprintw(row, 0, "%-30s  [INVALID/OOB: offset %d, type %s]",
                         fl.arr[i].name,
                         fl.arr[i].logical_offset,
                         (fl.arr[i].type==T_U8?"u8":(fl.arr[i].type==T_U16?"u16":"u32")));
                continue;
            }

            /* Read live value using REAL per-byte domain offsets */
            uint32_t v = read_field_value(dom, &fl.arr[i]);

            if (v != last[i]) attron(COLOR_PAIR(1));

            /* Print first byte’s real offset for quick verify;
               for multibyte, show low..high byte offsets too. */
            if (fl.arr[i].type == T_U8) {
                mvprintw(row, 0, "%-30s dom_off=%-4u  val=%10u (0x%08X)",
                         fl.arr[i].name, fl.arr[i].dom_off[0], v, v);
            } else if (fl.arr[i].type == T_U16) {
                mvprintw(row, 0, "%-30s dom_off=[%u,%u]  val=%10u (0x%08X)",
                         fl.arr[i].name, fl.arr[i].dom_off[0], fl.arr[i].dom_off[1], v, v);
            } else {
                mvprintw(row, 0, "%-30s dom_off=[%u,%u,%u,%u]  val=%10u (0x%08X)",
                         fl.arr[i].name, fl.arr[i].dom_off[0], fl.arr[i].dom_off[1],
                         fl.arr[i].dom_off[2], fl.arr[i].dom_off[3], v, v);
            }

            if (v != last[i]) attroff(COLOR_PAIR(1));
            last[i] = v;
        }

        ecrt_master_send(master);
        usleep(100000); /* 10 Hz */
    }

    endwin();
    return 0;
}
