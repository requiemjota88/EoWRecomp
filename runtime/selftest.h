// --selftest (runtime/selftest.c): checks a packaged build without game files or a GPU. Exit code 0 = everything passed.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
int rt_selftest(int argc, char **argv);
#ifdef __cplusplus
}
#endif
