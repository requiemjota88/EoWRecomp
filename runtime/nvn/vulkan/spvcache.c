// Persistent SPIR-V cache: see spvcache.h.
#include "spvcache.h"

#include <dirent.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <direct.h>
#include <process.h>
#include <windows.h>
#define SPV_MKDIR(p) _mkdir(p)
#define SPV_GETPID() _getpid()
#else
#include <unistd.h>
#define SPV_MKDIR(p) mkdir((p), 0755)
#define SPV_GETPID() getpid()
#endif

#define MAGIC "EOWSPV01"
#define MAX_WORDS (16u << 20)  // 64 MB: far beyond any shader; keeps a corrupt size field from allocating gigabytes

typedef struct {
    char magic[8];
    uint32_t format, stage;
    uint64_t key, env;
    uint32_t nwords, reserved;
    uint64_t checksum;  // spvcache_hash64 of the words, seeded with the key
} Hdr;
_Static_assert(sizeof(Hdr) == 48, "SPIR-V cache entry header");

static char g_dir[512];
static uint64_t g_env;
static bool g_on;
static SpvValidateFn g_validate;
static _Atomic uint64_t st_hits, st_misses, st_stores, st_rejected, st_store_failed, st_purged;
static _Atomic uint32_t g_tmp_seq;
static _Atomic int g_logged;

#define LOGLIMIT(...) do { if (atomic_fetch_add(&g_logged, 1) < 30) fprintf(stderr, __VA_ARGS__); } while (0)

uint64_t spvcache_hash64(const void *p, size_t n, uint64_t seed) {
    const uint8_t *b = p;
    uint64_t h = seed ^ 0x9E3779B97F4A7C15ull ^ ((uint64_t)n * 0xff51afd7ed558ccdull);
    while (n >= 8) {
        uint64_t v;
        memcpy(&v, b, 8);
        v *= 0xff51afd7ed558ccdull;
        v ^= v >> 32;
        h = (h ^ v) * 0xc4ceb9fe1a85ec53ull;
        h = (h << 31) | (h >> 33);
        b += 8;
        n -= 8;
    }
    uint64_t v = 0;
    if (n) memcpy(&v, b, n);
    v *= 0xff51afd7ed558ccdull;
    v ^= v >> 32;
    h = (h ^ v) * 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return h;
}

uint64_t spvcache_key(const char *src, size_t len, int stage) {
    return spvcache_hash64(src, len, ((uint64_t)(uint32_t)stage << 32) | SPVCACHE_FORMAT);
}

void spvcache_set_validator(SpvValidateFn fn) { g_validate = fn; }

SpvCacheStats spvcache_stats(void) {
    return (SpvCacheStats){atomic_load(&st_hits), atomic_load(&st_misses), atomic_load(&st_stores),
                           atomic_load(&st_rejected), atomic_load(&st_store_failed), atomic_load(&st_purged)};
}

// ------------------------------------------------------------------ SPIR-V structure
bool spvcache_check_spirv(const uint32_t *w, size_t n, char *why, size_t why_size) {
#define BAD(msg) do { if (why && why_size) snprintf(why, why_size, "%s", msg); return false; } while (0)
    if (!w || n < 5) BAD("too short for a SPIR-V header");
    if (w[0] != 0x07230203u) BAD("bad SPIR-V magic");
    if ((w[1] >> 16) != 1 || ((w[1] >> 8) & 0xFF) > 6 || (w[1] & 0xFF)) BAD("unsupported SPIR-V version");
    uint32_t bound = w[3];
    if (bound == 0 || bound > (64u << 20)) BAD("implausible id bound");
    if (w[4] != 0) BAD("reserved header word set");
    bool memory_model = false, entry = false;
    size_t i = 5;
    while (i < n) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFF;
        if (wc == 0) BAD("zero-length instruction");
        if (wc > n - i) BAD("instruction runs past the end");
        if (op == 14) memory_model = true;  // OpMemoryModel
        if (op == 15) {                      // OpEntryPoint: execution model, function id, name...
            if (wc < 4 || w[i + 2] == 0 || w[i + 2] >= bound) BAD("bad entry point");
            entry = true;
        }
        i += wc;
    }
    if (i != n) BAD("instruction stream does not end at the end");
    if (!memory_model) BAD("no OpMemoryModel");
    if (!entry) BAD("no OpEntryPoint");
    return true;
