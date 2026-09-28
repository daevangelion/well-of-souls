/* Native Clickteam Install Creator (CIC) installer decoder (see cic_unpack.h).
 * Port of the validated reference logic (tools/cic_reference.py) to C, using the
 * vendored miniz (raw deflate) and bzip2 decoders. The installer is untrusted
 * input, so every field read is bounds-checked and sizes are capped. */
#include "cic_unpack.h"
#include "platform/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MINIZ_HEADER_FILE_ONLY
#include "miniz.h"
#include "bzlib.h"

#define CIC_PATH_MAX   1024
#define CIC_MAX_FILES  200000
#define CIC_MAX_BLOCK  (64u * 1024u * 1024u)   /* cap any single decompressed block */
#define CIC_MAX_TOTAL  (512u * 1024u * 1024u)  /* cap total decompressed output */

#define BLOCK_FILE_LIST 0x143Au
#define BLOCK_FILE_DATA 0x7F7Fu

typedef struct { const unsigned char *b; size_t p, end; int bad; } Cur;
typedef struct { uint32_t off, comp, unc; char path[CIC_PATH_MAX]; } CicFile;

static uint16_t rd16(Cur *c) {
    if (c->bad || c->end - c->p < 2) { c->bad = 1; return 0; }
    uint16_t v = (uint16_t)(c->b[c->p] | (c->b[c->p+1] << 8)); c->p += 2; return v;
}
static uint32_t rd32(Cur *c) {
    if (c->bad || c->end - c->p < 4) { c->bad = 1; return 0; }
    uint32_t v = (uint32_t)c->b[c->p] | ((uint32_t)c->b[c->p+1] << 8) |
                 ((uint32_t)c->b[c->p+2] << 16) | ((uint32_t)c->b[c->p+3] << 24);
    c->p += 4; return v;
}
static void skip(Cur *c, size_t n) { if (c->end - c->p < n) { c->bad = 1; c->p = c->end; } else c->p += n; }
static int at(const unsigned char *b, size_t len, size_t off, unsigned char *out) {
    if (off >= len) return 0; *out = b[off]; return 1;
}

/* Raw-deflate one-shot: 2-byte zlib header skipped by the caller (we start at the
 * raw deflate stream). Self-terminating, so we feed the rest of the buffer. */
static int inflate_deflate(const unsigned char *src, size_t src_len, unsigned char *dst, size_t dst_len) {
    size_t n = tinfl_decompress_mem_to_mem(dst, dst_len, src, src_len, 0);
    return n != TINFL_DECOMPRESS_MEM_TO_MEM_FAILED ? (int)n : -1;
}
/* BZip2 one-shot: stream starts right after the method byte, self-terminating. */
static int inflate_bzip2(const unsigned char *src, size_t src_len, unsigned char *dst, size_t dst_len) {
    unsigned int dlen = (unsigned int)dst_len;
    if (BZ2_bzBuffToBuffDecompress((char *)dst, &dlen, (char *)(uintptr_t)src, (unsigned int)src_len, 0, 0) != BZ_OK)
        return -1;
    return (int)dlen;
}

/* mkdir -p; plat_mkdir returns 0 on success or "already exists". */
static int mkdirs(const char *dir) {
    char tmp[CIC_PATH_MAX * 2]; size_t n = strlen(dir);
    if (n == 0 || n >= sizeof(tmp)) return -1;
    memcpy(tmp, dir, n + 1);
    for (size_t i = 1; i <= n; ++i) {
        if (tmp[i] == '/' || tmp[i] == '\\' || i == n) {
            char save = tmp[i]; tmp[i] = 0;
            plat_mkdir(tmp);
            tmp[i] = save;
        }
    }
    return 0;
}

