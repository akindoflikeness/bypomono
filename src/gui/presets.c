/* dladdr needs the GNU extensions visible before any libc header */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "app.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

#define PATHBUF 1024
#define JOINBUF (PATHBUF + 320)
/* realpath may fill its destination to PATH_MAX, and a fortified libc
   aborts outright when the destination is provably smaller, however short
   the resolved path turns out to be. Every realpath destination gets this. */
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#if PATH_MAX > PATHBUF
#define RESOLVEDBUF PATH_MAX
#else
#define RESOLVEDBUF PATHBUF
#endif
#define STOCK_LEDGER ".stock-seen"
#define DRAINED_LEDGER ".drained-from"
#define SELECTED_WORD "selected"
#define ARG_SEP " - "

/* ---------- path helpers ---------- */

static bool path_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static bool is_dir_path(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool mkdir_p(const char *path) {
    char buf[PATHBUF];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof buf) return false;
    memcpy(buf, path, n + 1);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(buf, 0755) != 0 && errno != EEXIST) return false;
        *p = '/';
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST) return false;
    return is_dir_path(buf);
}

static char *read_all(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || (unsigned long)n > SESSION_JSON_MAX) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    return buf;
}

/* writes beside the target and renames over it, so a failed write leaves
   the old file whole instead of truncated */
static bool write_all(const char *path, const char *text) {
    char tmp[JOINBUF];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return false;
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    size_t n = strlen(text);
    bool ok = fwrite(text, 1, n, f) == n;
    if (fflush(f) != 0) ok = false;
#if !defined(_WIN32)
    if (ok && fsync(fileno(f)) != 0) ok = false;
#endif
    if (fclose(f) != 0) ok = false;
#if defined(_WIN32)
    if (ok) ok = MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
#else
    if (ok) ok = rename(tmp, path) == 0;
#endif
    if (!ok) remove(tmp);
    return ok;
}

static bool copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return false;
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    char buf[8192];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    if (ferror(in)) ok = false;
    fclose(in);
    if (fclose(out) != 0) ok = false;
    return ok;
}

static bool remove_dir_all(const char *path) {
    DIR *d = opendir(path);
    if (!d) return false;
    struct dirent *e;
    bool ok = true;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char full[PATHBUF];
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        if (is_dir_path(full)) {
            if (!remove_dir_all(full)) ok = false;
        } else if (unlink(full) != 0) {
            ok = false;
        }
    }
    closedir(d);
    if (rmdir(path) != 0) ok = false;
    return ok;
}

static void trim_into(const char *raw, char *out, size_t out_len) {
    const char *s = raw;
    while (*s && isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) n--;
    if (n >= out_len) n = out_len - 1;
    memcpy(out, s, n);
    out[n] = 0;
}

/* ---------- directory resolution ---------- */

/* full path of the running executable, or -1 */
static int exe_path(char *buf, size_t len) {
#if defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", buf, len - 1);
    if (n <= 0) return -1;
    buf[n] = 0;
    return 0;
#elif defined(__APPLE__)
    uint32_t n = (uint32_t)len;
    if (_NSGetExecutablePath(buf, &n) != 0) return -1;
    char real[RESOLVEDBUF];
    if (realpath(buf, real)) snprintf(buf, len, "%s", real);
    return 0;
#elif defined(_WIN32)
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)len);
    if (n == 0 || n >= len) return -1;
    for (char *p = buf; *p; p++)
        if (*p == '\\') *p = '/';
    return 0;
#else
    (void)buf; (void)len;
    return -1;
#endif
}

/* strip one trailing "/segment"; false if nothing is left */
static bool strip_last(char *path) {
    char *slash = strrchr(path, '/');
    if (!slash || slash == path) return false;
    *slash = 0;
    return true;
}

static const char *exe_dir(void) {
    static char buf[PATHBUF];
    static int state = 0; /* 0 unknown, 1 ok, -1 fail */
    if (state == 0) {
        if (exe_path(buf, sizeof buf) == 0 && strip_last(buf)) {
            state = 1;
#if defined(__APPLE__)
            /* inside an .app bundle the binary sits at
               Name.app/Contents/MacOS/; assets/ and presets/ travel beside
               the .app, so anchor on the bundle's parent folder */
            size_t n = strlen(buf);
            const char *tail = "/Contents/MacOS";
            size_t tn = strlen(tail);
            if (n > tn && strcmp(buf + n - tn, tail) == 0) {
                char up[PATHBUF];
                snprintf(up, sizeof up, "%s", buf);
                if (strip_last(up) && strip_last(up) && strip_last(up))
                    snprintf(buf, sizeof buf, "%s", up);
            }
#endif
        } else {
            state = -1;
        }
    }
    return state == 1 ? buf : NULL;
}

/* full path of the module this code is linked into. exe_path() names the
   host process, which for a plugin is the DAW; dladdr and
   GetModuleHandleEx-from-address name the .clap itself. */
static int module_path(char *buf, size_t len) {
#if defined(_WIN32)
    HMODULE h = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(uintptr_t)&module_path, &h))
        return -1;
    DWORD n = GetModuleFileNameA(h, buf, (DWORD)len);
    if (n == 0 || n >= len) return -1;
    for (char *p = buf; *p; p++)
        if (*p == '\\') *p = '/';
    return 0;
#else
    Dl_info info;
    if (!dladdr((void *)(uintptr_t)&module_path, &info) || !info.dli_fname)
        return -1;
    char real[RESOLVEDBUF];
    if (realpath(info.dli_fname, real))
        snprintf(buf, len, "%s", real);
    else
        snprintf(buf, len, "%s", info.dli_fname);
    return 0;
#endif
}

/* where resources sit relative to that module: inside a macOS bundle under
   Contents/Resources, everywhere else beside the file */
static const char *module_res_dir(void) {
    static char buf[PATHBUF];
    static int state = 0; /* 0 unknown, 1 ok, -1 fail */
    if (state == 0) {
        state = -1;
        if (module_path(buf, sizeof buf) == 0 && strip_last(buf)) {
            state = 1;
#if defined(__APPLE__)
            size_t n = strlen(buf);
            const char *tail = "/Contents/MacOS";
            size_t tn = strlen(tail);
            if (n > tn && strcmp(buf + n - tn, tail) == 0)
                snprintf(buf + n - tn, sizeof buf - (n - tn), "%s",
                         "/Contents/Resources");
#endif
        }
    }
    return state == 1 ? buf : NULL;
}

