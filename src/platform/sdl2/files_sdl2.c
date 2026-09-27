#include "platform/platform.h"
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <dirent.h>
#endif

#define PATH_CAP 4096
#define PATH_CACHE_SIZE 32
static struct { char request[PATH_CAP], actual[PATH_CAP]; } path_cache[PATH_CACHE_SIZE];
static unsigned cache_next;

static int equal_case(const char *a, const char *b)
{
    while (*a && *b) {
        unsigned char x = (unsigned char)*a++, y = (unsigned char)*b++;
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return 0;
    }
    return *a == *b;
}

static int append(char *out, const char *part)
{
    size_t n = strlen(out), m = strlen(part);
    if (m >= PATH_CAP - n) { errno = ENAMETOOLONG; return -1; }
    memcpy(out + n, part, m + 1);
    return 0;
}

static int match_component(const char *parent, const char *name, char *found)
{
#ifdef _WIN32
    WIN32_FIND_DATAA entry;
    HANDLE dir;
    char pattern[PATH_CAP] = "";
    if (append(pattern, *parent ? parent : ".") || append(pattern, "/*")) return -1;
    dir = FindFirstFileA(pattern, &entry);
    if (dir == INVALID_HANDLE_VALUE) return -1;
    do {
        if (equal_case(entry.cFileName, name)) {
            memcpy(found, entry.cFileName, strlen(entry.cFileName) + 1);
            FindClose(dir);
            return 0;
        }
    } while (FindNextFileA(dir, &entry));
    FindClose(dir);
#else
    struct dirent *entry;
    DIR *dir = opendir(*parent ? parent : ".");
    if (!dir) return -1;
    while ((entry = readdir(dir))) {
        if (equal_case(entry->d_name, name)) {
            memcpy(found, entry->d_name, strlen(entry->d_name) + 1);
            closedir(dir);
            return 0;
        }
    }
    closedir(dir);
#endif
    errno = ENOENT;
    return -1;
}

static int resolve_path(const char *path, char *out, int create_last)
{
    const char *p;
    char component[PATH_CAP], candidate[PATH_CAP], actual[PATH_CAP];
    struct stat st;
    if (!path || !*path) { errno = EINVAL; return -1; }
    if (strlen(path) >= PATH_CAP) { errno = ENAMETOOLONG; return -1; }
    out[0] = '\0';
    p = path;
#ifdef _WIN32
    if (p[0] && p[1] == ':') {
        out[0] = p[0]; out[1] = ':'; out[2] = '\0'; p += 2;
    }
    if ((p[0] == '/' || p[0] == '\\') && (p[1] == '/' || p[1] == '\\')) {
        if (append(out, "/")) return -1;
        ++p;
    }
#endif
    if (*p == '/' || *p == '\\') {
        if (append(out, "/")) return -1;
        ++p;
    }
    while (*p) {
        size_t n = 0, len;
        while (*p == '/' || *p == '\\') ++p;
        if (!*p) break;
        while (*p && *p != '/' && *p != '\\') component[n++] = *p++;
        component[n] = '\0';
        memcpy(candidate, out, strlen(out) + 1);
        len = strlen(candidate);
        if (len && candidate[len - 1] != '/' && candidate[len - 1] != ':') {
            if (append(candidate, "/")) return -1;
        }
        if (append(candidate, component)) return -1;
        if (stat(candidate, &st) == 0) {
            memcpy(out, candidate, strlen(candidate) + 1);
            continue;
        }
        if (match_component(out, component, actual)) {
            if (!create_last || *p) return -1;
            memcpy(actual, component, n + 1);
        }
        len = strlen(out);
        if (len && out[len - 1] != '/' && out[len - 1] != ':') {
            if (append(out, "/")) return -1;
        }
        if (append(out, actual)) return -1;
    }
    return 0;
}

FILE *plat_fopen(const char *path, const char *mode)
{
    char actual[PATH_CAP];
    FILE *file;
    unsigned i;
    if (!path || !mode || !*mode) { errno = EINVAL; return NULL; }
    for (i = 0; i < PATH_CACHE_SIZE; ++i) {
        if (!strcmp(path_cache[i].request, path)) {
            file = fopen(path_cache[i].actual, mode);
            if (file) return file;
            path_cache[i].request[0] = '\0';
            break;
        }
    }
    if (resolve_path(path, actual, mode[0] == 'w' || mode[0] == 'a')) return NULL;
    file = fopen(actual, mode);
    if (file) {
        i = cache_next++ % PATH_CACHE_SIZE;
        memcpy(path_cache[i].request, path, strlen(path) + 1);
        memcpy(path_cache[i].actual, actual, strlen(actual) + 1);
    }
    return file;
}

int plat_mkdir(const char *path)
{
    char actual[PATH_CAP];
    struct stat st;
    if (resolve_path(path, actual, 1)) return -1;
    if (stat(actual, &st) == 0) {
#ifdef _WIN32
        if ((st.st_mode & _S_IFMT) == _S_IFDIR) return 0;
#else
        if (S_ISDIR(st.st_mode)) return 0;
#endif
        errno = EEXIST;
        return -1;
    }
#ifdef _WIN32
    return _mkdir(actual);
#else
    return mkdir(actual, 0777);
#endif
}

int plat_list_dir(const char *path, void (*cb)(const char *, int, void *), void *user)
{
    char actual[PATH_CAP];
    int count = 0;
    if (resolve_path(path, actual, 0)) return -1;
#ifdef _WIN32
    {
        WIN32_FIND_DATAA entry;
        HANDLE dir;
        if (append(actual, "/*")) return -1;
        dir = FindFirstFileA(actual, &entry);
        if (dir == INVALID_HANDLE_VALUE) return -1;
        do {
            if (!strcmp(entry.cFileName, ".") || !strcmp(entry.cFileName, "..")) continue;
            if (cb) cb(entry.cFileName, !!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY), user);
            ++count;
        } while (FindNextFileA(dir, &entry));
        FindClose(dir);
    }
#else
    {
        struct dirent *entry;
        DIR *dir = opendir(actual);
        if (!dir) return -1;
        while ((entry = readdir(dir))) {
            char child[PATH_CAP];
            struct stat st;
            int is_dir;
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            memcpy(child, actual, strlen(actual) + 1);
            if (append(child, "/") || append(child, entry->d_name)) { closedir(dir); return -1; }
            is_dir = stat(child, &st) == 0 && S_ISDIR(st.st_mode);
            if (cb) cb(entry->d_name, is_dir, user);
            ++count;
        }
        closedir(dir);
    }
#endif
    return count;
}