static CicResult unpack(const unsigned char *image, size_t len, const char *out_dir,
                        void (*progress)(int, int, void *), void *user) {
    /* find "wwgT)H" signature */
    static const unsigned char sig[6] = {0x77,0x77,0x67,0x54,0x29,0x48};
    size_t pos = 0, sig_at = (size_t)-1;
    if (len < 6) return CIC_ERR_SIGNATURE;
    for (size_t i = 0; i + 6 <= len; ++i) {
        if (memcmp(image + i, sig, 6) == 0) { sig_at = i; break; }
    }
    if (sig_at == (size_t)-1) return CIC_ERR_SIGNATURE;
    pos = sig_at + 6;

    size_t data_block = (size_t)-1;
    unsigned char *file_list = NULL; size_t file_list_len = 0;

    /* block walk */
    for (;;) {
        if (pos + 64 > len) break;
        Cur c = { image, pos, len, 0 };
        uint16_t id = rd16(&c); skip(&c, 2); uint32_t bsize = rd32(&c);
        if (c.bad) break;
        size_t dstart = c.p;
        if (bsize > len - dstart) break;         /* truncated block */
        if (id == BLOCK_FILE_DATA) {
            data_block = dstart;
        } else if (id == BLOCK_FILE_LIST) {
            /* [u32 decSize][u8 comp][payload]; DEFLATE(1): skip 2, raw deflate */
            if (dstart + 5 <= len) {
                uint32_t dec = (uint32_t)image[dstart] | ((uint32_t)image[dstart+1] << 8) |
                               ((uint32_t)image[dstart+2] << 16) | ((uint32_t)image[dstart+3] << 24);
                unsigned char comp = image[dstart + 4];
                if (comp == 1 && dec > 0 && dec <= CIC_MAX_BLOCK) {
                    unsigned char *out = (unsigned char *)malloc(dec);
                    if (!out) { free(file_list); return CIC_ERR_MEMORY; }
                    int got = inflate_deflate(image + dstart + 7, len - (dstart + 7), out, dec);
                    if (got == (int)dec) { free(file_list); file_list = out; file_list_len = dec; }
                    else free(out);
                }
            }
        }
        pos = dstart + bsize;
    }

    if (data_block == (size_t)-1) return CIC_ERR_FORMAT;
    if (!file_list) return CIC_ERR_FORMAT;

    /* file list header: [u16 count][skip 2] */
    Cur c = { file_list, 0, file_list_len, 0 };
    uint16_t count = rd16(&c); skip(&c, 2);
    if (c.bad) { free(file_list); return CIC_ERR_FORMAT; }   /* count is u16, <= CIC_MAX_FILES */

    CicFile *files = (CicFile *)calloc(count ? count : 1, sizeof(CicFile));
    if (!files) { free(file_list); return CIC_ERR_MEMORY; }
    int nfiles = 0;
    for (uint16_t i = 0; i < count; ++i) {
        size_t ns = c.p;
        Cur n = { file_list, ns, file_list_len, 0 };
        uint32_t node_size = rd32(&n);
        uint16_t type = rd16(&n);
        if (n.bad) break;
        size_t node_end = ns + node_size;   /* nodeEnd = nodeStart + nodeSize */
        if (node_end > file_list_len) break;
        if (type == 0) {
            skip(&n, 3);
            unsigned char e2 = 0;
            if (!at(file_list, file_list_len, n.p, &e2)) { break; }
            n.p += 1;
            CicFile f; memset(&f, 0, sizeof(f));
            if (e2 == 0xE2) {
                skip(&n, 30);
            } else {
                skip(&n, 14);
                f.unc = rd32(&n); f.off = rd32(&n); f.comp = rd32(&n);
                skip(&n, 4);
                skip(&n, 24); /* times */
            }
            /* path: from n.p to node_end, up to first 0 */
            if (n.bad || n.p > node_end) { break; }
            size_t plen = node_end - n.p;
            if (plen >= CIC_PATH_MAX) plen = CIC_PATH_MAX - 1;
            memcpy(f.path, file_list + n.p, plen);
            f.path[plen] = 0;
            for (size_t k = 0; k < plen; ++k) { if (f.path[k] == 0) { f.path[k] = 0; break; } }
            if (f.unc <= CIC_MAX_TOTAL) files[nfiles++] = f;
        }
        c.p = node_end;
    }
    free(file_list);

    /* extract */
    mkdirs(out_dir);
    CicResult rc = CIC_OK;
    size_t total_out = 0;
    for (int i = 0; i < nfiles; ++i) {
        CicFile *f = &files[i];
        char rel[CIC_PATH_MAX]; size_t rl = 0;
        for (size_t k = 0; f->path[k] && rl + 1 < sizeof(rel); ++k)
            rel[rl++] = (f->path[k] == '\\') ? '/' : f->path[k];
        rel[rl] = 0;
        if (rl == 0 || rel[0] == '/' || strstr(rel, "..")) { rc = CIC_ERR_FORMAT; break; }

        char full[CIC_PATH_MAX * 2];
        if (snprintf(full, sizeof(full), "%s/%s", out_dir, rel) >= (int)sizeof(full)) { rc = CIC_ERR_FORMAT; break; }
        /* ensure parent dir */
        char *slash = strrchr(full, '/');
        if (slash) { *slash = 0; mkdirs(full); *slash = '/'; }

        FILE *out = fopen(full, "wb");
        if (!out) { rc = CIC_ERR_IO; break; }
        if (f->unc == 0) { fclose(out); }
        else {
            size_t base = data_block + (size_t)f->off + 4;
            unsigned char method = 0;
            if (base >= len || !at(image, len, base, &method)) { fclose(out); rc = CIC_ERR_FORMAT; break; }
            unsigned char *buf = (unsigned char *)malloc(f->unc);
            if (!buf) { fclose(out); rc = CIC_ERR_MEMORY; break; }
            int got = -1;
            if (method == 1)      got = inflate_deflate(image + base + 3, len - (base + 3), buf, f->unc);
            else if (method == 2) got = inflate_bzip2(image + base + 1, len - (base + 1), buf, f->unc);
            else if (method == 0) { memcpy(buf, image + base + 1, f->unc); got = (int)f->unc; }
            if (got != (int)f->unc) { free(buf); fclose(out); rc = CIC_ERR_CODEC; break; }
            total_out += f->unc;
            if (total_out > CIC_MAX_TOTAL) { free(buf); fclose(out); rc = CIC_ERR_MEMORY; break; }
            if (fwrite(buf, 1, f->unc, out) != f->unc) { free(buf); fclose(out); rc = CIC_ERR_IO; break; }
            free(buf);
            fclose(out);
        }
        if (progress) progress(i + 1, nfiles, user);
    }
    free(files);
    return rc;
}

