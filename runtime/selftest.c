// `eowrecomp.exe --selftest [--selftest-dll probe.dll]`: checks that the *package* works on a machine that has none of the
// build tools - the DLLs load, the video decoders are linked in, the Vulkan loader answers, the shader compiler compiles
// and the SPIR-V cache round-trips. It needs no game files and no GPU (a machine without a Vulkan device passes, with a
// note). The result goes to stdout and to selftest.log next to the exe (the release exe has no console), exit code 0/1.
// tools/windows/verify_package.sh runs it on the unpacked ZIP with a PATH that has no MSYS2 in it.
// Windows only: the other platforms are tested by their own scripts.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "paths.h"
#include "selftest.h"

#ifndef _WIN32
int rt_selftest(int argc, char **argv) {
    (void)argc, (void)argv;
    fprintf(stderr, "--selftest is only implemented for the Windows package\n");
    return 2;
}
#else
#include <SDL3/SDL.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <shaderc/shaderc.h>
#include <vulkan/vulkan.h>
#include <windows.h>

#include "cpu.h"
#include "nvn/vulkan/spvcache.h"

static FILE *g_log;
static int g_fail;
static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    fputs(b, stdout);
    fflush(stdout);
    if (g_log) fputs(b, g_log), fflush(g_log);
}
static void check(const char *what, int ok, const char *detail) {
    say("[%s] %s%s%s\n", ok ? "ok" : "FAIL", what, detail && *detail ? ": " : "", detail ? detail : "");
    if (!ok) g_fail++;
}

static void t_resources(void) {
    static const char *const need[] = {"eowgen/eowgen.exe", "zig/zig.exe", "zig/rt/link.rsp", "gamelib-src/cpu.h",
                                       "gamelib-src/gamelib_entry.c", "gamelib-src/hle_symbols.txt", "eowrecomp.dll.a",
                                       "Baloo2-SemiBold.ttf", "DejaVuSans.ttf"};
    for (size_t i = 0; i < sizeof need / sizeof *need; i++) {
        char p[1024];
        check(need[i], rt_resource(need[i], NULL, p, sizeof p), "");  // (a package file: no source-tree fallback)
    }
}

static void t_sdl(void) {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    int ok = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD) || SDL_Init(SDL_INIT_VIDEO);
    char d[256];
    snprintf(d, sizeof d, "SDL %d.%d.%d", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION);
    check("SDL3 initialises", ok, ok ? d : SDL_GetError());
    SDL_Quit();
}

static void t_ffmpeg(void) {
    say("[info] FFmpeg %s\n", av_version_info());
    check("FFmpeg license is LGPL (no GPL / nonfree parts)",
          !strstr(avcodec_license(), "nonfree") && !strstr(avcodec_license(), "GNU General") ? 1 : 0, avcodec_license());
    static const enum AVCodecID ids[] = {AV_CODEC_ID_VP9, AV_CODEC_ID_VORBIS, AV_CODEC_ID_OPUS, AV_CODEC_ID_H264, AV_CODEC_ID_AAC};
    for (size_t i = 0; i < sizeof ids / sizeof *ids; i++) {
        const AVCodec *c = avcodec_find_decoder(ids[i]);
        check(avcodec_get_name(ids[i]), c != NULL, c ? c->name : "decoder missing");
    }
    static const char *const dem[] = {"matroska", "mov"};
    for (size_t i = 0; i < sizeof dem / sizeof *dem; i++) check(dem[i], av_find_input_format(dem[i]) != NULL, "demuxer");
}

