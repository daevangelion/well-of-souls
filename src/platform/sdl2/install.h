/* Runtime "supply the installer" orchestration.
 *
 * Obtains the retail installer (from a local file or raw bytes) and unpacks it into
 * the game-data directory with the native CIC decoder. This is the path used when the
 * game data is not bundled in the app: the user supplies the installer once and it is
 * decoded to internal storage. Not part of the desktop game binary; built into the
 * Android app (which links the decoder + codecs).
 *
 * Obtaining the bytes is the platform's job: on Android, WosActivity handles the SAF
 * file picker and HTTP download and writes the installer to a file, which this layer
 * then decodes. The file/memory entry points are host-testable.
 */
#ifndef WOS_INSTALL_H
#define WOS_INSTALL_H

#include <stddef.h>

/* progress(done, total, user) is called after each decoded file; done==0,total==0
 * means "starting". */
typedef void (*WosInstallProgress)(int done, int total, void *user);

int wos_install_from_memory(const unsigned char *bytes, size_t len, const char *data_dir,
                            WosInstallProgress progress, void *user);
int wos_install_from_file(const char *installer_path, const char *data_dir,
                          WosInstallProgress progress, void *user);

/* True if data_dir already looks like an installed game (Souls.exe present). */
int wos_data_installed(const char *data_dir);

/* Last human-readable error/status message (never NULL). */
const char *wos_install_message(void);

#endif /* WOS_INSTALL_H */
