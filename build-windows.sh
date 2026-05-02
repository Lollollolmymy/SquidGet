#!/bin/bash
# Cross-compile SquidGet for Windows using MinGW

set -e

echo "[squidget] Cross-compiling for Windows..."

# Check for MinGW
if ! command -v x86_64-w64-mingw32-gcc &> /dev/null; then
    echo "Installing MinGW..."
    if command -v brew &> /dev/null; then
        brew install mingw-w64
    else
        echo "Error: Please install MinGW-w64"
        echo "On macOS: brew install mingw-w64"
        echo "On Ubuntu/Debian: sudo apt install mingw-w64"
        exit 1
    fi
fi

# Compile for Windows
echo "Compiling for Windows x64..."
x86_64-w64-mingw32-gcc -std=c11 -Wall -Wextra -O2 -g \
    main.c api.c download.c tui.c json.c config.c platform.c tag.c \
    -lwinhttp -lws2_32 -lcrypt32 -o squidget.exe

echo "[squidget] Windows build complete: squidget.exe"

# Create Windows batch file for easier use
cat > squidget.bat << 'EOF'
@echo off
cd /d "%~dp0"
squidget.exe %*
EOF

echo "Created squidget.bat for Windows users"
