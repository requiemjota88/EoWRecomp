// Test of the persistent SPIR-V cache (runtime/nvn/vulkan/spvcache.c) on its own: no GPU, no game files.
// usage: test_spvcache <scratch dir>      (tools/test_spvcache.sh builds and runs it)
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir((p), 0755)
#endif

#include "../runtime/nvn/vulkan/spvcache.h"

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// A valid no-op compute shader; `bound` varies the content (and the module) per key.
static size_t make_module(uint32_t *w, uint32_t bound) {
    static const uint32_t body[] = {
        0x00020011, 1,                                   // OpCapability Shader
        0x0003000E, 0, 1,                                // OpMemoryModel Logical GLSL450
        0x0005000F, 5, 1, 0x6E69616D, 0,                 // OpEntryPoint GLCompute %1 "main"
        0x00060010, 1, 17, 1, 1, 1,                      // OpExecutionMode %1 LocalSize 1 1 1
        0x00020013, 2,                                   // OpTypeVoid %2
        0x00030021, 3, 2,                                // OpTypeFunction %3 %2
        0x00050036, 2, 1, 0, 3,                          // OpFunction %2 %1 None %3
        0x000200F8, 4,                                   // OpLabel %4
        0x000100FD,                                      // OpReturn
        0x00010038,                                      // OpFunctionEnd
    };
    uint32_t hdr[5] = {0x07230203, 0x00010000, 0, bound < 5 ? 5 : bound, 0};
    memcpy(w, hdr, sizeof hdr);
    memcpy(w + 5, body, sizeof body);
    return 5 + sizeof body / 4;
}

static void entry_file(char *out, size_t n, const char *dir, uint64_t key) {
    snprintf(out, n, "%s/%016llx.spv", dir, (unsigned long long)key);
}

static long file_size(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

static void poke(const char *path, long off, uint8_t xor_with) {
    FILE *f = fopen(path, "r+b");
    if (!f) return;
    fseek(f, off, SEEK_SET);
    int c = fgetc(f);
    fseek(f, off, SEEK_SET);
    fputc(c ^ xor_with, f);
    fclose(f);
}

static void truncate_to(const char *path, long n) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char *b = malloc((size_t)n);
    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    f = fopen(path, "wb");
    if (f) { fwrite(b, 1, got, f); fclose(f); }
    free(b);
}

static void wipe(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[600];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        remove(p);
    }
    closedir(d);
}

static int count_files(const char *dir, const char *suffix) {
    int n = 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t a = strlen(e->d_name), b = strlen(suffix);
        if (a >= b && !strcmp(e->d_name + a - b, suffix)) n++;
    }
    closedir(d);
    return n;
}

static bool reject_all(const uint32_t *spv, size_t n, char *why, size_t ws) {
    (void)spv, (void)n;
    snprintf(why, ws, "validator says no");
    return false;
}