CicResult cic_unpack_memory(const unsigned char *image, size_t image_len, const char *out_dir,
                            void (*progress)(int, int, void *), void *user) {
    if (!image || !image_len) return CIC_ERR_OPEN;
    return unpack(image, image_len, out_dir, progress, user);
}

CicResult cic_unpack_file(const char *installer_path, const char *out_dir,
                          void (*progress)(int, int, void *), void *user) {
    FILE *f = fopen(installer_path, "rb");
    if (!f) return CIC_ERR_OPEN;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return CIC_ERR_OPEN; }
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return CIC_ERR_OPEN; }
    rewind(f);
    unsigned char *buf = (unsigned char *)malloc((size_t)sz);
    if (!buf) { fclose(f); return CIC_ERR_MEMORY; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); return CIC_ERR_IO; }
    CicResult r = unpack(buf, got, out_dir, progress, user);
    free(buf);
    return r;
}

const char *cic_result_string(CicResult r) {
    switch (r) {
        case CIC_OK:            return "ok";
        case CIC_ERR_OPEN:      return "cannot open installer";
        case CIC_ERR_SIGNATURE: return "not a Clickteam installer";
        case CIC_ERR_FORMAT:    return "malformed installer data";
        case CIC_ERR_MEMORY:    return "out of memory";
        case CIC_ERR_CODEC:     return "decompression failed";
        case CIC_ERR_IO:        return "cannot write output file";
    }
    return "unknown error";
}
