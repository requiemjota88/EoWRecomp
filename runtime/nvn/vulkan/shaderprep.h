// STATUS: interface only - shaderprep.c is NOT written yet, nothing includes this header (see DEVELOPMENT_STATUS.md, stage 3).
// Shader preparation (shaderprep.c): translates and compiles the game's shaders AHEAD of play and fills the SPIR-V
// cache (spvcache.h), instead of waiting for the renderer to need each one.
//
// What it works from - the *corpus*: a local journal, cache/shaders/corpus.bin, in which the running game records every
// shader program (the Maxwell code of its stages + the translation key) the first time it uses it. Together with the
// user's own game that is the only place the program binaries exist, so the corpus lives on the user's machine and is
// never shipped. From it the preparation can rebuild the whole SPIR-V cache without starting the game: after an app
// update changed the translator, a new GPU or driver, a wiped cache. The same step is used one program at a time by the
// renderer to compile a program the moment the game loads it (prewarm) rather than at its first draw.
//
// Properties:
//  - skips what is already compiled and valid (a "done" table records, per program and for the current environment,
//    the SPIR-V keys it produced; they are re-checked in the cache, so a deleted or corrupt entry is rebuilt);
//  - detects new programs (the corpus grows) and compiles only those;
//  - interruptible and resumable: every finished program is journalled at once, `cancel` stops within one program;
//  - failures are remembered (a program the translator rejects is not retried every start);
//  - no Vulkan dependency: the compiler is a function pointer (vk_glsl_to_spirv, or a test double).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../maxwell/shader.h"

typedef uint32_t *(*SpvCompileFn)(const char *src, int stage, const char *name, size_t *nwords);  // malloc'd, NULL = error

// What the translator needs around a call (exec.c owns the translator: it isn't re-entrant, and sets the guest heap
// window and the loop guard before translating). lock/unlock/setup may be NULL (a private lock, no setup).
typedef struct {
    void (*lock)(void);
    void (*unlock)(void);
    void (*setup)(void);
} ShaderPrepHooks;
void shaderprep_set_hooks(const ShaderPrepHooks *h);

enum { PREP_GRAPHICS = 1, PREP_COMPUTE = 2 };

// ---- the corpus
// dir: e.g. "cache/shaders". env: identifies the translator + compiler (a change invalidates the "done" table, not the
// corpus). false: unusable directory (everything below then does nothing).
bool shaderprep_open(const char *dir, const char *env);
void shaderprep_close(void);
// Records a program (the code bytes are copied into the corpus if new). Graphics: vs + fs with their keys; compute: cs
// in code0, len1 = 0. Cheap for a known program. Thread-safe.
void shaderprep_record(int kind, const uint8_t *code0, size_t len0, const uint8_t *code1, size_t len1, const ShaderKey *vkey,
                       const ShaderKey *fkey);
uint64_t shaderprep_programs(void);  // programs in the corpus

// ---- one program (also what the workers run)
// Translates and compiles the program, storing the SPIR-V in the cache. false: why says what failed. `keys` receives
// the SPIR-V cache keys of the stages (compute: keys[1] = 0).
bool shaderprep_build(int kind, const uint8_t *code0, size_t len0, const uint8_t *code1, size_t len1, const ShaderKey *vkey,
                      const ShaderKey *fkey, SpvCompileFn compile, uint64_t keys[2], char *why, size_t why_size);

// ---- the whole corpus
typedef struct {
    uint64_t total;     // programs to look at
    uint64_t done;      // looked at so far (skipped + compiled + failed)
    uint64_t skipped;   // already compiled and still valid
    uint64_t compiled;  // built now
    uint64_t failed;    // translation or compilation failed
    bool running;
} ShaderPrepProgress;

// Programs that still need work in the current environment (not done, or their cached SPIR-V is gone).
uint64_t shaderprep_pending(void);

typedef struct ShaderPrep ShaderPrep;
// Starts a preparation on `threads` worker threads (0: automatic). NULL if there is nothing to do or it can't start.
ShaderPrep *shaderprep_start(SpvCompileFn compile, int threads);
void shaderprep_progress(const ShaderPrep *p, ShaderPrepProgress *out);
void shaderprep_cancel(ShaderPrep *p);  // finishes the program in hand, then stops; a later start resumes
void shaderprep_wait(ShaderPrep *p);    // joins the workers
void shaderprep_free(ShaderPrep *p);