static void t_vulkan(void) {
    uint32_t ver = 0;
    PFN_vkEnumerateInstanceVersion f = NULL;
    HMODULE h = LoadLibraryA("vulkan-1.dll");
    check("vulkan-1.dll loads", h != NULL, "");
    if (!h) return;
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(h, "vkGetInstanceProcAddr");
    if (gipa) f = (PFN_vkEnumerateInstanceVersion)gipa(NULL, "vkEnumerateInstanceVersion");
    char d[128];
    if (f && f(&ver) == VK_SUCCESS) snprintf(d, sizeof d, "instance version %u.%u", VK_VERSION_MAJOR(ver), VK_VERSION_MINOR(ver));
    else snprintf(d, sizeof d, "no answer");
    check("Vulkan loader answers", f && ver >= VK_API_VERSION_1_1, d);
    // devices: informative (CI runners have no GPU). A device is listed, none is not a failure.
    PFN_vkCreateInstance ci = gipa ? (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance") : NULL;
    if (ci) {
        VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
        VkInstanceCreateInfo ic = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai};
        VkInstance inst = NULL;
        if (ci(&ic, NULL, &inst) == VK_SUCCESS) {
            uint32_t n = 0;
            ((PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices"))(inst, &n, NULL);
            say("[info] Vulkan physical devices: %u%s\n", n, n ? "" : " (none: the game needs a GPU with Vulkan 1.1+, this test does not)");
            ((PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance"))(inst, NULL);
        } else say("[info] vkCreateInstance failed (no driver) - not a package problem\n");
    }
}

static const char *kGlsl = "#version 450\nlayout(location=0) in vec4 a0;\nlayout(location=0) out vec4 o;\n"
                           "void main(){ o = a0 * 2.0; gl_Position = a0; }\n";

static void t_shaderc_and_cache(void) {
    shaderc_compiler_t c = shaderc_compiler_initialize();
    check("shaderc initialises", c != NULL, "");
    if (!c) return;
    shaderc_compile_options_t o = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(o, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    shaderc_compilation_result_t r = shaderc_compile_into_spv(c, kGlsl, strlen(kGlsl), shaderc_vertex_shader, "selftest", "main", o);
    int ok = shaderc_result_get_compilation_status(r) == shaderc_compilation_status_success;
    check("GLSL -> SPIR-V", ok, ok ? "" : shaderc_result_get_error_message(r));
    if (ok) {
        size_t nw = shaderc_result_get_length(r) / 4;
        uint32_t *spv = malloc(nw * 4);
        memcpy(spv, shaderc_result_get_bytes(r), nw * 4);
        char why[128] = "";
        check("SPIR-V is well formed", spvcache_check_spirv(spv, nw, why, sizeof why), why);
        const char *dir = "cache/selftest-spv";
        check("SPIR-V cache opens", spvcache_open(dir, "selftest"), dir);
        uint64_t k = spvcache_key(kGlsl, strlen(kGlsl), 0);
        check("cache store", spvcache_store(k, 0, spv, nw), "");
        size_t nw2 = 0;
        uint32_t *back = spvcache_load(k, &nw2);
        check("cache load returns the same module", back && nw2 == nw && !memcmp(back, spv, nw * 4), "");
        free(back);
        spvcache_close();
        free(spv);
    }
    shaderc_result_release(r);
    shaderc_compile_options_release(o);
    shaderc_compiler_release(c);
}

// The game code library is linked against eowrecomp.dll.a and calls back into the exe; a probe DLL built that way proves
// the export table, the import library and the loader agree (verify_package.sh builds it with the packaged zig).
static void t_probe(const char *dll) {
    HMODULE h = LoadLibraryA(dll);
    check("probe DLL (linked with the packaged import library) loads", h != NULL, dll);
    if (!h) return;
    uint64_t (*probe)(void) = (uint64_t (*)(void))GetProcAddress(h, "eow_probe");
    check("probe exports eow_probe", probe != NULL, "");
    if (probe) {
        uint64_t a = probe(), b;
        Sleep(20);
        b = probe();
        check("probe called rt_ticks() in the exe", a != 0 && b > a, "");
    }
    FreeLibrary(h);
}

int rt_selftest(int argc, char **argv) {
    const char *probe = NULL;
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--selftest-dll")) probe = argv[i + 1];
    g_log = fopen("selftest.log", "w");
    say("EoWRecomp selftest\n");
    t_resources();
    t_sdl();
    t_ffmpeg();
    t_vulkan();
    t_shaderc_and_cache();
    if (probe) t_probe(probe);
    say("%s (%d failed)\n", g_fail ? "SELFTEST FAILED" : "SELFTEST OK", g_fail);
    if (g_log) fclose(g_log);
    return g_fail ? 1 : 0;
}
#endif