/* first root that holds a directory called leaf; NULL roots are skipped */
static bool first_dir_with(char out[PATHBUF], const char *leaf,
                           const char *const *roots, int n) {
    for (int i = 0; i < n; i++) {
        if (!roots[i]) continue;
        char cand[JOINBUF];
        snprintf(cand, sizeof cand, "%s/%s", roots[i], leaf);
        if (is_dir_path(cand)) {
            snprintf(out, PATHBUF, "%.1023s", cand);
            return true;
        }
    }
    return false;
}

static const char *stock_dir(void) {
    static char buf[PATHBUF];
    static bool done = false;
    if (!done) {
        const char *roots[] = {module_res_dir(), exe_dir()};
        if (!first_dir_with(buf, "presets", roots, 2)) {
            if (exe_dir()) snprintf(buf, sizeof buf, "%s/presets", exe_dir());
            else snprintf(buf, sizeof buf, "presets");
        }
        done = true;
    }
    return buf;
}

const char *user_data_root(void) {
    static char buf[PATHBUF];
    static int state = 0;
    if (state == 0) {
#if defined(_WIN32)
        const char *base = getenv("APPDATA");
        if (!base) base = getenv("USERPROFILE");
        if (base) {
            snprintf(buf, sizeof buf, "%s/bypo", base);
            for (char *p = buf; *p; p++)
                if (*p == '\\') *p = '/';
            state = 1;
        } else {
            state = -1;
        }
#elif defined(__APPLE__)
        const char *home = getenv("HOME");
        if (home) {
            snprintf(buf, sizeof buf, "%s/Library/Application Support/bypo",
                     home);
            state = 1;
        } else {
            state = -1;
        }
#else
        const char *xdg = getenv("XDG_DATA_HOME");
        const char *home = getenv("HOME");
        /* a flatpak host rewrites XDG_DATA_HOME to its app-private .var
           dir; ignore it so the plugin shares the standalone's data home */
        if (xdg && strstr(xdg, "/.var/app/")) xdg = NULL;
        if (xdg && xdg[0] == '/') {
            snprintf(buf, sizeof buf, "%s/bypo", xdg);
            state = 1;
        } else if (home) {
            snprintf(buf, sizeof buf, "%s/.local/share/bypo", home);
            state = 1;
        } else {
            state = -1;
        }
#endif
    }
    return state == 1 ? buf : NULL;
}

const char *preset_dir(void) {
    static char buf[PATHBUF];
    static bool done = false;
    if (!done) {
        const char *root = user_data_root();
        bool ok = false;
        if (root) {
            snprintf(buf, sizeof buf, "%s/presets", root);
            ok = mkdir_p(buf);
        }
        if (!ok) snprintf(buf, sizeof buf, "%s", stock_dir());
        done = true;
    }
    return buf;
}

static bool parse_user_dir(const char *text, const char *key, char *out,
                           size_t out_len) {
    const char *home = getenv("HOME");
    if (!home) home = "";
    size_t key_len = strlen(key);
    const char *line = text;
    while (line && *line) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        char raw[PATHBUF];
        if (len >= sizeof raw) len = sizeof raw - 1;
        memcpy(raw, line, len);
        raw[len] = 0;
        char t[PATHBUF];
        trim_into(raw, t, sizeof t);
        line = end ? end + 1 : NULL;
        if (t[0] == '#') continue;
        if (strncmp(t, key, key_len) != 0 || t[key_len] != '=') continue;
        char val[PATHBUF];
        trim_into(t + key_len + 1, val, sizeof val);
        size_t vn = strlen(val);
        while (vn > 0 && val[0] == '"') {
            memmove(val, val + 1, vn);
            vn--;
        }
        while (vn > 0 && val[vn - 1] == '"') val[--vn] = 0;
        if (vn == 0) return false;
        char expanded[PATHBUF];
        if (strncmp(val, "$HOME/", 6) == 0) {
            snprintf(expanded, sizeof expanded, "%s/%s", home, val + 6);
        } else if (strcmp(val, "$HOME") == 0) {
            return false;
        } else {
            snprintf(expanded, sizeof expanded, "%s", val);
        }
        if (expanded[0] != '/') return false;
        snprintf(out, out_len, "%s", expanded);
        return true;
    }
    return false;
}

static bool music_dir(char *out, size_t out_len) {
#if defined(_WIN32)
    const char *profile = getenv("USERPROFILE");
    if (!profile) return false;
    snprintf(out, out_len, "%s/Music", profile);
    for (char *p = out; *p; p++)
        if (*p == '\\') *p = '/';
    return true;
#else
    const char *home = getenv("HOME");
    if (home) {
        char cfg[PATHBUF];
        snprintf(cfg, sizeof cfg, "%s/.config/user-dirs.dirs", home);
        char *text = read_all(cfg);
        if (text) {
            bool got = parse_user_dir(text, "XDG_MUSIC_DIR", out, out_len);
            free(text);
            if (got) return true;
        }
        snprintf(out, out_len, "%s/Music", home);
        return true;
    }
    return false;
#endif
}

const char *recording_dir(void) {
    static char buf[PATHBUF];
    static bool done = false;
    if (!done) {
        char cand[PATHBUF];
        bool got = false;
        if (music_dir(cand, sizeof cand)) {
            char full[JOINBUF];
            snprintf(full, sizeof full, "%s/BYPO", cand);
            if (mkdir_p(full)) {
                snprintf(buf, sizeof buf, "%.1023s", full);
                got = true;
            }
        }
        if (!got && user_data_root()) {
            char full[JOINBUF];
            snprintf(full, sizeof full, "%s/recordings", user_data_root());
            if (mkdir_p(full)) {
                snprintf(buf, sizeof buf, "%.1023s", full);
                got = true;
            }
        }
        if (!got) snprintf(buf, sizeof buf, ".");
        done = true;
    }
    return buf;
}

const char *asset_dir(void) {
    static char buf[PATHBUF];
    static bool done = false;
    if (!done) {
        /* the module this code is linked into comes first, so a plugin finds
           the assets that ship with it rather than the host's */
        const char *roots[] = {module_res_dir(), ".", exe_dir(),
                               user_data_root()};
        if (!first_dir_with(buf, "assets", roots, 4))
            snprintf(buf, sizeof buf, "assets");
        done = true;
    }
    return buf;
}

