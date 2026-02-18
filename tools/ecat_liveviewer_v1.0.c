#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <jansson.h>
#include <ecrt.h>
#include <ncurses.h>

/**********************************************************************
 * We do NOT trust JSON offsets as domain offsets.
 * We ONLY use JSON to map field names to their logical byte positions
 * within SM3.
 *
 * REAL offsets come from IgH after domain registration.
 **********************************************************************/

typedef enum { T_U8, T_U16, T_U32 } ftype_t;

typedef struct {
    char name[128];
    int logical_offset;   // byte offset in SM3
    ftype_t type;
    unsigned real_offset; // returned by IgH from domain registration
} field_t;

typedef struct {
    field_t *f;
    size_t count;
} field_list_t;


/**********************************************************************
 * Load JSON fields.slave0.sm3 ONLY (SM3 = input buffer)
 **********************************************************************/
static int load_field_list(const char *path, field_list_t *out)
{
    json_error_t err;
    json_t *root = json_load_file(path, 0, &err);
    if (!root) {
        fprintf(stderr, "JSON error: %s at line %d\n", err.text, err.line);
        return -1;
    }

    json_t *fields = json_object_get(root, "fields");
    if (!fields) { json_decref(root); return -1; }

    json_t *slave0 = json_object_get(fields, "slave0");
    if (!slave0) { json_decref(root); return -1; }

    json_t *sm3 = json_object_get(slave0, "sm3");
    if (!sm3 || !json_is_array(sm3)) { json_decref(root); return -1; }

    size_t n = json_array_size(sm3);
    out->f = calloc(n, sizeof(field_t));
    out->count = n;

    for (size_t i = 0; i < n; i++) {
        json_t *jf = json_array_get(sm3, i);
        if (!jf) continue;

        const char *nm = json_string_value(json_object_get(jf, "name"));
        json_t *joff   = json_object_get(jf, "offset");
        const char *ty = json_string_value(json_object_get(jf, "type"));

        strncpy(out->f[i].name, nm ? nm : "?", 127);
        out->f[i].logical_offset =
            json_is_integer(joff) ? json_integer_value(joff) : 0;

        if (ty) {
            if (!strcasecmp(ty, "u16")) out->f[i].type = T_U16;
            else if (!strcasecmp(ty, "u32")) out->f[i].type = T_U32;
            else out->f[i].type = T_U8;
        } else out->f[i].type = T_U8;
    }

    json_decref(root);
    return 0;
}


/**********************************************************************
 * EtherCAT setup (SM2=62 bytes, SM3=234 bytes)
 **********************************************************************/
#define SM2_BYTES 62
#define SM3_BYTES 234
#define TOTAL_BYTES (SM2_BYTES + SM3_BYTES)

static int setup(ec_master_t **out_master,
                 ec_domain_t **out_domain,
                 unsigned **out_offsets)
{
    ec_master_t *master = ecrt_request_master(0);
    if (!master) return -1;

    ec_slave_config_t *sc =
        ecrt_master_slave_config(master,
                                 0, 0,
                                 0x0000006c, /* vendor */
                                 0x0000a72c  /* product */);
    if (!sc) return -1;

    // Build entry arrays
    ec_pdo_entry_info_t *sm2_e = calloc(SM2_BYTES, sizeof(*sm2_e));
    ec_pdo_entry_info_t *sm3_e = calloc(SM3_BYTES, sizeof(*sm3_e));

    for (int i=0;i<SM2_BYTES;i++)
        sm2_e[i] = (ec_pdo_entry_info_t){0x7000,(uint8_t)(i+1),8};
    for (int i=0;i<SM3_BYTES;i++)
        sm3_e[i] = (ec_pdo_entry_info_t){0x6000,(uint8_t)(i+1),8};

    ec_pdo_info_t sm2_pdos[]={{0x1600,SM2_BYTES,sm2_e}};
    ec_pdo_info_t sm3_pdos[]={{0x1A00,SM3_BYTES,sm3_e}};

    ec_sync_info_t syncs[]={
        {0,EC_DIR_OUTPUT,0,NULL,EC_WD_DISABLE},
        {1,EC_DIR_INPUT ,0,NULL,EC_WD_DISABLE},
        {2,EC_DIR_OUTPUT,1,sm2_pdos,EC_WD_ENABLE},
        {3,EC_DIR_INPUT ,1,sm3_pdos,EC_WD_DISABLE},
        {0xff}
    };

    if (ecrt_slave_config_pdos(sc, EC_END, syncs)) return -1;

    ec_domain_t *domain = ecrt_master_create_domain(master);
    if (!domain) return -1;

    // Register all entries and capture offsets
    ec_pdo_entry_reg_t *regs =
        calloc(TOTAL_BYTES+1, sizeof(ec_pdo_entry_reg_t));
    unsigned *offsets = calloc(TOTAL_BYTES, sizeof(unsigned));
    if (!regs||!offsets) return -1;

    int ix=0;
    // SM2 first (not used by viewer but included for completeness)
    for(int i=0;i<SM2_BYTES;i++,ix++){
        regs[ix]=(ec_pdo_entry_reg_t){
            .alias=0,.position=0,
            .vendor_id=0x0000006c,
            .product_code=0x0000a72c,
            .index=0x7000,
            .subindex=(uint8_t)(i+1),
            .offset = &offsets[ix],
        };
    }
    // SM3
    for(int i=0;i<SM3_BYTES;i++,ix++){
        regs[ix]=(ec_pdo_entry_reg_t){
            .alias=0,.position=0,
            .vendor_id=0x0000006c,
            .product_code=0x0000a72c,
            .index=0x6000,
            .subindex=(uint8_t)(i+1),
            .offset = &offsets[ix],
        };
    }

    if (ecrt_domain_reg_pdo_entry_list(domain, regs)) return -1;
    if (ecrt_master_activate(master)) return -1;

    *out_master = master;
    *out_domain = domain;
    *out_offsets= offsets;
    return 0;
}


