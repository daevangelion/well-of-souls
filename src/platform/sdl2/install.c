/* Runtime "supply the installer" orchestration (see install.h). */
#include "install.h"
#include "cic_unpack.h"
#include "platform/platform.h"

#include <stdlib.h>
#include <stdarg.h>

#define INSTALL_MAX_IMAGE (256u * 1024u * 1024u)  /* cap an installer image */

static char last_message[256] = "ok";

static void set_message(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(last_message, sizeof(last_message), fmt, ap);
    va_end(ap);
}
const char *wos_install_message(void) { return last_message; }

static int decoded_count;
static void on_progress(int done, int total, void *user) {
    WosInstallProgress cb = (WosInstallProgress)user;
    if (done > 0) decoded_count = done;
    if (cb) cb(done, total, NULL);
}

int wos_data_installed(const char *data_dir) {
    char path[4096];
    if (!data_dir || !*data_dir) return 0;
    snprintf(path, sizeof(path), "%s/Souls.exe", data_dir);
    return plat_fopen(path, "rb") != NULL;
}

int wos_install_from_memory(const unsigned char *bytes, size_t len, const char *data_dir,
                            WosInstallProgress progress, void *user) {
    if (!bytes || !len) { set_message("empty installer"); return 1; }
    if (!data_dir || !*data_dir) { set_message("no data directory"); return 1; }
    if (progress) progress(0, 0, NULL);
    decoded_count = 0;
    CicResult r = cic_unpack_memory(bytes, len, data_dir, on_progress, (void *)progress);
    if (r != CIC_OK) { set_message("unpack failed: %s", cic_result_string(r)); return 1; }
    set_message("installed %d files", decoded_count);
    (void)user;
    return 0;
}

int wos_install_from_file(const char *installer_path, const char *data_dir,
                          WosInstallProgress progress, void *user) {
    FILE *f;
    long sz;
    unsigned char *buf;
    size_t got;
    int rc;

    if (!installer_path || !*installer_path) { set_message("no installer path"); return 1; }
    f = fopen(installer_path, "rb");
    if (!f) { set_message("cannot open installer: %s", installer_path); return 1; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); set_message("cannot size installer"); return 1; }
    sz = ftell(f);
    if (sz <= 0 || (unsigned long)sz > INSTALL_MAX_IMAGE) { fclose(f); set_message("bad installer size"); return 1; }
    rewind(f);
    buf = (unsigned char *)malloc((size_t)sz);
    if (!buf) { fclose(f); set_message("out of memory"); return 1; }
    got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); set_message("short read on installer"); return 1; }
    rc = wos_install_from_memory(buf, got, data_dir, progress, user);
    free(buf);
    return rc;
}