/* ---------- names ---------- */

void sanitise_segment(const char *raw, char *out, size_t out_len) {
    if (out_len == 0) return;
    out[0] = 0;
    char t[256];
    trim_into(raw, t, sizeof t);
    size_t n = strlen(t);
    if (n == 0) return;
    if (strcmp(t, ".") == 0 || strcmp(t, "..") == 0) return;
    if (strchr(t, '/') || strchr(t, '\\')) return;
    bool underscore = false;
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < out_len; i++) {
        unsigned char c = (unsigned char)t[i];
        if (isspace(c)) {
            if (!underscore && o > 0) out[o++] = '_';
            underscore = true;
            continue;
        }
        if (!(isalnum(c) || c >= 0x80 || c == '-' || c == '_')) c = '-';
        out[o++] = (char)c;
        underscore = c == '_';
    }
    out[o] = '\0';
}

/* a path segment must stay one segment: no separators, no . or .. */
static bool segment_ok(const char *seg) {
    return seg[0] && strcmp(seg, ".") != 0 && strcmp(seg, "..") != 0 &&
           !strchr(seg, '/') && !strchr(seg, '\\');
}

bool preset_path(const PresetRef *r, char *out, size_t out_len) {
    int n;
    if (out_len) out[0] = 0;
    if (!segment_ok(r->name)) return false;
    if (r->bank[0] && !segment_ok(r->bank)) return false;
    if (r->bank[0])
        n = snprintf(out, out_len, "%s/%s/%s.json", preset_dir(), r->bank,
                     r->name);
    else
        n = snprintf(out, out_len, "%s/%s.json", preset_dir(), r->name);
    return n > 0 && (size_t)n < out_len;
}

const char *preset_bank_label(const PresetRef *r) {
    return r->bank[0] ? r->bank : MINE_BANK;
}

void preset_qualified(const PresetRef *r, char *out, size_t out_len) {
    snprintf(out, out_len, "%s/%s", preset_bank_label(r), r->name);
}

static bool refs_equal(const PresetRef *a, const PresetRef *b) {
    return strcmp(a->bank, b->bank) == 0 && strcmp(a->name, b->name) == 0;
}

static PresetRef ref_make(const char *bank, const char *name) {
    PresetRef r;
    snprintf(r.bank, sizeof r.bank, "%.63s", bank ? bank : "");
    snprintf(r.name, sizeof r.name, "%.127s", name);
    return r;
}

static bool is_view_name(const char *folder) {
    return strcasecmp(folder, "ALL") == 0 || strcasecmp(folder, MINE_BANK) == 0;
}

/* ---------- scanning ---------- */

static bool preset_stem(const char *fname, char *out, size_t out_len) {
    size_t n = strlen(fname);
    if (n < 6 || strcmp(fname + n - 5, ".json") != 0) return false;
    size_t stem = n - 5;
    if (stem == 5 && strncmp(fname, "state", 5) == 0) return false;
    if (stem >= out_len) return false;
    memcpy(out, fname, stem);
    out[stem] = 0;
    return segment_ok(out);
}

static int ref_cmp(const void *pa, const void *pb) {
    const PresetRef *a = pa, *b = pb;
    bool la = a->bank[0] == 0, lb = b->bank[0] == 0;
    if (la != lb) return la ? -1 : 1;
    int c = strcmp(a->bank, b->bank);
    if (c) return c;
    return strcmp(a->name, b->name);
}

static int scan_presets_dir(const char *dir, PresetRef *out, int max) {
    int count = 0;
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && count < max) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            char full[JOINBUF];
            snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
            if (strcmp(e->d_name, TRASH_DIR) == 0) continue;
            if (is_dir_path(full)) {
                DIR *inner = opendir(full);
                if (!inner) continue;
                struct dirent *f;
                while ((f = readdir(inner)) && count < max) {
                    char ifull[JOINBUF + 300];
                    snprintf(ifull, sizeof ifull, "%s/%s", full, f->d_name);
                    char stem[128];
                    if (!is_dir_path(ifull) &&
                        preset_stem(f->d_name, stem, sizeof stem))
                        out[count++] = ref_make(e->d_name, stem);
                }
                closedir(inner);
            } else {
                char stem[128];
                if (preset_stem(e->d_name, stem, sizeof stem))
                    out[count++] = ref_make(NULL, stem);
            }
        }
        closedir(d);
    }
    qsort(out, (size_t)count, sizeof *out, ref_cmp);
    return count;
}

static int str64_cmp(const void *pa, const void *pb) {
    return strcmp((const char *)pa, (const char *)pb);
}

static int scan_folders_dir(const char *dir, char out[][64], int max) {
    int count = 0;
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && count < max) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            char full[JOINBUF];
            snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
            if (strcmp(e->d_name, TRASH_DIR) == 0) continue;
            if (is_dir_path(full))
                snprintf(out[count++], 64, "%.63s", e->d_name);
        }
        closedir(d);
    }
    qsort(out, (size_t)count, 64, str64_cmp);
    return count;
}

/* ---------- migration + first-run sync ---------- */

enum { LEDGER_MAX = 2048 };

static int copy_presets(const char *from, const char *to, bool overwrite) {
    DIR *d = opendir(from);
    if (!d) return 0;
    if (!mkdir_p(to)) {
        closedir(d);
        return 0;
    }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (len < 5 || strcmp(e->d_name + len - 5, ".json") != 0 ||
            strcmp(e->d_name, "state.json") == 0)
            continue;
        char src[PATHBUF], dst[PATHBUF];
        snprintf(src, sizeof src, "%s/%s", from, e->d_name);
        snprintf(dst, sizeof dst, "%s/%s", to, e->d_name);
        if (!overwrite && path_exists(dst)) continue;
        if (copy_file(src, dst)) n++;
    }
    closedir(d);
    return n;
}