// ---- concurrency: writers and readers on the same keys; a reader never sees a torn module
#define NT 6
#define NK 24
static const char *g_dir;
static void *hammer(void *arg) {
    int t = (int)(intptr_t)arg;
    uint32_t w[64];
    for (int round = 0; round < 200; round++) {
        uint32_t k = (uint32_t)(round * 7 + t) % NK;
        uint64_t key = spvcache_key("shader", 6, (int)k) ^ k;
        size_t n = make_module(w, 10 + k);
        if ((round + t) % 2) spvcache_store(key, 0, w, n);
        size_t got = 0;
        uint32_t *r = spvcache_load(key, &got);
        if (r) {
            if (got != n || memcmp(r, w, n * 4)) { g_fail++; fprintf(stderr, "FAIL: torn or wrong module read (key %u)\n", k); }
            free(r);
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <scratch dir>\n", argv[0]); return 2; }
    g_dir = argv[1];
    char dir[600], p[700];
    MKDIR(g_dir);
    snprintf(dir, sizeof dir, "%s/spirv", g_dir);
    MKDIR(dir);
    wipe(dir);
    uint32_t w[64], *r;
    size_t nw = make_module(w, 20), got = 0;

    // ---- hashing / keys
    uint64_t k_vs = spvcache_key("void main(){}", 13, 0), k_fs = spvcache_key("void main(){}", 13, 1);
    CHECK(k_vs != k_fs);                                                        // stage is part of the key
    CHECK(k_vs == spvcache_key("void main(){}", 13, 0));                        // deterministic
    CHECK(k_vs != spvcache_key("void main() {}", 14, 0));                       // content
    CHECK(spvcache_hash64("a", 1, 1) != spvcache_hash64("a", 1, 2));            // seed

    // ---- structural check
    char why[200];
    CHECK(spvcache_check_spirv(w, nw, why, sizeof why));
    uint32_t bad[64];
    memcpy(bad, w, nw * 4); bad[0] ^= 1;
    CHECK(!spvcache_check_spirv(bad, nw, why, sizeof why));                      // magic
    memcpy(bad, w, nw * 4); bad[1] = 0x00020000;
    CHECK(!spvcache_check_spirv(bad, nw, why, sizeof why));                      // version 2.0
    memcpy(bad, w, nw * 4); bad[5] &= 0xFFFF;                                    // an instruction of length 0
    CHECK(!spvcache_check_spirv(bad, nw, why, sizeof why));
    CHECK(!spvcache_check_spirv(w, nw - 3, why, sizeof why));                    // cut inside an instruction (OpLabel)
    memcpy(bad, w, nw * 4); bad[7] = (bad[7] & 0xFFFF0000u) | 0x1234;           // OpMemoryModel -> unknown op
    CHECK(!spvcache_check_spirv(bad, nw, why, sizeof why));
    memcpy(bad, w, nw * 4); bad[3] = 0;
    CHECK(!spvcache_check_spirv(bad, nw, why, sizeof why));                      // bound 0
    CHECK(!spvcache_check_spirv(w, 3, why, sizeof why));                         // shorter than a header
    CHECK(!spvcache_check_spirv(NULL, 0, why, sizeof why));

    // ---- disabled cache never fails the caller
    spvcache_close();
    CHECK(!spvcache_enabled());
    CHECK(spvcache_load(k_vs, &got) == NULL);
    CHECK(!spvcache_store(k_vs, 0, w, nw));
    CHECK(!spvcache_open("", "x"));

    // ---- store / has / load round trip
    CHECK(spvcache_open(dir, "env-A"));
    CHECK(spvcache_enabled());
    CHECK(!spvcache_has(k_vs));
    CHECK(spvcache_load(k_vs, &got) == NULL);                                    // miss
    CHECK(spvcache_store(k_vs, 0, w, nw));
    CHECK(spvcache_has(k_vs));
    r = spvcache_load(k_vs, &got);
    CHECK(r && got == nw && !memcmp(r, w, nw * 4));
    free(r);
    SpvCacheStats s = spvcache_stats();
    CHECK(s.stores == 1 && s.hits == 1 && s.misses == 1 && s.rejected == 0);
    CHECK(count_files(dir, ".tmp") == 0);                                        // no leftovers
    CHECK(!spvcache_store(k_fs, 1, bad, 3));                                     // garbage is refused
    CHECK(!spvcache_has(k_fs));
    memcpy(bad, w, nw * 4); bad[0] = 0;
    CHECK(!spvcache_store(k_fs, 1, bad, nw));
    CHECK(spvcache_stats().store_failed == 2);

    // ---- it persists across "runs" (same environment)
    CHECK(spvcache_open(dir, "env-A"));
    CHECK(spvcache_has(k_vs));
    r = spvcache_load(k_vs, &got);
    CHECK(r != NULL);
    free(r);

    // ---- corruption of every kind is detected and removed, never returned
    entry_file(p, sizeof p, dir, k_vs);
    long full = file_size(p);
    CHECK(full == (long)(48 + nw * 4));
    poke(p, 48 + 10, 0x40);                                                      // a bit flipped in the payload
    CHECK(spvcache_load(k_vs, &got) == NULL);
    CHECK(file_size(p) < 0);                                                     // and the bad file is gone
    CHECK(spvcache_store(k_vs, 0, w, nw));
    truncate_to(p, full - 8);                                                    // truncated (interrupted copy)
    CHECK(!spvcache_has(k_vs));
    CHECK(spvcache_load(k_vs, &got) == NULL);
    CHECK(spvcache_store(k_vs, 0, w, nw));
    poke(p, 0, 0xFF);                                                            // magic damaged
    CHECK(spvcache_load(k_vs, &got) == NULL);
    CHECK(spvcache_store(k_vs, 0, w, nw));
    { FILE *f = fopen(p, "ab"); fputc(0, f); fclose(f); }                        // trailing byte
    CHECK(spvcache_load(k_vs, &got) == NULL);
    { FILE *f = fopen(p, "wb"); fputs("not a shader", f); fclose(f); }           // garbage file
    CHECK(spvcache_load(k_vs, &got) == NULL);
    { FILE *f = fopen(p, "wb"); fclose(f); }                                     // empty file
    CHECK(spvcache_load(k_vs, &got) == NULL);
    CHECK(spvcache_store(k_vs, 0, w, nw));
    uint64_t other = k_vs ^ 0x1234;                                              // entry under another key's name
    char q[700];
    entry_file(q, sizeof q, dir, other);
    { FILE *a = fopen(p, "rb"), *b = fopen(q, "wb"); char buf[512]; size_t n = fread(buf, 1, sizeof buf, a); fwrite(buf, 1, n, b); fclose(a); fclose(b); }
    CHECK(spvcache_load(other, &got) == NULL);
    CHECK(spvcache_stats().rejected >= 7);
    r = spvcache_load(k_vs, &got);                                               // the good one is untouched
    CHECK(r != NULL);
    free(r);

    // ---- a validator (e.g. SPIRV-Tools) can veto in both directions
    spvcache_set_validator(reject_all);
    CHECK(!spvcache_store(k_fs, 1, w, nw));
    CHECK(spvcache_load(k_vs, &got) == NULL);                                    // veto on load drops the entry
    spvcache_set_validator(NULL);
    CHECK(spvcache_store(k_vs, 0, w, nw));

    // ---- environment change: everything is dropped; an entry smuggled back in is refused
    entry_file(p, sizeof p, dir, k_vs);
    char saved[600];
    snprintf(saved, sizeof saved, "%s/saved.bin", g_dir);
    { FILE *a = fopen(p, "rb"), *b = fopen(saved, "wb"); char buf[512]; size_t n = fread(buf, 1, sizeof buf, a); fwrite(buf, 1, n, b); fclose(a); fclose(b); }
    CHECK(spvcache_open(dir, "env-B (new translator)"));
    CHECK(count_files(dir, ".spv") == 0);
    CHECK(spvcache_stats().purged >= 1);
    CHECK(!spvcache_has(k_vs));
    { FILE *a = fopen(saved, "rb"), *b = fopen(p, "wb"); char buf[512]; size_t n = fread(buf, 1, sizeof buf, a); fwrite(buf, 1, n, b); fclose(a); fclose(b); }
    CHECK(!spvcache_has(k_vs));                                                  // header says env-A
    CHECK(spvcache_load(k_vs, &got) == NULL);
    remove(saved);

    // ---- a legacy directory (entries without cache.id, as the old cache wrote them) is purged
    spvcache_close();
    snprintf(p, sizeof p, "%s/cache.id", dir);
    remove(p);
    { snprintf(q, sizeof q, "%s/0123456789abcdef.spv", dir); FILE *f = fopen(q, "wb"); fputs("old format", f); fclose(f); }
    { snprintf(q, sizeof q, "%s/keep.me", dir); FILE *f = fopen(q, "wb"); fputs("not ours", f); fclose(f); }
    CHECK(spvcache_open(dir, "env-B (new translator)"));
    CHECK(count_files(dir, ".spv") == 0);
    CHECK(file_size(q) > 0);                                                     // files it doesn't own are left alone
    remove(q);

    // ---- many threads at once
    wipe(dir);
    spvcache_close();
    CHECK(spvcache_open(dir, "env-threads"));
    pthread_t th[NT];
    for (int i = 0; i < NT; i++) pthread_create(&th[i], NULL, hammer, (void *)(intptr_t)i);
    for (int i = 0; i < NT; i++) pthread_join(th[i], NULL);
    s = spvcache_stats();
    CHECK(s.rejected == 0);                                                      // no reader ever saw a partial file
    CHECK(count_files(dir, ".tmp") == 0);
    CHECK(count_files(dir, ".spv") > 0 && count_files(dir, ".spv") <= NK);

    wipe(dir);
    fprintf(stderr, "test_spvcache: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