#undef BAD
}

// ------------------------------------------------------------------ files
static void mkdir_p(const char *path) {
    char p[512];
    snprintf(p, sizeof p, "%s", path);
    for (char *c = p + 1; *c; c++)
        if (*c == '/' || *c == '\\') {
            char s = *c;
            *c = 0;
            SPV_MKDIR(p);
            *c = s;
        }
    SPV_MKDIR(p);
}

static bool replace_file(const char *tmp, const char *dst) {
#ifdef _WIN32
    return MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(tmp, dst) == 0;
#endif
}

static void entry_path(char *out, size_t n, uint64_t key) {
    snprintf(out, n, "%s/%016llx.spv", g_dir, (unsigned long long)key);
}

static bool write_file_atomic(const char *path, const void *data, size_t size) {
    char tmp[700];
    snprintf(tmp, sizeof tmp, "%s.%d.%u.tmp", path, (int)SPV_GETPID(), (unsigned)atomic_fetch_add(&g_tmp_seq, 1));
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, size, f) == size;
    ok = fflush(f) == 0 && ok;
    ok = fclose(f) == 0 && ok;
    if (!ok || !replace_file(tmp, path)) {
        remove(tmp);
        return false;
    }
    return true;
}

static uint64_t purge(void) {
    uint64_t n = 0;
    DIR *d = opendir(g_dir);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t len = strlen(e->d_name);
        bool spv = len > 4 && !strcmp(e->d_name + len - 4, ".spv");
        if (!spv && !strstr(e->d_name, ".tmp")) continue;  // only what this cache creates
        char p[800];  // (g_dir + a 255-byte name)
        snprintf(p, sizeof p, "%s/%s", g_dir, e->d_name);
        if (remove(p) == 0 && spv) n++;
    }
    closedir(d);
    return n;
}

bool spvcache_open(const char *dir, const char *id) {
    g_on = false;
    if (!dir || !*dir || !id || strlen(dir) > sizeof g_dir - 80) return false;
    snprintf(g_dir, sizeof g_dir, "%s", dir);
    mkdir_p(g_dir);
    char want[600];
    snprintf(want, sizeof want, "EOWSPV %d\n%s\n", SPVCACHE_FORMAT, id);
    g_env = spvcache_hash64(want, strlen(want), 0x5350564341434845ull);
    char idp[600], have[600] = {0};
    snprintf(idp, sizeof idp, "%s/cache.id", g_dir);
    FILE *f = fopen(idp, "rb");
    if (f) {
        size_t n = fread(have, 1, sizeof have - 1, f);
        have[n] = 0;
        fclose(f);
    }
    if (strcmp(have, want) != 0) {
        uint64_t dropped = purge();
        atomic_fetch_add(&st_purged, dropped);
        if (have[0] || dropped) fprintf(stderr, "[spvcache] %s: environment changed (now \"%s\"): %llu entries dropped\n", g_dir, id,
                                        (unsigned long long)dropped);
        if (!write_file_atomic(idp, want, strlen(want))) {
            fprintf(stderr, "[spvcache] %s is not writable: SPIR-V cache off\n", g_dir);
            return false;
        }
    }
    g_on = true;
    return true;
}

void spvcache_close(void) {
    g_on = false;
    g_validate = NULL;
    atomic_store(&st_hits, 0);
    atomic_store(&st_misses, 0);
    atomic_store(&st_stores, 0);
    atomic_store(&st_rejected, 0);
    atomic_store(&st_store_failed, 0);
    atomic_store(&st_purged, 0);
    atomic_store(&g_logged, 0);
}