static int sync_stock_bank(const char *stock, const char *dir) {
    char ledger_path[JOINBUF];
    snprintf(ledger_path, sizeof ledger_path, "%s/%s", dir, STOCK_LEDGER);
    char(*seen)[128] = malloc(LEDGER_MAX * 128);
    if (!seen) return 0;
    int seen_n = 0;
    char *text = read_all(ledger_path);
    if (text) {
        char *line = text;
        while (line && *line && seen_n < LEDGER_MAX) {
            char *end = strchr(line, '\n');
            if (end) *end = 0;
            if (line[0]) snprintf(seen[seen_n++], 128, "%s", line);
            line = end ? end + 1 : NULL;
        }
        free(text);
    }

    char stock_bank[JOINBUF], dir_bank[JOINBUF];
    snprintf(stock_bank, sizeof stock_bank, "%s/%s", stock, STOCK_BANK);
    snprintf(dir_bank, sizeof dir_bank, "%s/%s", dir, STOCK_BANK);
    DIR *d = opendir(stock_bank);
    if (!d) {
        /* STOCK_BANK names a folder on disk, not just a label: rename the
           define without renaming presets/<name> and the stock bank stops
           being found. Say so — silently shipping no presets is the one
           failure here that looks like nothing happened. */
        fprintf(stderr, "presets: no stock bank at %s (STOCK_BANK is \"%s\")\n",
                stock_bank, STOCK_BANK);
        free(seen);
        return 0;
    }

    bool first_run = !path_exists(ledger_path);
    if (first_run && is_dir_path(dir_bank)) {
        FILE *f = fopen(ledger_path, "wb");
        if (f) {
            struct dirent *e;
            bool first = true;
            while ((e = readdir(d))) {
                size_t len = strlen(e->d_name);
                if (len < 5 || strcmp(e->d_name + len - 5, ".json") != 0 ||
                    strcmp(e->d_name, "state.json") == 0)
                    continue;
                fprintf(f, "%s%s", first ? "" : "\n", e->d_name);
                first = false;
            }
            fclose(f);
        }
        closedir(d);
        free(seen);
        return 0;
    }

    int synced = 0;
    bool ledger_changed = false;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (len < 5 || strcmp(e->d_name + len - 5, ".json") != 0 ||
            strcmp(e->d_name, "state.json") == 0)
            continue;
        bool known = false;
        for (int i = 0; i < seen_n; i++)
            if (strcmp(seen[i], e->d_name) == 0) {
                known = true;
                break;
            }
        if (known) continue;

        /* v1.2 tightened stock names to underscores. If this exact preset is
           in the old ledger under its spaced spelling, rename the installed
           copy and update the ledger instead of installing a duplicate. */
        char legacy[128];
        snprintf(legacy, sizeof legacy, "%s", e->d_name);
        for (char *p = legacy; *p && strcmp(p, ".json") != 0; p++)
            if (*p == '_') *p = ' ';
        int legacy_seen = -1;
        for (int i = 0; i < seen_n; i++)
            if (strcmp(seen[i], legacy) == 0) legacy_seen = i;
        if (legacy_seen >= 0) {
            char old_path[JOINBUF + 300], new_path[JOINBUF + 300];
            snprintf(old_path, sizeof old_path, "%s/%s", dir_bank, legacy);
            snprintf(new_path, sizeof new_path, "%s/%s", dir_bank, e->d_name);
            if (path_exists(old_path) && !path_exists(new_path)
                && rename(old_path, new_path) == 0) {
                snprintf(seen[legacy_seen], 128, "%s", e->d_name);
                ledger_changed = true;
                synced++;
                continue;
            }
        }

        char src[JOINBUF + 300], dst[JOINBUF + 300];
        snprintf(src, sizeof src, "%s/%s", stock_bank, e->d_name);
        snprintf(dst, sizeof dst, "%s/%s", dir_bank, e->d_name);
        if (mkdir_p(dir_bank) && copy_file(src, dst)) synced++;
        if (seen_n < LEDGER_MAX) snprintf(seen[seen_n++], 128, "%s", e->d_name);
        ledger_changed = true;
    }
    closedir(d);
    if (ledger_changed) {
        FILE *f = fopen(ledger_path, "wb");
        if (f) {
            for (int i = 0; i < seen_n; i++)
                fprintf(f, "%s%s", i ? "\n" : "", seen[i]);
            fclose(f);
        }
    }
    free(seen);
    return synced;
}

/* the walk prepare_preset_dir just did, waiting for the first rescan */
static struct {
    bool fresh;
    char dir[PATHBUF];
    int presets;
    int folders;
    PresetRef names[MAX_PRESETS];
    char banks[MAX_FOLDERS][64];
} opened;

static void scan_opened(const char *dir) {
    snprintf(opened.dir, sizeof opened.dir, "%s", dir);
    opened.presets = scan_presets_dir(dir, opened.names, MAX_PRESETS);
    opened.folders = scan_folders_dir(dir, opened.banks, MAX_FOLDERS);
    opened.fresh = true;
}

static void report_opened(const char *dir) {
    int nf = opened.presets;
    int nb = opened.folders;
    if (nf == 0) {
        printf("presets: NONE FOUND in %s — the library is empty\n", dir);
        return;
    }
    char counts[2048];
    size_t off = 0;
    counts[0] = 0;
    for (int i = 0; i < nb && off < sizeof counts; i++) {
        int n = 0;
        for (int j = 0; j < nf; j++)
            if (strcmp(opened.names[j].bank, opened.banks[i]) == 0) n++;
        off += (size_t)snprintf(counts + off, sizeof counts - off, "%s%s %d",
                                off ? ", " : "", opened.banks[i], n);
    }
    int loose = 0;
    for (int j = 0; j < nf; j++)
        if (opened.names[j].bank[0] == 0) loose++;
    if (loose > 0 && off < sizeof counts)
        snprintf(counts + off, sizeof counts - off, "%s%s %d", off ? ", " : "",
                 MINE_BANK, loose);
    printf("presets: %d in %s — %s\n", nf, dir, counts);
}

