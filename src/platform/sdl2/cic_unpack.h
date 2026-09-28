/* Native Clickteam Install Creator (CIC) installer decoder.
 *
 * Unpacks the retail Well of Souls installer (a Clickteam Install Creator PE stub
 * with a "wwgT)H" overlay) into a directory tree, on-device, without cicdec/Mono.
 * The format and this port were validated byte-for-byte against cicdec 3.0.1's
 * output for the real installer (875 files, installer version 40).
 *
 * The overlay is a sequence of blocks: [u16 id][skip u16][u32 size][payload]. The
 * FILE_LIST block holds a deflate-compressed node table; the FILE_DATA block holds
 * the file contents, each individually DEFLATE (raw, 2-byte header skipped),
 * BZip2, or stored. This file links the vendored miniz + bzip2 decoders.
 */
#ifndef WOS_CIC_UNPACK_H
#define WOS_CIC_UNPACK_H

#include <stddef.h>

/* Result codes. 0 on full success. Negative on failure. */
typedef enum {
    CIC_OK            =  0,
    CIC_ERR_OPEN      = -1, /* cannot read installer */
    CIC_ERR_SIGNATURE = -2, /* not a CIC installer (no "wwgT)H" overlay) */
    CIC_ERR_FORMAT    = -3, /* malformed block/file-list structure */
    CIC_ERR_MEMORY    = -4, /* out of memory */
    CIC_ERR_CODEC     = -5, /* decompress failure (deflate/bzip2) */
    CIC_ERR_IO        = -6  /* cannot create/write an output file */
} CicResult;

/* Unpack a CIC installer to out_dir (created if missing; subdirectories as needed).
 * Only the extracted game payload is written (the installer's own resources/strings
 * are skipped). Paths inside the installer are backslash-separated and become '/'.
 *
 * When progress is non-NULL it is called as progress(done, total) after each file.
 * Returns CIC_OK if every payload file was written, else the first error. */
CicResult cic_unpack_file(const char *installer_path, const char *out_dir,
                          void (*progress)(int done, int total, void *user), void *user);

/* Same, but unpacks an in-memory installer image (used by the Android URL download
 * and SAF flows, which obtain the installer as bytes). */
CicResult cic_unpack_memory(const unsigned char *image, size_t image_len, const char *out_dir,
                            void (*progress)(int done, int total, void *user), void *user);

/* Human-readable message for a CicResult. */
const char *cic_result_string(CicResult r);

#endif /* WOS_CIC_UNPACK_H */