bool spvcache_enabled(void) { return g_on; }

// Header fields that must hold for the entry of `key`; NULL if they do.
static const char *header_problem(const Hdr *h, uint64_t key) {
    if (memcmp(h->magic, MAGIC, 8)) return "bad magic";
    if (h->format != SPVCACHE_FORMAT) return "other format version";
    if (h->env != g_env) return "other environment";
    if (h->key != key) return "key mismatch";
    if (h->nwords < 5 || h->nwords > MAX_WORDS) return "implausible size";
    return NULL;
}

bool spvcache_has(uint64_t key) {
    if (!g_on) return false;
    char path[700];
    entry_path(path, sizeof path, key);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    Hdr h;
    bool ok = fread(&h, sizeof h, 1, f) == 1 && !header_problem(&h, key);
    if (ok) {
        ok = fseek(f, 0, SEEK_END) == 0 && ftell(f) == (long)(sizeof h + (size_t)h.nwords * 4);
    }
    fclose(f);
    return ok;
}

uint32_t *spvcache_load(uint64_t key, size_t *nwords) {
    if (!g_on) return NULL;
    char path[700];
    entry_path(path, sizeof path, key);
    FILE *f = fopen(path, "rb");
    if (!f) {
        atomic_fetch_add(&st_misses, 1);
        return NULL;
    }
    Hdr h;
    uint32_t *d = NULL;
    char reason[200];
    const char *why = NULL;
    if (fread(&h, sizeof h, 1, f) != 1) why = "short header";
    else if ((why = header_problem(&h, key)) != NULL) {}
    else if (!(d = malloc((size_t)h.nwords * 4))) why = "out of memory";
    else if (fread(d, 4, h.nwords, f) != h.nwords) why = "truncated";
    else if (fgetc(f) != EOF) why = "trailing data";
    fclose(f);
    if (!why && spvcache_hash64(d, (size_t)h.nwords * 4, key) != h.checksum) why = "checksum mismatch";
    if (!why && !spvcache_check_spirv(d, h.nwords, reason, sizeof reason)) why = reason;
    if (!why && g_validate && !g_validate(d, h.nwords, reason, sizeof reason)) why = reason;
    if (why) {
        atomic_fetch_add(&st_rejected, 1);
        LOGLIMIT("[spvcache] dropping %016llx.spv: %s\n", (unsigned long long)key, why);
        free(d);
        remove(path);
        return NULL;
    }
    atomic_fetch_add(&st_hits, 1);
    *nwords = h.nwords;
    return d;
}

bool spvcache_store(uint64_t key, int stage, const uint32_t *spv, size_t nwords) {
    if (!g_on) return false;
    char why[200], path[700];
    if (nwords > MAX_WORDS || !spvcache_check_spirv(spv, nwords, why, sizeof why) ||
        (g_validate && !g_validate(spv, nwords, why, sizeof why))) {
        atomic_fetch_add(&st_store_failed, 1);
        LOGLIMIT("[spvcache] not storing %016llx: %s\n", (unsigned long long)key, nwords > MAX_WORDS ? "too large" : why);
        return false;
    }
    size_t total = sizeof(Hdr) + nwords * 4;
    uint8_t *buf = malloc(total);
    if (!buf) return false;
    Hdr h = {.format = SPVCACHE_FORMAT, .stage = (uint32_t)stage, .key = key, .env = g_env, .nwords = (uint32_t)nwords,
             .checksum = spvcache_hash64(spv, nwords * 4, key)};
    memcpy(h.magic, MAGIC, 8);
    memcpy(buf, &h, sizeof h);
    memcpy(buf + sizeof h, spv, nwords * 4);
    entry_path(path, sizeof path, key);
    bool ok = write_file_atomic(path, buf, total);
    free(buf);
    atomic_fetch_add(ok ? &st_stores : &st_store_failed, 1);
    return ok;
}