void prepare_preset_dir(void) {
    const char *dir = preset_dir();
    char stock[PATHBUF];
    snprintf(stock, sizeof stock, "%s", stock_dir());

    /* no stock folder beside the binary is normal: nothing to migrate or sync */
    if (strcmp(dir, stock) != 0 && is_dir_path(stock)) {
        char ledger_path[JOINBUF];
        snprintf(ledger_path, sizeof ledger_path, "%s/%s", dir, DRAINED_LEDGER);
        char *drained = read_all(ledger_path);
        char key[RESOLVEDBUF];
        if (!realpath(stock, key)) snprintf(key, sizeof key, "%s", stock);
        bool done = false;
        if (drained) {
            char *line = drained;
            while (line && *line) {
                char *end = strchr(line, '\n');
                if (end) *end = 0;
                if (strcmp(line, key) == 0) done = true;
                if (end) *end = '\n';
                line = end ? end + 1 : NULL;
                if (done) break;
            }
        }
        if (!done) {
            int moved = copy_presets(stock, dir, false);
            static char banks[MAX_FOLDERS][64];
            int nb = scan_folders_dir(stock, banks, MAX_FOLDERS);
            for (int i = 0; i < nb; i++) {
                if (strcmp(banks[i], STOCK_BANK) == 0) continue;
                char from[JOINBUF], to[JOINBUF];
                snprintf(from, sizeof from, "%s/%.63s", stock, banks[i]);
                snprintf(to, sizeof to, "%s/%.63s", dir, banks[i]);
                moved += copy_presets(from, to, false);
            }
            char state[JOINBUF], stock_state[JOINBUF];
            snprintf(state, sizeof state, "%s/state.json", dir);
            snprintf(stock_state, sizeof stock_state, "%s/state.json", stock);
            if (!path_exists(state) && copy_file(stock_state, state))
                printf("presets: took the last session across from %s\n", stock);
            if (moved > 0)
                printf(
                    "presets: brought %d across from %s — an old cwd-anchored "
                    "store\n",
                    moved, stock);
            FILE *f = fopen(ledger_path, "wb");
            if (f) {
                if (drained) fputs(drained, f);
                fprintf(f, "%s\n", key);
                fclose(f);
            }
        }
        free(drained);

        int n = sync_stock_bank(stock, dir);
        if (n > 0)
            printf("presets: updated %d in %s from %s\n", n, STOCK_BANK,
                   stock);
    }

    /* opening the editor used to walk this tree here and again in
       preset_rescan. Keep the walk and hand it to that call. */
    scan_opened(dir);
    report_opened(dir);
}

/* ---------- session plumbing ---------- */

static const char INIT_JSON[] =
    "{\n"
    "  \"patch\": {\n"
    "    \"algorithm\": 15,\n"
    "    \"ratio_mode\": \"Fibonacci\",\n"
    "    \"ops\": [\n"
    "      {\n"
    "        \"enabled\": true,\n"
    "        \"ratio\": 1.0,\n"
    "        \"detune_cents\": 0.0,\n"
    "        \"level\": 1.0\n"
    "      },\n"
    "      {\n"
    "        \"enabled\": false,\n"
    "        \"ratio\": 1.0,\n"
    "        \"detune_cents\": 0.0,\n"
    "        \"level\": 1.0\n"
    "      },\n"
    "      {\n"
    "        \"enabled\": true,\n"
    "        \"ratio\": 2.0,\n"
    "        \"detune_cents\": 0.0,\n"
    "        \"level\": 1.0\n"
    "      },\n"
    "      {\n"
    "        \"enabled\": true,\n"
    "        \"ratio\": 3.0,\n"
    "        \"detune_cents\": 0.0,\n"
    "        \"level\": 1.0\n"
    "      },\n"
    "      {\n"
    "        \"enabled\": true,\n"
    "        \"ratio\": 5.0,\n"
    "        \"detune_cents\": 0.0,\n"
    "        \"level\": 1.0\n"
    "      }\n"
    "    ],\n"
    "    \"feedback\": 0.71,\n"
    "    \"index\": 0.24098434,\n"
    "    \"rip\": 0.2,\n"
    "    \"master_level\": 0.8,\n"
    "    \"glide_seconds\": 0.0012295281\n"
    "  },\n"
    "  \"verb\": {\n"
    "    \"mix\": 0.34,\n"
    "    \"ghost\": 0.24,\n"
    "    \"rt60\": 0.12,\n"
    "    \"damp\": 0.55,\n"
    "    \"haunt\": 0.45\n"
    "  },\n"
    "  \"melody\": {\n"
    "    \"enabled\": false,\n"
    "    \"tuning\": \"Scale\",\n"
    "    \"scale\": \"Phrygian\",\n"
    "    \"root_midi\": 28,\n"
    "    \"range_degrees\": 5,\n"
    "    \"rate_hz\": 5.5,\n"
    "    \"source\": \"GoldenWeyl\"\n"
    "  },\n"
    "  \"drone_hz\": 50.0,\n"
    "  \"warmth\": 0.5\n"
    "}\n";

static Session compiled_init(void) {
    Session s;
    if (!session_from_json(INIT_JSON, &s)) s = session_default();
    return s;
}

bool session_load_file(const char *path, Session *out) {
    char *text = read_all(path);
    if (!text) return false;
    bool ok = session_from_json(text, out);
    free(text);
    return ok;
}

Session app_session(const App *a) {
    Session s;
    s.patch = a->shadow;
    s.verb = a->shadow_verb;
    s.melody = a->shadow_melody;
    s.drone_hz = a->drone_hz;
    s.chandas = a->shadow_chandas;
    s.tempo_bpm = a->tempo_bpm;
    s.warmth = a->shadow_warmth;
    s.limiter_enabled = a->shadow_limiter_enabled;
    s.limiter_ceiling_db = a->shadow_limiter_ceiling_db;
    s.attack_s = a->shadow_attack_s;
    s.decay_s = a->shadow_decay_s;
    s.sustain = a->shadow_sustain;
    s.release_s = a->shadow_release_s;
    s.drone = a->engaged;
    s.mods = a->mods;
    s.pitch = a->shadow_pitch;
    return s;
}

