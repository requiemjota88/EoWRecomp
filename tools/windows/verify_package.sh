#!/bin/bash
# Checks the unpacked release ZIP the way a player's machine would use it (MSYS2 shell on Windows, after `make dist`).
#   1. every file the app looks for is there, and nothing that must never ship is (game files, keys, build products)
#   2. the exe starts and passes its own --selftest with a PATH that has no MSYS2 / build tool in it: a DLL that is
#      missing from the package fails right here, not on the player's machine
#   3. the packaged compiler really builds a DLL against the packaged headers and import library, and the DLL loads and
#      calls back into the exe (the chain the game code library uses on the player's machine)
#   4. the ZIP itself unpacks to the same file list as MANIFEST.txt
# usage: tools/windows/verify_package.sh [zip]
set -uo pipefail
cd "$(dirname "$0")/../.."
ZIP=${1:-$(ls build/dist/EoWRecomp-*-windows-x64.zip | head -1)}
W=build/verify; rm -rf "$W"; mkdir -p "$W"
fail=0
bad() { echo "::error::verify_package: $*"; fail=$((fail + 1)); }

unzip -q "$ZIP" -d "$W" || { bad "can't unzip $ZIP"; exit 1; }
P=$W/EoWRecomp
echo "verify_package: $ZIP"

# ---- 1. contents
for f in eowrecomp.exe eowrecomp.dll.a eowgen/eowgen.exe zig/zig.exe zig/rt/link.rsp gamelib-src/cpu.h gamelib-src/simd.h \
         gamelib-src/simd_fp.h gamelib-src/gamelib.h gamelib-src/modules.h gamelib-src/gamelib_entry.c gamelib-src/hle_symbols.txt \
         Baloo2-SemiBold.ttf DejaVuSans.ttf LEIA-ME.txt Iniciar-EoWRecomp.bat README.md MANIFEST.txt licenses/THIRD-PARTY-NOTICES.txt; do
    [ -f "$P/$f" ] || bad "missing from the package: $f"
done
for dll in SDL3.dll vulkan-1.dll avcodec avformat avutil shaderc_shared; do
    ls "$P" | grep -qi "${dll%.dll}" || bad "no DLL matching $dll in the package"
done
# never in a package: game files / keys / generated game code / build products and tools
forbidden='\.(nsp|xci|nca|nso|nro|npdm|bntx|bfres|bfsha|bnsh|szs|romfs|keys|o|obj|pdb|ilk|c|cpp)$|prod\.keys|title\.keys|(^|/)(extracted|generated|cache|save)/|eowgame|CMakeCache|build\.ninja'
(cd "$P" && find . -type f | sed 's|^\./||') > "$W/files.txt"
# (gamelib_entry.c is the one source file that ships: it is compiled against the player's own generated code)
grep -Ei "$forbidden" "$W/files.txt" | grep -v '^gamelib-src/gamelib_entry\.c$' | grep -Ev '^zig/(lib|rt)/' > "$W/forbidden.txt"
[ -s "$W/forbidden.txt" ] && { bad "forbidden files in the package:"; cat "$W/forbidden.txt"; }
big=$(find "$P" -type f -size +400M | head -3); [ -n "$big" ] && bad "suspiciously large files: $big"
sort "$W/files.txt" | grep -v '^MANIFEST.txt$' > "$W/a.txt"; grep -v '^MANIFEST.txt$' "$P/MANIFEST.txt" | sort > "$W/b.txt"
diff -q "$W/a.txt" "$W/b.txt" > /dev/null || bad "MANIFEST.txt doesn't match the ZIP"
echo "verify_package: $(wc -l < "$W/files.txt") files, contents checked"

# ---- 2. the exe, with a PATH that has only Windows in it
WINPATH="$(cygpath -u "$SYSTEMROOT")/System32:$(cygpath -u "$SYSTEMROOT")"
run_clean() { (cd "$P" && env -i SystemRoot="$SYSTEMROOT" SYSTEMROOT="$SYSTEMROOT" TEMP="$TEMP" PATH="$WINPATH" "$@"); }
run_clean ./eowrecomp.exe --selftest > "$W/selftest.out" 2>&1; rc=$?
cat "$W/selftest.out"; [ -f "$P/selftest.log" ] && echo "(selftest.log written: $(wc -l < "$P/selftest.log") lines)"
[ $rc = 0 ] || bad "eowrecomp.exe --selftest exited $rc (0xC0000135 = a DLL is missing from the package)"
grep -q 'SELFTEST OK' "$P/selftest.log" 2>/dev/null || bad "selftest.log lacks SELFTEST OK"

# eowgen starts (the PyInstaller bundle unpacks and runs)
run_clean ./eowgen/eowgen.exe --help > "$W/eowgen.out" 2>&1; rc=$?
[ $rc = 0 ] || [ $rc = 2 ] || { bad "eowgen.exe didn't start (exit $rc)"; head -20 "$W/eowgen.out"; }

# ---- 3. the packaged compiler builds a DLL against the packaged headers + import library
cat > "$W/probe.c" <<'EOP'
#include "cpu.h"
__declspec(dllexport) unsigned long long eow_probe(void) { return rt_ticks(); }
EOP
FLAGS=$(make -s print-gamelib-flags 2>/dev/null)
Z=$(cygpath -w "$PWD/$P/zig")
mkdir -p "$W/probe"
# shellcheck disable=SC2086
(cd "$W/probe" && "$PWD/../../../$P/zig/zig.exe" cc $FLAGS -I"$PWD/../../../$P/gamelib-src" -c ../probe.c -o probe.o) \
    || bad "the packaged zig can't compile against gamelib-src"
if [ -f "$W/probe/probe.o" ]; then
    (cd "$W/probe" && "$PWD/../../../$P/zig/zig.exe" lld-link "@$(cygpath -w "$PWD/../../../$P/zig/rt/link.rsp")" \
        "-LIBPATH:$(cygpath -w "$PWD/../../../$P/zig/rt")" -OUT:probe.dll probe.o "$(cygpath -w "$PWD/../../../$P/eowrecomp.dll.a")") \
        || bad "the packaged zig can't link against eowrecomp.dll.a"
fi
if [ -f "$W/probe/probe.dll" ]; then
    run_clean ./eowrecomp.exe --selftest --selftest-dll "$(cygpath -w "$PWD/$W/probe/probe.dll")" > "$W/selftest2.out" 2>&1 \
        || { bad "probe DLL selftest failed"; tail -15 "$W/selftest2.out"; }
    grep -q 'called rt_ticks' "$W/selftest2.out" && grep -q '^\[ok\] probe called' "$W/selftest2.out" || bad "probe didn't reach the exe"
else
    bad "no probe.dll was produced"
fi
echo "verify_package: $([ $fail = 0 ] && echo PASSED || echo "FAILED ($fail)")"
[ $fail = 0 ]
