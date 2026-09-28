/* Host test for the runtime install orchestration: decode a real installer to a dir.
 * Usage: install_selftest <installer> <data_dir> */
#include "install.h"
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <installer> <data_dir>\n", argv[0]); return 2; }
    if (wos_install_from_file(argv[1], argv[2], NULL, NULL) != 0) {
        fprintf(stderr, "install failed: %s\n", wos_install_message());
        return 1;
    }
    printf("installed=%d message=%s\n", wos_data_installed(argv[2]), wos_install_message());
    return 0;
}