void app_apply_session(App *a, Session s) {
    a->shadow = s.patch;
    a->shadow_verb = s.verb;
    a->shadow_melody = s.melody;
    a->shadow_chandas = s.chandas;
    a->tempo_bpm = s.tempo_bpm;
    a->shadow_warmth = s.warmth;
    a->shadow_limiter_enabled = s.limiter_enabled;
    a->shadow_limiter_ceiling_db = s.limiter_ceiling_db;
    a->drone_hz = s.drone_hz;
    a->shadow_attack_s = s.attack_s;
    a->shadow_decay_s = s.decay_s;
    a->shadow_sustain = s.sustain;
    a->shadow_release_s = s.release_s;
    /* s.drone is carried for the plugin's host state; loading a preset here
       must not start or stop the standalone's drone under the player */
    app_send(a, (Event){.kind = EV_SET_PATCH, .u.patch = a->shadow});
    app_send(a, (Event){.kind = EV_SET_VERB, .u.verb = a->shadow_verb});
    app_send(a, (Event){.kind = EV_SET_MELODY, .u.melody = a->shadow_melody});
    app_send(a,
             (Event){.kind = EV_SET_CHANDAS, .u.chandas = a->shadow_chandas});
    app_send(a, (Event){.kind = EV_SET_TEMPO, .u.f = a->tempo_bpm});
    app_send(a, (Event){.kind = EV_SET_WARMTH, .u.f = a->shadow_warmth});
    app_send(a, (Event){.kind = EV_SET_LIMITER,
                         .u.limiter = {a->shadow_limiter_enabled,
                                       a->shadow_limiter_ceiling_db}});
    app_send(a, (Event){.kind = EV_RESET_CHANDAS});
    app_send(a, (Event){.kind = EV_GLIDE_TO, .u.f = a->drone_hz});
    a->mods = s.mods;
    app_send_mods(a);
    a->shadow_pitch = s.pitch;
    pitch_send(a);
}

void app_save_state(App *a) {
    const char *dir = preset_dir();
    mkdir_p(dir);
    Session s = app_session(a);
    char *json = session_to_json(&s);
    if (json) {
        char path[JOINBUF];
        snprintf(path, sizeof path, "%s/state.json", dir);
        write_all(path, json);
        free(json);
    }
}

bool app_restore_state(App *a) {
    char state_path[JOINBUF];
    snprintf(state_path, sizeof state_path, "%s/state.json", preset_dir());
    Session s;
    bool restored = session_load_file(state_path, &s);
    bool from_init = false;
    if (!restored) {
        PresetRef def = ref_make(STOCK_BANK, DEFAULT_PRESET);
        char def_path[PATHBUF];
        preset_path(&def, def_path, sizeof def_path);
        from_init = path_exists(def_path);
        if (!session_load_file(def_path, &s)) s = compiled_init();
    }
    app_apply_session(a, s);
    a->restored = restored;
    if (restored)
        push_log(a, "previous session restored from state.json.");
    else if (from_init)
        push_log(a, "opened on '%s/%s'.", STOCK_BANK, DEFAULT_PRESET);
    return restored;
}

/* ---------- App-level flows ---------- */

void preset_rescan(App *a) {
    const char *dir = preset_dir();
    if (!opened.fresh || strcmp(opened.dir, dir) != 0) scan_opened(dir);
    a->preset_count = opened.presets;
    if (opened.presets > 0)
        memcpy(a->preset_names, opened.names,
               (size_t)opened.presets * sizeof(PresetRef));
    a->folder_count = opened.folders;
    if (opened.folders > 0)
        memcpy(a->preset_folders, opened.banks, (size_t)opened.folders * 64);
    opened.fresh = false;
}

static void clear_name_bar(App *a) {
    a->preset_name.len = 0;
    a->preset_name.text[0] = 0;
}

static bool filter_matches(const App *a, const PresetRef *p) {
    switch (a->preset_filter.kind) {
    case FILTER_ALL: return true;
    case FILTER_MINE: return strcmp(p->bank, STOCK_BANK) != 0;
    case FILTER_BANK: return strcmp(p->bank, a->preset_filter.bank) == 0;
    }
    return true;
}

static void forget_missing(App *a) {
    char path[PATHBUF];
    if (a->have_selected &&
        (!preset_path(&a->preset_selected, path, sizeof path) ||
         !path_exists(path)))
        a->have_selected = false;
    if (a->have_loaded &&
        (!preset_path(&a->preset_loaded, path, sizeof path) ||
         !path_exists(path)))
        a->have_loaded = false;
}

void preset_load(App *a, const PresetRef *r) {
    char path[PATHBUF];
    Session s;
    if (preset_path(r, path, sizeof path) && session_load_file(path, &s)) {
        app_apply_session(a, s);
        a->preset_loaded = *r;
        a->have_loaded = true;
        push_log(a, "preset '%s' loaded. algorithm %s / %s, %.1f hz.", r->name,
                 ROMAN[algorithm_index_of(&a->shadow)],
                 mode_name_of(a->shadow.ratio_mode), (double)a->drone_hz);
    } else {
        push_log(a, "preset '%s' unreadable — ignored.", r->name);
    }
}

static bool save_in(App *a, const char *folder, const char *name,
                    PresetRef *out) {
    char file[128];
    sanitise_segment(name, file, sizeof file);
    if (file[0] == 0) {
        push_log(a, "'%s' cannot be a preset name.", name);
        return false;
    }
    /* the bank is a path segment like any other, so it gets the same scrub */
    char bank[64];
    bank[0] = 0;
    if (folder && folder[0]) {
        sanitise_segment(folder, bank, sizeof bank);
        if (bank[0] == 0) {
            push_log(a, "'%s' cannot be a preset folder.", folder);
            return false;
        }
    }
    folder = bank[0] ? bank : NULL;
    char dir[JOINBUF];
    if (bank[0])
        snprintf(dir, sizeof dir, "%s/%.63s", preset_dir(), bank);
    else
        snprintf(dir, sizeof dir, "%s", preset_dir());
    if (!mkdir_p(dir)) {
        push_log(a, "preset directory could not be created.");
        return false;
    }
    PresetRef written = ref_make(folder, file);
    char path[PATHBUF];
    if (!preset_path(&written, path, sizeof path)) {
        push_log(a, "'%s' cannot be a preset name.", name);
        return false;
    }
    if (path_exists(path)) {
        char q[256];
        preset_qualified(&written, q, sizeof q);
        push_log(a, "'%s' already exists. use ow %s to replace it.", q, q);
        return false;
    }
    Session s = app_session(a);
    char *json = session_to_json(&s);
    if (!json) {
        push_log(a, "preset serialize failed.");
        return false;
    }
    bool ok = write_all(path, json);
    free(json);
    if (!ok) {
        push_log(a, "preset write failed: %s.", strerror(errno));
        return false;
    }
    preset_rescan(a);
    push_log(a, "preset '%s' written.", written.name);
    *out = written;
    return true;
}

void preset_save_in(App *a, const char *bank, const char *name) {
    PresetRef written;
    save_in(a, bank, name, &written);
}

