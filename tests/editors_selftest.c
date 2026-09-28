/* Self-test for the authoring editors and the chat command dispatcher.
 *
 * Editors: copies the retail world out of extracted/ into a temp tree, loads a map's
 * .obl/.mon/.ter, saves them back untouched and proves the bytes are unchanged, then places a
 * link and a monster and proves the written files differ from the originals only inside the
 * edited records. The .mon difference at record offset 16 is expected and is asserted
 * separately: FUN_00464461 zeroes the runtime instance pointer of every record on load and
 * FUN_00464421 dumps the array verbatim.
 *
 * Chat: submits the offline slash commands and asserts the structured events land in the log.
 *
 * Usage: editors_selftest <data-dir> <scratch-dir> */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/chat.h"
#include "game/editors.h"
#include "game/world.h"
#include "engine/ini.h"
#include "engine/log.h"
#include "engine/rng.h"
#include "platform/platform.h"

#define MAP_ROOT "Evergreen"

static int le32(const unsigned char *p)
{ return (int)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24)); }

static unsigned char *slurp(const char *path, size_t *size)
{
    unsigned char *buf;
    long n;
    FILE *f = plat_fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    buf = malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

/* index of the first differing byte, or -1 */
static long first_diff(const unsigned char *a, const unsigned char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i) if (a[i] != b[i]) return (long)i;
    return -1;
}

static int count_records_changed(const unsigned char *a, const unsigned char *b,
                                 size_t size, size_t record, int *offsets_out, int max_offsets)
{
    int changed = 0, n = 0;
    size_t r, i;
    for (r = 0; r + record <= size; r += record) {
        int rec = 0;
        for (i = 0; i < record; ++i) {
            if (a[r + i] != b[r + i]) {
                if (!rec && n < max_offsets) offsets_out[n] = (int)i;
                if (!rec) ++n;
                rec = 1;
            }
        }
        changed += rec;
    }
    return changed;
}

static void copy_file(const char *src, const char *dst)
{
    size_t n;
    unsigned char *buf = slurp(src, &n);
    FILE *f;
    assert(buf);
    f = plat_fopen(dst, "wb");
    assert(f);
    assert(fwrite(buf, 1, n, f) == n);
    assert(fclose(f) == 0);
    free(buf);
}

static void copy_dir(const char *from, const char *to);

typedef struct { const char *from, *to; } DirPair;

static void copy_entry(const char *name, int is_dir, void *user)
{
    const DirPair *pair = (const DirPair *)user;
    char a[1024], b[1024];
    snprintf(a, sizeof(a), "%s/%s", pair->from, name);
    snprintf(b, sizeof(b), "%s/%s", pair->to, name);
    if (is_dir) copy_dir(a, b);
    else copy_file(a, b);
}

static void copy_dir(const char *from, const char *to)
{
    DirPair pair;
    pair.from = from;
    pair.to = to;
    plat_mkdir(to);
    plat_list_dir(from, copy_entry, &pair);
}

/* Copies <data>/worlds into <scratch>/worlds so the editor never reads or writes the retail
 * tree. extracted/ is never modified. */
static void copy_worlds(const char *data, const char *scratch)
{
    char from[1024], to[1024];
    snprintf(from, sizeof(from), "%s/worlds", data);
    snprintf(to, sizeof(to), "%s/worlds", scratch);
    copy_dir(from, to);
}

static char *read_all(const char *path, size_t *size)
{
    return (char *)slurp(path, size);
}

