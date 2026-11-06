// Persistent SPIR-V cache (spvcache.c): one file per compiled shader, named after the hash of its GLSL source, in a
// directory that belongs to one *environment* (translator version, shaderc / SPIR-V version, compile options).
//
//  - Every entry carries a header (format version, environment hash, key, size, checksum) and is checked before use:
//    a truncated, corrupted, foreign or stale file is deleted and counts as a miss, never as a result.
//  - The directory has a cache.id file. When it differs from the running environment, every entry is discarded.
//  - Writes go to a temporary file and replace the entry atomically, so a crash or a second process never leaves a
//    half-written shader behind and readers never see a partial one.
//  - Plain C with no Vulkan / SDL dependency, so tools/test_spvcache.c can exercise it on its own.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SPVCACHE_FORMAT 1

typedef struct {
    uint64_t hits, misses, stores, rejected, store_failed, purged;
} SpvCacheStats;

// Opens (creating it) the cache in dir for the environment `id`. If the directory was made for another id or format,
// all its entries are deleted first. false: the directory can't be used - the cache stays off and every call below
// degrades to "miss" / "not stored" (never fatal). Call once, before other threads use the cache.
bool spvcache_open(const char *dir, const char *id);
void spvcache_close(void);
bool spvcache_enabled(void);

// The key of a GLSL source for a stage (the entry's file name).
uint64_t spvcache_key(const char *src, size_t len, int stage);
// Hash used for keys and checksums (not cryptographic; it guards against corruption, not against tampering).
uint64_t spvcache_hash64(const void *p, size_t n, uint64_t seed);

// The cached SPIR-V (malloc'd, *nwords set) or NULL when absent or not valid (invalid entries are removed).
uint32_t *spvcache_load(uint64_t key, size_t *nwords);
// True if a structurally sound entry exists (header and size are checked; the payload is not read).
bool spvcache_has(uint64_t key);
// Stores a module. The SPIR-V is checked first (spvcache_check_spirv): garbage is refused.
bool spvcache_store(uint64_t key, int stage, const uint32_t *spv, size_t nwords);

// Structural SPIR-V check: magic, version, id bound, a well-formed instruction stream that ends exactly at the end,
// an OpMemoryModel and an OpEntryPoint. false: why says what's wrong.
bool spvcache_check_spirv(const uint32_t *spv, size_t nwords, char *why, size_t why_size);
// An additional check run on every module going into or out of the cache (the prepare tool and the tests plug the
// SPIRV-Tools validator in here). NULL removes it.
typedef bool (*SpvValidateFn)(const uint32_t *spv, size_t nwords, char *why, size_t why_size);
void spvcache_set_validator(SpvValidateFn fn);

SpvCacheStats spvcache_stats(void);