bool preset_run_save(App *a, const char *name) {
    PresetRef written;
    if (!save_in(a, NULL, name, &written)) return false;
    if (a->preset_filter.kind != FILTER_ALL) {
        a->preset_filter.kind = FILTER_MINE;
        a->preset_filter.bank[0] = 0;
    }
    a->preset_selected = written;
    a->have_selected = true;
    a->preset_loaded = written;
    a->have_loaded = true;
    clear_name_bar(a);
    a->preset_searching = false;
    return true;
}

static bool resolve_preset(App *a, const char *name, PresetRef *out, char *err,
                           size_t err_len) {
    char want[256];
    trim_into(name, want, sizeof want);
    for (char *p = want; *p; p++) *p = (char)tolower((unsigned char)*p);
    if (strcmp(want, SELECTED_WORD) == 0) {
        if (a->have_selected) {
            *out = a->preset_selected;
            return true;
        }
        snprintf(err, err_len, "'%s' needs a preset highlighted first.",
                 SELECTED_WORD);
        return false;
    }
    PresetRef hits[MAX_PRESETS];
    int nh = 0;
    for (int pass = 0; pass < 2 && nh == 0; pass++) {
        for (int i = 0; i < a->preset_count; i++) {
            const PresetRef *p = &a->preset_names[i];
            if (pass == 0 && !filter_matches(a, p)) continue;
            char low[128];
            snprintf(low, sizeof low, "%s", p->name);
            for (char *q = low; *q; q++) *q = (char)tolower((unsigned char)*q);
            if (strcmp(low, want) == 0) hits[nh++] = *p;
        }
    }
    if (nh == 0) {
        snprintf(err, err_len, "no preset called '%s'.", name);
        return false;
    }
    if (nh == 1) {
        *out = hits[0];
        return true;
    }
    snprintf(err, err_len,
             "'%s' is in %d folders — narrow the browser to one first.", name,
             nh);
    return false;
}

/* mirrors parse_line's per-arg clean: SELECTED passthrough, else sanitise */
static bool clean_arg(App *a, const char *raw, char *out, size_t out_len) {
    char t[256];
    trim_into(raw, t, sizeof t);
    if (strcasecmp(t, SELECTED_WORD) == 0) {
        snprintf(out, out_len, "%s", SELECTED_WORD);
        return true;
    }
    sanitise_segment(t, out, out_len);
    if (out[0] == 0) {
        push_log(a, "'%s' cannot be a name — no slashes, and not empty.", t);
        return false;
    }
    return true;
}

/* splits args at the LAST " - " like parse_line's rsplitn */
static bool split_pair(const char *args, char *first, size_t first_len,
                       char *second, size_t second_len) {
    const char *last = NULL;
    for (const char *p = args; (p = strstr(p, ARG_SEP)); p += 1) last = p;
    if (!last) return false;
    size_t n = (size_t)(last - args);
    char raw1[256], raw2[256];
    if (n >= sizeof raw1) n = sizeof raw1 - 1;
    memcpy(raw1, args, n);
    raw1[n] = 0;
    snprintf(raw2, sizeof raw2, "%s", last + strlen(ARG_SEP));
    trim_into(raw1, first, first_len);
    trim_into(raw2, second, second_len);
    return true;
}

static void delete_preset(App *a, const PresetRef *preset) {
    char path[PATHBUF];
    preset_path(preset, path, sizeof path);
    if (remove(path) == 0) {
        preset_rescan(a);
        forget_missing(a);
        char q[256];
        preset_qualified(preset, q, sizeof q);
        push_log(a, "preset '%s' deleted.", q);
    } else {
        push_log(a, "preset delete failed: %s.", strerror(errno));
    }
}

void preset_run_delete(App *a, const char *args) {
    char t[256];
    trim_into(args ? args : "", t, sizeof t);
    PresetRef preset;
    if (t[0]) {
        char name[256];
        if (!clean_arg(a, t, name, sizeof name)) return;
        char err[256];
        if (!resolve_preset(a, name, &preset, err, sizeof err)) {
            push_log(a, "%s", err);
            return;
        }
    } else if (a->have_selected) {
        preset = a->preset_selected;
    } else {
        push_log(a, "no preset highlighted to delete.");
        return;
    }
    delete_preset(a, &preset);
}

void preset_run_overwrite(App *a, const char *args) {
    char name[256];
    if (!clean_arg(a, args, name, sizeof name)) return;
    PresetRef preset;
    char err[256];
    if (!resolve_preset(a, name, &preset, err, sizeof err)) {
        push_log(a, "%s", err);
        return;
    }
    Session s = app_session(a);
    char *json = session_to_json(&s);
    if (!json) {
        push_log(a, "preset serialize failed.");
        return;
    }
    char path[PATHBUF];
    preset_path(&preset, path, sizeof path);
    bool ok = write_all(path, json);
    free(json);
    if (ok) {
        preset_rescan(a);
        a->preset_selected = preset;
        a->have_selected = true;
        a->preset_loaded = preset;
        a->have_loaded = true;
        clear_name_bar(a);
        char q[256];
        preset_qualified(&preset, q, sizeof q);
        push_log(a, "'%s' overwritten with the current sound.", q);
    } else {
        push_log(a, "overwrite failed: %s.", strerror(errno));
    }
}

static void refile(App *a, const PresetRef *was, const PresetRef *now,
                   const char *where_to) {
    preset_rescan(a);
    clear_name_bar(a);
    if (a->have_selected && refs_equal(&a->preset_selected, was)) {
        a->preset_selected = *now;
    }
    if (a->have_loaded && refs_equal(&a->preset_loaded, was)) {
        a->preset_loaded = *now;
    }
    push_log(a, "'%s' → %s/%s.", was->name, where_to, now->name);
}