int main(int argc, char **argv)
{
    const char *data = argc > 1 ? argv[1] : "extracted";
    const char *scratch = argc > 2 ? argv[2] : "/tmp/wos-editors-selftest";
    char src[1024], dst[512];
    char orig_obl[600], orig_mon[600], orig_ter[600];
    char logpath[1024];
    unsigned char *o_obl, *o_mon, *o_ter, *n_obl, *n_mon, *n_ter;
    size_t s_obl, s_mon, s_ter;
    int offsets[8], changed, map_id = -1, i;
    char *logtext;
    size_t logsize;

    if (plat_init("wos-editors-selftest", PLAT_SCREEN_W, PLAT_SCREEN_H, PLAT_INIT_HEADLESS)) {
        fprintf(stderr, "plat_init failed\n");
        return 1;
    }
    plat_mkdir(scratch);

    /* --- copy the retail world's map binaries into the scratch tree ---------------------- */
    copy_worlds(data, scratch);
    snprintf(dst, sizeof(dst), "%s/worlds/%s/maps", scratch, MAP_ROOT);
    snprintf(orig_obl, sizeof(orig_obl), "%s/%s.obl", dst, "evergreen");
    snprintf(orig_mon, sizeof(orig_mon), "%s/%s.mon", dst, "evergreen");
    snprintf(orig_ter, sizeof(orig_ter), "%s/%s.ter", dst, "evergreen");
    (void)src;
    o_obl = slurp(orig_obl, &s_obl); assert(o_obl);
    o_mon = slurp(orig_mon, &s_mon); assert(o_mon);
    o_ter = slurp(orig_ter, &s_ter); assert(o_ter);
    assert(s_obl == (size_t)OBL_RECORDS * OBL_RECORD_SIZE);
    assert(s_mon == (size_t)MON_RECORDS * MON_RECORD_SIZE);

    /* --- open the world and the map out of the scratch tree -------------------------------- */
    editors_set_data_dir(scratch);
    assert(editors_open(MAP_ROOT) == 0);
    for (i = 0; i < WORLD_MAX_MAPS; ++i) if (g_world.maps[i].used) { map_id = i; break; }
    assert(map_id >= 0);
    assert(strcmp(g_world.maps[map_id].root, "evergreen") == 0);
    assert(editors_load_map(map_id) == 0);
    assert(editors_state()->ter_w == 192 && editors_state()->ter_h == 192);

    /* --- 1. an untouched save reproduces the retail .obl byte for byte --------------------- */
    assert(editors_save_links() == 0);
    n_obl = slurp(orig_obl, &s_obl); assert(n_obl);
    assert(first_diff(o_obl, n_obl, (size_t)OBL_RECORDS * OBL_RECORD_SIZE) == -1);
    printf("PASS obl round-trip is byte identical (%lu bytes)\n",
           (unsigned long)(OBL_RECORDS * OBL_RECORD_SIZE));

    /* --- 2. an untouched .mon save differs only where FUN_00464461 zeroed the instance ---- */
    assert(editors_save_monsters() == 0);
    n_mon = slurp(orig_mon, &s_mon); assert(n_mon);
    changed = count_records_changed(o_mon, n_mon, (size_t)MON_RECORDS * MON_RECORD_SIZE,
                                    MON_RECORD_SIZE, offsets, 8);
    printf("mon records differing after an untouched save: %d\n", changed);
    for (i = 0; i < changed; ++i) {
        int r;
        for (r = 0; r < MON_RECORDS; ++r) {
            const unsigned char *a = o_mon + (size_t)r * MON_RECORD_SIZE;
            const unsigned char *b = n_mon + (size_t)r * MON_RECORD_SIZE;
            size_t k;
            if (memcmp(a, b, MON_RECORD_SIZE) == 0) continue;
            for (k = 0; k < MON_RECORD_SIZE; ++k)
                assert(a[k] != b[k] ? (k >= MON_OFFSET_INSTANCE && k < MON_OFFSET_INSTANCE + 4)
                                     : 1);
        }
    }
    assert(changed > 0);   /* Evergreen ships 103 records with a non-zero instance pointer */
    free(n_mon);

    /* --- 3. an untouched .ter save reproduces the retail file byte for byte ---------------- */
    assert(editors_save_terrain() == 0);
    n_ter = slurp(orig_ter, &s_ter); assert(n_ter);
    assert(first_diff(o_ter, n_ter, s_ter) == -1);
    printf("PASS ter round-trip is byte identical (%lu bytes)\n", (unsigned long)s_ter);
    free(n_ter);

    /* --- 4. place a link, place a monster, paint one terrain cell ------------------------- */
    {
        int link_index, mon_index, cell_before;
        int free_link = -1;
        for (i = 0; i < OBL_RECORDS; ++i) {
            Link l;
            assert(editors_link_get(i, &l) == 0);
            if (l.used == 0) { free_link = i; break; }
        }
        assert(free_link >= 0);
        link_index = editors_link_add(300, 250, 7, 2, map_id, 11);
        assert(link_index == free_link);
        assert(editors_link_at(300, 250) == link_index);

        mon_index = editors_monster_place(120, 90, 42, 0, 0);
        assert(mon_index >= 0);
        {
            MonPlace mp;
            assert(editors_monster_get(mon_index, &mp) == 0);
            assert(mp.monster_id == 42);
            assert(mp.x == 30 && mp.y == 22);          /* map units / 4, as FUN_00464b06 does */
            assert(mp.radius == editors_state()->brush_size);
        }

        cell_before = editors_terrain_get(20, 20);
        assert(cell_before >= 0);
        editors_set_brush_terrain((cell_before + 1) & 0xff);
        assert(editors_terrain_paint(20 * 16, 20 * 16, 1) == 1);
        assert(editors_terrain_get(20, 20) == ((cell_before + 1) & 0xff));

        assert(editors_save() == 3);

        /* the .obl differs only inside the one record we wrote */
        n_obl = slurp(orig_obl, &s_obl); assert(n_obl);
        changed = count_records_changed(o_obl, n_obl, (size_t)OBL_RECORDS * OBL_RECORD_SIZE,
                                        OBL_RECORD_SIZE, offsets, 8);
        printf("obl records differing after adding link %d: %d\n", link_index, changed);
        /* Record `link_index` is the one we wrote. Every other difference is FUN_004636d3's
         * cursor-distance scratch at OBL_OFFSET_DISTANCE, which the original also leaves in
         * memory and dumps whenever the file is saved for any other reason. Nothing else
         * may move. */
        {
            int edited = 0, other = 0, r;
            for (r = 0; r < OBL_RECORDS; ++r) {
                size_t k;
                int bad = 0;
                for (k = 0; k < OBL_RECORD_SIZE; ++k) {
                    if (o_obl[(size_t)r * OBL_RECORD_SIZE + k] == n_obl[(size_t)r * OBL_RECORD_SIZE + k]) continue;
                    if (k >= OBL_OFFSET_DISTANCE && k < OBL_OFFSET_DISTANCE + 4) continue;
                    bad = 1;
                }
                if (r == link_index) { assert(bad); edited = 1; }
                else if (bad) ++other;
            }
            assert(edited);
            assert(other == 0);
            assert(changed >= 1);
        }
        free(n_obl);

        /* the .mon differs only inside the one record we wrote, plus the instance-pointer
         * zeroing FUN_00464461 does on load */
        n_mon = slurp(orig_mon, &s_mon); assert(n_mon);
        changed = count_records_changed(o_mon, n_mon, (size_t)MON_RECORDS * MON_RECORD_SIZE,
                                        MON_RECORD_SIZE, offsets, 8);
        printf("mon records differing after adding monster %d: %d\n", mon_index, changed);
        assert(changed >= 1);
        {
            int edited = 0, other = 0, r;
            for (r = 0; r < MON_RECORDS; ++r) {
                size_t k;
                int bad = 0;
                for (k = 0; k < MON_RECORD_SIZE; ++k)
                    if (o_mon[(size_t)r * MON_RECORD_SIZE + k] != n_mon[(size_t)r * MON_RECORD_SIZE + k]) {
                        if (k >= MON_OFFSET_INSTANCE && k < MON_OFFSET_INSTANCE + 4) continue;
                        bad = 1;
                    }
                if (r == mon_index) {
                    assert(memcmp(o_mon + (size_t)r * MON_RECORD_SIZE,
                                  n_mon + (size_t)r * MON_RECORD_SIZE, 4) != 0);
                    edited = 1;
                } else if (bad) ++other;
            }
            assert(edited);
            assert(other == 0);
        }
        free(n_mon);

        /* the .ter differs in exactly one byte */
        n_ter = slurp(orig_ter, &s_ter); assert(n_ter);
        {
            long d = first_diff(o_ter, n_ter, s_ter);
            size_t differing = 0, k;
            for (k = 0; k < s_ter; ++k) if (o_ter[k] != n_ter[k]) ++differing;
            printf("ter bytes differing: %lu (first at %ld)\n", (unsigned long)differing, d);
            assert(differing == 1);
        }
        free(n_ter);
        free(o_obl); free(o_mon); free(o_ter);
    }

    /* --- 5. world signing preserves the opaque signature ------------------------------------ */
    {
        char creator[WORLD_VER_NAME_MAX], last_mod[WORLD_VER_NAME_MAX];
        uint32_t serial = 0;
        assert(editors_world_version(NULL, creator, last_mod, &serial) == 0);
        assert(strcmp(creator, "Dan Samuel") == 0);
        assert(serial == 0x47df5284u);   /* the u32 at 0x5C, little-endian */
        assert(editors_world_format(NULL) == 0x0a96u);   /* the shipping file predates 0xA97 */
        /* FUN_0049747C: the file's +0x10 against the player's id decides official vs MOD. */
        assert(editors_world_is_mod(0x80516c88u) == 0);
        assert(editors_world_is_mod(1u) == 1);
        /* FUN_00497106, transcribed: h = 0x402C8E17 then h = (int8)c * (h*2) per character. */
        assert(editors_ver_hash("") == 0x402c8e17u);
        assert(editors_ver_hash("A") == (uint32_t)65u * (0x402c8e17u * 2u));
        assert(editors_sign_world("Editors", "1.0137", 0x39698b0du, 0x1234u) == 0);
        assert(editors_world_version(NULL, creator, last_mod, &serial) == 0);
        assert(strcmp(creator, "Editors") == 0);
        assert(serial == 0x1234u);
        assert(editors_world_format(NULL) == WORLD_VER_FORMAT);
        {
            /* Re-read the raw record: the field map must be the one FUN_00497080 writes. */
            unsigned char rec[WORLD_VER_SIZE];
            char path[1024];
            FILE *f;
            size_t n;
            int body_zero = 1, i;
            snprintf(path, sizeof(path), "%s/worlds/%s/world.ver", scratch, MAP_ROOT);
            f = plat_fopen(path, "rb");
            assert(f);
            n = fread(rec, 1, sizeof(rec), f);
            fclose(f);
            assert(n == sizeof(rec));
            assert(le32(rec + WORLD_VER_ID) == (int)0x39698b0du);
            assert(le32(rec + WORLD_VER_ID2) == (int)0x39698b0du);
            assert(le32(rec + WORLD_VER_AUTHOR) == 0x1234);
            assert(le32(rec + WORLD_VER_ID3) == (int)0x39698b0du);
            assert(le32(rec + WORLD_VER_SERIAL) == 0x1234);
            assert(le32(rec + WORLD_VER_HASH) == (int)editors_ver_hash("1.0137"));
            assert(le32(rec + WORLD_VER_FORMAT_AT) == (int)WORLD_VER_FORMAT);
            /* FUN_00497080 memsets 0x4A4 bytes and writes only the fields above, so every byte
             * the original never stores must come back zero. */
            for (i = WORLD_VER_ID + 4; i < WORLD_VER_ID2; ++i) if (rec[i]) body_zero = 0;
            for (i = WORLD_VER_AUTHOR + 4; i < WORLD_VER_CREATOR; ++i) if (rec[i]) body_zero = 0;
            for (i = WORLD_VER_FORMAT_AT + 4; i < WORLD_VER_SIZE; ++i) if (rec[i]) body_zero = 0;
            assert(body_zero);
            printf("PASS world.ver field map and zero fill\n");
        }
        printf("PASS world.ver sign round-trip\n");
    }
    editors_close();

    /* --- 6. chat: the offline slash commands log events ------------------------------------ */
    snprintf(logpath, sizeof(logpath), "%s/chat.log", scratch);
    assert(wos_log_open(logpath) == 0);
    crt_srand(1);
    chat_init();
    {
        static const char *const lines[] = {
            "/springy", "/version", "/terrain", "/monsters", "/coord", "/fps", "/share",
            "/eavesdrop", "/seance", "/tune 3", "/tune", "/weather 2", "/fx 1", "/pal 1",
            "/gender 2", "/afk tester", "/dice 3d6", "/pdice 2d20", "/shake", "/nope",
            "/battle", "/battle2", "/battle3", "/gimme", "/mags", "/pwd", "/sayings",
        };
        size_t n = sizeof(lines) / sizeof(lines[0]);
        for (i = 0; i < (int)n; ++i) assert(chat_submit(lines[i]) == 1);
    }
    assert(chat_overlay_terrain() == 1);
    assert(chat_overlay_coord() == 1);
    assert(chat_show_fps() == 1);
    assert(chat_share() == 1);
    assert(chat_eavesdrop() == 1);
    assert(chat_seance() == 1);
    assert(chat_tune() == 3);
    assert(chat_springy_loaded() == 1);
    assert(chat_springy_object_count() == 1);
    assert(chat_springy_masses(0) == 4);
    {
        double mass[4];
        assert(chat_springy_mass(0, 0, mass) == 0);
        assert(mass[0] == 10.0 && mass[1] == -1.0 && mass[2] == 1.0 && mass[3] == 0.3);
    }
    wos_log_close();

    logtext = read_all(logpath, &logsize);
    assert(logtext);
    {
        static const char *const required[] = {
            "EVT chat_command cmd=springy", "EVT chat_command cmd=version",
            "EVT chat_command cmd=terrain", "EVT chat_command cmd=monsters",
            "EVT chat_command cmd=coord", "EVT chat_command cmd=dice",
            "EVT chat_command cmd=pdice", "EVT chat_command cmd=battle",
            "EVT chat_command cmd=battle2", "EVT chat_command cmd=battle3",
            "EVT chat_command cmd=nope state=unknown", "EVT chat_dice",
            "EVT chat_overlay name=terrain", "EVT chat_overlay name=coord",
            "EVT springy_object", "EVT world_version", "EVT chat_say",
        };
        size_t k;
        for (k = 0; k < sizeof(required) / sizeof(required[0]); ++k) {
            if (!strstr(logtext, required[k])) {
                fprintf(stderr, "FAIL: log is missing \"%s\"\n", required[k]);
                return 1;
            }
        }
        printf("PASS chat events logged (%lu bytes)\n", (unsigned long)logsize);
    }
    free(logtext);

    /* the .mon load zeroes the instance pointer, so a world we never saved still round-trips
     * the .obl and the .ter exactly; the .mon difference is asserted above, not here. */
    printf("PASS editors_selftest\n");
    plat_shutdown();
    return 0;
}
