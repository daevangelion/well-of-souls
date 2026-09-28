/* Host test for the native CIC decoder: decode a real installer to a directory.
 * Usage: cic_unpack_selftest <installer> <out_dir>
 * Prints the CicResult name and the file count; the caller diffs against the
 * cicdec reference tree. Not part of the game. */
#include "cic_unpack.h"
#include <stdio.h>

static void progress(int done, int total, void *user) { (void)done; (void)total; (void)user; }

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <installer> <out_dir>\n", argv[0]); return 2; }
    CicResult r = cic_unpack_file(argv[1], argv[2], progress, NULL);
    printf("%s (%d)\n", cic_result_string(r), (int)r);
    return r == CIC_OK ? 0 : 1;
}