void preset_run_move(App *a, const char *args) {
    char raw_name[256], raw_folder[256];
    if (!split_pair(args, raw_name, sizeof raw_name, raw_folder,
                    sizeof raw_folder)) {
        push_log(a,
                 "move <preset> - <folder> — put a preset in a folder that "
                 "already exists.");
        return;
    }
    char name[256], folder[64];
    if (!clean_arg(a, raw_name, name, sizeof name)) return;
    if (!clean_arg(a, raw_folder, folder, sizeof folder)) return;
    if (strcasecmp(folder, STOCK_BANK) == 0) {
        push_log(a,
                 "'%s' is the shipped bank and an update rewrites it — file "
                 "it under one of yours.",
                 STOCK_BANK);
        return;
    }
    if (is_view_name(folder)) {
        push_log(a,
                 "'%s' is a chip in the row above, not a folder you can file "
                 "into.",
                 folder);
        return;
    }
    PresetRef preset;
    char err[256];
    if (!resolve_preset(a, name, &preset, err, sizeof err)) {
        push_log(a, "%s", err);
        return;
    }
    PresetRef dest = ref_make(folder, preset.name);
    if (refs_equal(&dest, &preset)) {
        push_log(a, "'%s' is already in %s.", preset.name, folder);
        return;
    }
    char dest_path[PATHBUF];
    preset_path(&dest, dest_path, sizeof dest_path);
    if (path_exists(dest_path)) {
        push_log(a, "%s already holds a '%s'.", folder, preset.name);
        return;
    }
    char folder_path[JOINBUF];
    snprintf(folder_path, sizeof folder_path, "%s/%s", preset_dir(), folder);
    if (!is_dir_path(folder_path)) {
        push_log(a, "no folder called '%s'. make it first: add %s", folder,
                 folder);
        return;
    }
    char src_path[PATHBUF];
    preset_path(&preset, src_path, sizeof src_path);
    if (rename(src_path, dest_path) == 0)
        refile(a, &preset, &dest, folder);
    else
        push_log(a, "move failed: %s.", strerror(errno));
}

void preset_run_rename(App *a, const char *args) {
    char raw_from[256], raw_to[256];
    if (!split_pair(args, raw_from, sizeof raw_from, raw_to, sizeof raw_to)) {
        push_log(a,
                 "rename <preset> - <new name> — give a preset a different "
                 "name, where it sits.");
        return;
    }
    char from[256], to[128];
    if (!clean_arg(a, raw_from, from, sizeof from)) return;
    if (!clean_arg(a, raw_to, to, sizeof to)) return;
    PresetRef preset;
    char err[256];
    if (!resolve_preset(a, from, &preset, err, sizeof err)) {
        push_log(a, "%s", err);
        return;
    }
    if (strcmp(preset.bank, STOCK_BANK) == 0) {
        push_log(a,
                 "'%s' is the shipped bank and an update rewrites it — save "
                 "your own copy instead.",
                 STOCK_BANK);
        return;
    }
    PresetRef dest = ref_make(preset.bank, to);
    if (refs_equal(&dest, &preset)) {
        push_log(a, "'%s' is what it is already called.", to);
        return;
    }
    char dest_path[PATHBUF];
    preset_path(&dest, dest_path, sizeof dest_path);
    if (path_exists(dest_path)) {
        push_log(a, "%s already holds a '%s'.", preset_bank_label(&preset), to);
        return;
    }
    char src_path[PATHBUF];
    preset_path(&preset, src_path, sizeof src_path);
    if (rename(src_path, dest_path) == 0)
        refile(a, &preset, &dest, preset_bank_label(&dest));
    else
        push_log(a, "rename failed: %s.", strerror(errno));
}

void preset_run_add(App *a, const char *args) {
    char folder[64];
    if (!clean_arg(a, args, folder, sizeof folder)) return;
    if (strcasecmp(folder, STOCK_BANK) == 0) {
        push_log(a,
                 "'%s' is the shipped bank's name and an update rewrites it — "
                 "pick another.",
                 STOCK_BANK);
        return;
    }
    if (is_view_name(folder)) {
        push_log(a,
                 "'%s' is a chip in the row above, not a folder — pick "
                 "another name.",
                 folder);
        return;
    }
    char path[JOINBUF];
    snprintf(path, sizeof path, "%s/%s", preset_dir(), folder);
    if (is_dir_path(path)) {
        push_log(a, "'%s' is already there.", folder);
        return;
    }
    if (mkdir_p(path)) {
        preset_rescan(a);
        clear_name_bar(a);
        push_log(a, "folder '%s' made. it is empty until you move something in.",
                 folder);
    } else {
        push_log(a, "folder '%s' could not be made: %s.", folder,
                 strerror(errno));
    }
}

void preset_run_remove(App *a, const char *args) {
    char folder[64];
    if (!clean_arg(a, args, folder, sizeof folder)) return;
    if (is_view_name(folder)) {
        push_log(a,
                 "'%s' is a view of the list, not a folder on disk — there is "
                 "nothing there to remove.",
                 folder);
        return;
    }
    char path[JOINBUF];
    snprintf(path, sizeof path, "%s/%s", preset_dir(), folder);
    if (!is_dir_path(path)) {
        push_log(a, "no folder called '%s'.", folder);
        return;
    }
    if (remove_dir_all(path)) {
        preset_rescan(a);
        clear_name_bar(a);
        if (a->preset_filter.kind == FILTER_BANK &&
            strcmp(a->preset_filter.bank, folder) == 0) {
            a->preset_filter.kind = FILTER_ALL;
            a->preset_filter.bank[0] = 0;
        }
        forget_missing(a);
        push_log(a, "folder '%s' and everything in it deleted.", folder);
    } else {
        push_log(a, "remove failed: %s.", strerror(errno));
    }
}

static int cycle_index(int here /* -1 = none */, int len, int by) {
    if (len == 0) return 0;
    if (here >= 0) {
        int i = (here + by) % len;
        if (i < 0) i += len;
        return i;
    }
    return by >= 0 ? 0 : len - 1;
}

void preset_cycle(App *a, bool forward) {
    int by = forward ? 1 : -1;
    int list[MAX_PRESETS];
    int n = 0;
    for (int i = 0; i < a->preset_count; i++)
        if (filter_matches(a, &a->preset_names[i])) list[n++] = i;
    if (n == 0) {
        push_log(a, "no presets to cycle through.");
        return;
    }
    int here = -1;
    if (a->have_loaded)
        for (int i = 0; i < n; i++)
            if (refs_equal(&a->preset_names[list[i]], &a->preset_loaded)) {
                here = i;
                break;
            }
    int next = cycle_index(here, n, by);
    PresetRef preset = a->preset_names[list[next]];
    a->preset_selected = preset;
    a->have_selected = true;
    preset_load(a, &preset);
}
