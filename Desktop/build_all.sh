#!/bin/sh
# build_all.sh — compile squidget for every platform
# Requires: zig (installed via pip or ziglang.org)
#
# Usage:
#   ./build_all.sh           → build everything into ./dist/
#   ./build_all.sh --windows → also cross-compile Windows (needs mingw-w64)

set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
DIST="$DIR/dist"
CURL_STUB="$DIR/.curl_stub"
MACOS_STUB="$DIR/.macos_stub"

# ── colours ──────────────────────────────────────────────────────────
GRN='\033[0;32m' RED='\033[0;31m' YLW='\033[0;33m' RST='\033[0m'
ok()   { printf "${GRN}  ✓${RST} %s\n" "$1"; }
fail() { printf "${RED}  ✗${RST} %s\n" "$1"; }
info() { printf "${YLW}  →${RST} %s\n" "$1"; }

SRCS="$DIR/main.c $DIR/api.c $DIR/download.c $DIR/tui.c $DIR/json.c"
CFLAGS="-std=c11 -O2"

# ── check for zig ────────────────────────────────────────────────────
find_zig() {
    if command -v zig >/dev/null 2>&1; then
        echo "zig"
    elif python3 -m ziglang version >/dev/null 2>&1; then
        echo "python3 -m ziglang"
    else
        echo ""
    fi
}

install_zig() {
    info "zig not found — installing via pip..."
    if python3 -m pip install ziglang --break-system-packages >/dev/null 2>&1 ||
       python3 -m pip install ziglang >/dev/null 2>&1; then
        ok "zig installed via pip"
    else
        printf "${RED}ERROR:${RST} Could not install zig.\n"
        printf "Install manually: pip install ziglang\n"
        printf "Or download from: https://ziglang.org/download/\n"
        exit 1
    fi
}

# ── generate stubs ───────────────────────────────────────────────────
make_curl_stub() {
    mkdir -p "$CURL_STUB/curl"
    # Minimal curl.h — just the types and functions squidget uses
    cat > "$CURL_STUB/curl/curl.h" << 'CURLH'
#pragma once
#include <stddef.h>
typedef void CURL;
typedef int CURLcode;
typedef int CURLoption;
typedef int CURLINFO;
#define CURLE_OK 0
#define CURL_GLOBAL_DEFAULT 3
#define CURLOPT_URL           10002
#define CURLOPT_WRITEFUNCTION 20011
#define CURLOPT_WRITEDATA     10001
#define CURLOPT_FOLLOWLOCATION 52
#define CURLOPT_TIMEOUT        13
#define CURLOPT_USERAGENT     10018
#define CURLINFO_RESPONSE_CODE 0x200002
CURL *curl_easy_init(void);
void curl_easy_cleanup(CURL *);
CURLcode curl_easy_setopt(CURL *, CURLoption, ...);
CURLcode curl_easy_perform(CURL *);
CURLcode curl_easy_getinfo(CURL *, CURLINFO, ...);
char *curl_easy_escape(CURL *, const char *, int);
void curl_free(void *);
CURLcode curl_global_init(long);
void curl_global_cleanup(void);
CURLH

    # Weak symbol stub — lets binaries link without a real libcurl at compile time.
    # The real libcurl.so/dylib on the target system takes over at runtime.
    cat > "$CURL_STUB/curl_stub.c" << 'CSTUB'
#include <stddef.h>
__attribute__((weak)) void *curl_easy_init(void){return NULL;}
__attribute__((weak)) void  curl_easy_cleanup(void *c){(void)c;}
__attribute__((weak)) int   curl_easy_setopt(void *c,...){(void)c;return 0;}
__attribute__((weak)) int   curl_easy_perform(void *c){(void)c;return 1;}
__attribute__((weak)) int   curl_easy_getinfo(void *c,...){(void)c;return 0;}
__attribute__((weak)) char *curl_easy_escape(void *c,const char *s,int l){(void)c;(void)s;(void)l;return NULL;}
__attribute__((weak)) void  curl_free(void *p){(void)p;}
__attribute__((weak)) int   curl_global_init(long f){(void)f;return 0;}
__attribute__((weak)) void  curl_global_cleanup(void){}
CSTUB

    mkdir -p "$MACOS_STUB"
    cat > "$MACOS_STUB/libcurl.tbd" << 'TBD'
--- !tapi-tbd
tbd-version:     4
targets:         [ x86_64-macos, arm64-macos ]
install-name:    '/usr/lib/libcurl.dylib'
exports:
  - targets:     [ x86_64-macos, arm64-macos ]
    symbols:     [ _curl_easy_init, _curl_easy_cleanup, _curl_easy_setopt,
                   _curl_easy_perform, _curl_easy_getinfo, _curl_easy_escape,
                   _curl_free, _curl_global_init, _curl_global_cleanup ]
...
TBD
}

# ── build one target ─────────────────────────────────────────────────
# Usage: build_target ZIG "target-triple" outfile [extra_flags]
build_target() {
    ZIG="$1" TARGET="$2" OUT="$3"
    shift 3
    EXTRA="$*"

    mkdir -p "$(dirname "$OUT")"

    if $ZIG cc $CFLAGS \
        $SRCS \
        "$CURL_STUB/curl_stub.c" \
        -I"$DIR" -I"$CURL_STUB" \
        -lm -lpthread \
        $EXTRA \
        -target "$TARGET" \
        -o "$OUT" 2>/tmp/zig_err; then
        ok "$TARGET → $(basename "$OUT")"
    else
        fail "$TARGET"
        sed 's/^/     /' /tmp/zig_err | head -5
    fi
}

