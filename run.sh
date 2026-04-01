#!/bin/sh
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
EXE="$DIR/squidget"

# already compiled — just run
if [ -f "$EXE" ]; then
    # cd into the script's own directory so the CWD is always predictable
    # regardless of whether we were launched from a terminal, Finder, or
    # any other launch method.  resolve_out_dir() in main.c ignores CWD
    # and always writes to ~/Music/squidget/, so this is just defensive.
    cd "$DIR"
    exec "$EXE"
fi

echo "[squidget] first run: compiling..."

# detect OS and install deps if missing
OS="$(uname -s)"

if [ "$OS" = "Darwin" ]; then
    # macOS — libcurl is built in, just need Xcode CLT
    if ! command -v cc >/dev/null 2>&1; then
        echo "[squidget] installing Xcode command line tools..."
        xcode-select --install
        echo "[squidget] re-run this script after the install finishes."
        exit 1
    fi
    cc -std=c11 -O2 \
        "$DIR/main.c" "$DIR/api.c" "$DIR/download.c" \
        "$DIR/tui.c"  "$DIR/json.c" "$DIR/config.c" "$DIR/platform.c" \
        -I"$DIR" -lcurl -lm -lpthread \
        -o "$EXE"

    # Strip the quarantine extended attribute so macOS doesn't try to
    # apply App Translocation on subsequent Finder launches.
    xattr -d com.apple.quarantine "$EXE" 2>/dev/null || true

elif [ "$OS" = "Linux" ]; then
    if ! command -v gcc >/dev/null 2>&1; then
        echo "[squidget] installing gcc..."
        if command -v apt-get >/dev/null 2>&1; then
            sudo apt-get install -y gcc libcurl4-openssl-dev
        elif command -v pacman >/dev/null 2>&1; then
            sudo pacman -S --noconfirm gcc curl
        elif command -v dnf >/dev/null 2>&1; then
            sudo dnf install -y gcc libcurl-devel
        else
            echo "[squidget] please install gcc and libcurl manually then re-run."
            exit 1
        fi
    fi
    gcc -std=c11 -O2 \
        "$DIR/main.c" "$DIR/api.c" "$DIR/download.c" \
        "$DIR/tui.c"  "$DIR/json.c" "$DIR/config.c" "$DIR/platform.c" \
        -I"$DIR" -lcurl -lm -lpthread \
        -o "$EXE"

else
    echo "[squidget] unsupported OS: $OS"
    exit 1
fi

echo "[squidget] done. launching..."
cd "$DIR"
exec "$EXE"
