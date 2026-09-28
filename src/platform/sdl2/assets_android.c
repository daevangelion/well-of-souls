#ifdef __ANDROID__
#include "platform/platform.h"
#include "install.h"
#include <SDL.h>
#include <SDL_system.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PATH_CAP 4096
#define MANIFEST_MAX (8 * 1024 * 1024)

static int make_path(char *out, const char *root, const char *name)
{
    int n = snprintf(out, PATH_CAP, "%s/%s", root, name);
    return n < 0 || n >= PATH_CAP ? -1 : 0;
}

static int asset_name_valid(const char *name)
{
    const char *p = name;
    if (!*p || *p == '/' || strchr(p, '\\')) return 0;
    while (*p) {
        const char *end = strchr(p, '/');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (!n || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.')) return 0;
        if (!end) return 1;
        p = end + 1;
    }
    return 0;
}

static int copy_asset(const char *root, const char *name, unsigned long long expected)
{
    char path[PATH_CAP], temp[PATH_CAP], asset[PATH_CAP], buffer[65536];
    SDL_RWops *input;
    FILE *output;
    char *p;
    unsigned long long total = 0;
    size_t n;
    int failed = 0;
    if (!asset_name_valid(name) || make_path(path, root, name) ||
        make_path(asset, "wos-data", name)) return -1;
    if (snprintf(temp, sizeof(temp), "%s.part", path) >= (int)sizeof(temp)) return -1;
    for (p = path + strlen(root) + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = '\0';
        failed = plat_mkdir(path);
        *p = '/';
        if (failed) return -1;
    }
    input = SDL_RWFromFile(asset, "rb");
    if (!input) return -1;
    if (SDL_RWsize(input) < 0 || (unsigned long long)SDL_RWsize(input) != expected) {
        SDL_RWclose(input);
        return -1;
    }
    output = fopen(temp, "wb");
    if (!output) { SDL_RWclose(input); return -1; }
    while ((n = SDL_RWread(input, buffer, 1, sizeof(buffer))) != 0) {
        if (n > expected - total || fwrite(buffer, 1, n, output) != n) { failed = 1; break; }
        total += n;
    }
    if (fclose(output)) failed = 1;
    SDL_RWclose(input);
    if (!failed && total == expected && rename(temp, path) == 0) return 0;
    remove(temp);
    return -1;
}

/* Decode a user-supplied installer (installer-supplied.bin) into the data dir. When
 * wait_ms > 0, poll for the file to appear first: the app's supply UI runs on the UI
 * thread while this SDL thread blocks, so the user can pick/download the installer and
 * we pick it up and decode it here. Returns 0 on success, -1 otherwise. */
static int install_from_supplied(const char *supplied, const char *data, int wait_ms)
{
    int waited = 0;
    for (;;) {
        FILE *sf = fopen(supplied, "rb");
        if (sf) {
            fclose(sf);
            return wos_install_from_file(supplied, data, NULL, NULL);
        }
        if (waited >= wait_ms) return -1;
        {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 250L * 1000L * 1000L;
            nanosleep(&ts, NULL);
        }
        waited += 250;
    }
}

/* SDL's Android entry point has established its AssetManager before calling SDL_main. */
int wos_android_paths(char *data, char *save, size_t capacity)
{
    const char *root = SDL_AndroidGetInternalStoragePath();
    char marker[PATH_CAP], temporary[PATH_CAP], line[PATH_CAP + 100];
    SDL_RWops *rw;
    Sint64 length;
    char *manifest, *cursor;
    FILE *file;
    int n, result = -1;
    if (!root || capacity < PATH_CAP) return -1;
    if (make_path(data, root, "data") || make_path(save, root, "saves") ||
        plat_mkdir(data) || plat_mkdir(save) || make_path(marker, root, "installed-manifest.txt") ||
        make_path(temporary, root, "installed-manifest.part")) return -1;
    /* The game data need not be bundled. Boot succeeds if the data is already
     * installed, or a user-supplied installer (installer-supplied.bin, written by
     * WosActivity via the SAF picker or an HTTP download) is present. Only if the APK
     * bundles a manifest do we fall back to extracting the assets from it. */
    if (wos_data_installed(data)) return 0;
    {
        char supplied[PATH_CAP];
        int have = make_path(supplied, root, "installer-supplied.bin") == 0;
        SDL_RWops *probe;
        if (have && install_from_supplied(supplied, data, 0) == 0) return 0;
        if (!have) return -1;
        /* If the APK bundles the data (a manifest asset is present), extract from it
         * below. Otherwise the data is supplied at runtime: wait for the app to write
         * installer-supplied.bin (the supply UI runs on the UI thread while this SDL
         * thread blocks) and decode it. */
        probe = SDL_RWFromFile("wos-manifest.txt", "rb");
        if (!probe) return install_from_supplied(supplied, data, 300000);
        SDL_RWclose(probe);
    }
    rw = SDL_RWFromFile("wos-manifest.txt", "rb");
    if (!rw) return -1;
    length = SDL_RWsize(rw);
    if (length <= 0 || length > MANIFEST_MAX) { SDL_RWclose(rw); return -1; }
    manifest = SDL_malloc((size_t)length + 1);
    if (!manifest) { SDL_RWclose(rw); return -1; }
    if (SDL_RWread(rw, manifest, 1, (size_t)length) != (size_t)length) {
        SDL_RWclose(rw); SDL_free(manifest); return -1;
    }
    SDL_RWclose(rw);
    manifest[length] = '\0';
    file = fopen(marker, "rb");
    if (file) {
        size_t offset = 0, got;
        while ((got = fread(line, 1, sizeof(line), file)) != 0) {
            if (got > (size_t)length - offset || memcmp(line, manifest + offset, got)) break;
            offset += got;
        }
        result = !ferror(file) && !got && offset == (size_t)length ? 0 : -1;
        fclose(file);
        if (!result) { SDL_free(manifest); return 0; }
    }
    for (cursor = manifest; *cursor;) {
        char *end = strchr(cursor, '\n'), *size, *name, *number_end;
        unsigned long long bytes;
        size_t len;
        if (!end) goto done;
        len = (size_t)(end - cursor);
        if (len >= sizeof(line)) goto done;
        memcpy(line, cursor, len); line[len] = '\0';
        size = strchr(line, '\t');
        if (!size || size - line != 64) goto done;
        name = strchr(++size, '\t');
        if (!name) goto done;
        *name++ = '\0';
        if (*size < '0' || *size > '9') goto done;
        errno = 0;
        bytes = strtoull(size, &number_end, 10);
        if (errno || *number_end || copy_asset(data, name, bytes)) goto done;
        cursor = end + 1;
    }
    file = fopen(temporary, "wb");
    if (!file) goto done;
    n = fwrite(manifest, 1, (size_t)length, file) == (size_t)length ? 0 : -1;
    if (fclose(file)) n = -1;
    if (!n && !rename(temporary, marker)) result = 0;
    else remove(temporary);
 done:
    SDL_free(manifest);
    if (result) SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Unable to install WoS game assets: %s", SDL_GetError());
    return result;
}
#endif