# ── main ─────────────────────────────────────────────────────────────
printf "\n${YLW}squidget — universal build${RST}\n\n"

# Find or install zig
ZIG="$(find_zig)"
if [ -z "$ZIG" ]; then
    install_zig
    ZIG="$(find_zig)"
fi
info "using zig: $($ZIG version)"

# Clean dist
rm -rf "$DIST"
mkdir -p "$DIST"

# Generate stubs
make_curl_stub

printf "\n${YLW}── macOS ─────────────────────────────────────────${RST}\n"
build_target "$ZIG" "aarch64-macos" \
    "$DIST/macos/squidget_arm64" \
    "-L$MACOS_STUB -lcurl"

build_target "$ZIG" "x86_64-macos" \
    "$DIST/macos/squidget_x86_64" \
    "-L$MACOS_STUB -lcurl"

printf "\n${YLW}── Linux glibc (Debian/Ubuntu/Fedora/Arch/etc) ──${RST}\n"
# glibc 2.17 = CentOS 7 era (2013) — runs on virtually every glibc distro
build_target "$ZIG" "x86_64-linux-gnu.2.17"    "$DIST/linux/glibc_x86_64/squidget"
build_target "$ZIG" "aarch64-linux-gnu.2.17"   "$DIST/linux/glibc_arm64/squidget"
build_target "$ZIG" "arm-linux-gnueabihf.2.17" "$DIST/linux/glibc_armv7/squidget"

printf "\n${YLW}── Linux musl (Alpine/Void-musl/etc) ────────────${RST}\n"
build_target "$ZIG" "x86_64-linux-musl"  "$DIST/linux/musl_x86_64/squidget"
build_target "$ZIG" "aarch64-linux-musl" "$DIST/linux/musl_arm64/squidget"

printf "\n${YLW}── Windows ───────────────────────────────────────${RST}\n"
if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    mkdir -p "$DIST/windows"
    if x86_64-w64-mingw32-gcc -std=c11 -O2 \
        $SRCS -I"$DIR" -lwinhttp -lm \
        -o "$DIST/windows/squidget.exe" 2>/tmp/mingw_err; then
        ok "windows x86_64 → squidget.exe"
    else
        fail "windows (mingw error)"
        head -3 /tmp/mingw_err | sed 's/^/     /'
    fi
elif $ZIG cc $CFLAGS $SRCS -I"$DIR" \
    -target x86_64-windows \
    -lwinhttp \
    -o "$DIST/windows/squidget.exe" 2>/tmp/zig_win_err; then
    ok "windows x86_64 → squidget.exe (via zig)"
else
    info "skipping Windows — install mingw-w64 or run on Linux"
    info "  Linux:  sudo apt install mingw-w64"
    info "  macOS:  brew install mingw-w64"
fi

# ── copy the .bat into windows dist if present ────────────────────────
if [ -f "$DIR/squidget.bat" ] && [ -d "$DIST/windows" ]; then
    cp "$DIR/squidget.bat" "$DIST/windows/"
fi

# ── write distro guide ────────────────────────────────────────────────
cat > "$DIST/WHICH_FILE.txt" << 'EOF'
squidget — which binary to use?
================================

macOS
  M1/M2/M3/M4 (Apple Silicon) → macos/squidget_arm64
  Intel Mac                    → macos/squidget_x86_64

Linux — glibc (most distros)
  Covers: Debian, Ubuntu, Mint, Kali, Parrot, Pop!_OS, Zorin, elementary,
          Fedora, RHEL, Rocky, AlmaLinux, CentOS, openSUSE, Arch, Manjaro,
          EndeavourOS, CachyOS, Garuda, Void (glibc), Raspberry Pi OS, and ~40 more
  64-bit PC    → linux/glibc_x86_64/squidget
  64-bit ARM   → linux/glibc_arm64/squidget   (Raspberry Pi 4+, AWS Graviton)
  32-bit ARM   → linux/glibc_armv7/squidget   (older Raspberry Pi)

Linux — musl (Alpine and musl-based distros)
  64-bit PC    → linux/musl_x86_64/squidget   (statically linked, no deps)
  64-bit ARM   → linux/musl_arm64/squidget

Windows
  → windows/squidget.exe
  If blocked by antivirus/guest account: use windows/squidget.bat instead

Not sure?
  Run: uname -m
  x86_64  → use the x86_64 binary
  aarch64 → use the arm64 binary
  armv7l  → use the armv7 binary

Not listed?
  Use the source folder and run: ./run.sh
EOF

# ── cleanup stubs ─────────────────────────────────────────────────────
rm -rf "$CURL_STUB" "$MACOS_STUB" /tmp/zig_err /tmp/zig_win_err /tmp/mingw_err 2>/dev/null

printf "\n${GRN}Done!${RST} Binaries in: $DIST/\n\n"
find "$DIST" -type f ! -name "*.txt" | sort | while read f; do
    SIZE=$(du -sh "$f" 2>/dev/null | cut -f1)
    printf "  %-8s %s\n" "$SIZE" "${f#$DIST/}"
done
printf "\n"