/**********************************************************************
 * Helpers
 **********************************************************************/
static uint32_t read_val(const uint8_t *dom,
                         unsigned offset, ftype_t t)
{
    switch(t){
    case T_U8:  return dom[offset];
    case T_U16: return dom[offset] | (dom[offset+1]<<8);
    case T_U32: return dom[offset] |
                       (dom[offset+1]<<8)|
                       (dom[offset+2]<<16)|
                       (dom[offset+3]<<24);
    default: return 0;
    }
}


/**********************************************************************
 * MAIN VIEWER
 **********************************************************************/
int main(int argc,char**argv)
{
    if(argc<2){
        printf("Usage: sudo %s ecat_pdo_config.json\n", argv[0]);
        return 1;
    }

    // Load fields list from JSON
    field_list_t fl={0};
    if(load_field_list(argv[1], &fl)) return 1;

    // Setup EtherCAT + domain
    ec_master_t *master=NULL;
    ec_domain_t *domain=NULL;
    unsigned *offsets=NULL;

    if(setup(&master, &domain, &offsets)) {
        printf("Setup failed.\n");
        return 1;
    }

    uint8_t *dom = ecrt_domain_data(domain);
    if(!dom){ printf("domain_data is NULL\n");return 1;}

    // Map each field’s logical_offset to a real_offset
    for(size_t i=0;i<fl.count;i++){
        int logical = fl.f[i].logical_offset;

        if(logical<0 || logical>=SM3_BYTES){
            fl.f[i].real_offset = 0xFFFFFFFF; // mark invalid
            continue;
        }

        int real_index = SM2_BYTES + logical; // SM3 entries start after SM2
        fl.f[i].real_offset = offsets[real_index];
    }

    // Setup ncurses
    initscr();
    noecho();
    cbreak();
    curs_set(0);
    timeout(0);

    if (has_colors()){
        start_color();
        init_pair(1,COLOR_GREEN,COLOR_BLACK);
        init_pair(2,COLOR_YELLOW,COLOR_BLACK);
    }

    // Previous values for change highlighting
    uint32_t *last = calloc(fl.count,sizeof(uint32_t));

    int ch;
    while((ch=getch())!='q'){
        ecrt_master_receive(master);
        ecrt_domain_process(domain);

        attron(COLOR_PAIR(2));
        mvprintw(0,0,"CIFX RE/ECS Live Viewer - Press q to quit");
        attroff(COLOR_PAIR(2));

        for(size_t i=0;i<fl.count;i++){
            int row = (int)i + 2;

            if(fl.f[i].real_offset==0xFFFFFFFF){
                mvprintw(row,0,"%-30s INVALID OFFSET", fl.f[i].name);
                continue;
            }

            uint32_t v = read_val(dom, fl.f[i].real_offset, fl.f[i].type);

            if(v!=last[i]) attron(COLOR_PAIR(1));

            mvprintw(row,0,
                "%-30s off=%4u  val=%10u (0x%X)",
                fl.f[i].name,
                fl.f[i].real_offset,
                v, v);

            if(v!=last[i]) attroff(COLOR_PAIR(1));

            last[i]=v;
        }

        ecrt_master_send(master);
        usleep(100000); // 10 Hz
    }

    endwin();
    return 0;
}
