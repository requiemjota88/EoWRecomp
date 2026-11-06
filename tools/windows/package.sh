#!/bin/bash
# make dist, last step (MSYS2 UCRT64 shell on Windows): assembles build/dist/EoWRecomp/ and the ZIP from a native build.
#
#   EoWRecomp/
#     eowrecomp.exe           the runtime and start screen (no console window)
#     *.dll                   every DLL the exe (and those DLLs) load that Windows doesn't already have: found with
#                             ntldd, never listed by hand, so a dependency that changes can't be forgotten
#     eowrecomp.dll.a         the import library the game code is linked against on the player's machine
#     gamelib-src/            the headers and entry file that game code is compiled against (as in the macOS app)
#     eowgen/                 the code generator helper (PyInstaller); zig/ the compiler (slim zig, tools/zig_slim.sh)
#     fonts, licenses, README
#
# No game files, game code, keys or build tools go in (tools/windows/verify_package.sh checks it).
# EOW_VERSION: version in the file name (default: EOWRECOMP_VERSION in runtime/ui/launcher.mm).
set -euo pipefail
cd "$(dirname "$0")/../.."
VERSION=${EOW_VERSION:-$(sed -n 's/^#define EOWRECOMP_VERSION "\(.*\)"/\1/p' runtime/ui/launcher.mm)}
EXE=build/eowrecomp.exe
IMPLIB=build/eowrecomp.dll.a
GEN=build/helper/eowgen
ZIG=build/zig-slim
DEPS=build/deps-win
D=build/dist
OUT=$D/EoWRecomp
ZIP=$D/EoWRecomp-$VERSION-windows-x64.zip

for f in "$EXE" "$IMPLIB" "$GEN/eowgen.exe" "$ZIG/zig.exe" "$ZIG/rt/link.rsp" "$DEPS/bin"; do
    [ -e "$f" ] || { echo "package.sh: $f is missing (make dist builds it)"; exit 1; }
done

rm -rf "$OUT" "$D"/*.zip
mkdir -p "$OUT/gamelib-src" "$OUT/licenses"
cp "$EXE" "$IMPLIB" "$OUT/"
cp runtime/cpu.h runtime/simd.h runtime/simd_fp.h runtime/gamelib.h runtime/modules.h tools/gamelib_entry.c \
   runtime/hle/hle_symbols.txt "$OUT/gamelib-src/"
cp -R "$GEN" "$OUT/eowgen"
cp -R "$ZIG" "$OUT/zig"

# ---- DLLs: the exe's imports, recursively, minus what Windows itself provides
export PATH="$PWD/$DEPS/bin:$PATH"
ntldd -R "$EXE" | tr -d '\r' > "$D/ntldd.txt"
missing=$(grep -c 'not found' "$D/ntldd.txt" || true)
if [ "$missing" != 0 ]; then
    echo "package.sh: the exe has imports ntldd can't resolve:"; grep 'not found' "$D/ntldd.txt"; exit 1
fi
n=0
while IFS= read -r line; do
    case "$line" in *"=>"*) ;; *) continue ;; esac
    name=$(echo "${line%%=>*}" | tr -d '[:space:]')
    path=${line#*=> }; path=${path% (0x*}
    p=$(cygpath -u "$path")
    lower=$(echo "$p" | tr 'A-Z' 'a-z')
    case "$lower" in */windows/system32/*|*/windows/syswow64/*|*/windows/winsxs/*) continue ;; esac
    case "$(echo "$name" | tr 'A-Z' 'a-z')" in api-ms-win-*|ext-ms-win-*) continue ;; esac
    [ -f "$OUT/$name" ] || { cp "$p" "$OUT/$name"; n=$((n + 1)); }
done < "$D/ntldd.txt"
echo "package.sh: $n DLLs copied"

# ---- fonts, licenses, docs
cp third_party/fonts/Baloo2/Baloo2-SemiBold.ttf "$OUT/"
cp third_party/fonts/DejaVu/DejaVuSans.ttf "$OUT/"
cp third_party/fonts/Baloo2/OFL.txt "$OUT/licenses/Baloo2-OFL.txt"
cp third_party/fonts/DejaVu/LICENSE "$OUT/licenses/DejaVu-LICENSE.txt"
cp LICENSE "$OUT/licenses/EoWRecomp-LICENSE.txt"
cp tools/windows/THIRD-PARTY-NOTICES.txt "$OUT/licenses/"
cp tools/windows/LEIA-ME.txt "$OUT/LEIA-ME.txt"
cp tools/windows/Iniciar-EoWRecomp.bat "$OUT/"
cp README.md "$OUT/README.md"
for l in "$DEPS"/src/ffmpeg/COPYING.LGPLv2.1; do [ -f "$l" ] && cp "$l" "$OUT/licenses/FFmpeg-COPYING.LGPLv2.1.txt"; done
for pkg in sdl3 shaderc glslang spirv-tools vulkan-loader; do
    for l in /ucrt64/share/licenses/$pkg/*; do [ -f "$l" ] && cp "$l" "$OUT/licenses/$pkg-$(basename "$l").txt"; done
done

# ---- manifest + zip
(cd "$OUT" && find . -type f | sort | sed 's|^\./||' > MANIFEST.txt)
(cd "$D" && zip -q -r -9 "$(basename "$ZIP")" EoWRecomp)
echo "package.sh: $ZIP ($(du -h "$ZIP" | cut -f1), $(wc -l < "$OUT/MANIFEST.txt") files)"
( cd "$D" && sha256sum "$(basename "$ZIP")" | tee "$(basename "$ZIP").sha256" )
