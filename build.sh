#!/bin/bash
# Universal SquidGet Builder - auto-detects OS and builds accordingly

set -e

# Detect OS
OS=$(uname -s)

case "$OS" in
    MINGW*|MSYS*|CYGWIN*)
        # Windows (Git Bash, WSL, or Cygwin)
        if [ -f "squidget.bat" ]; then
            cmd /c squidget.bat
        else
            echo "Error: squidget.bat not found"
            exit 1
        fi
        ;;
    Darwin)
        # macOS
        echo "[squidget] Building on macOS..."
        
        # Check for Xcode Command Line Tools
        if ! xcode-select -p &> /dev/null; then
            echo "Installing Xcode Command Line Tools..."
            xcode-select --install
            echo "Please run this script again after installation completes."
            exit 0
        fi
        
        # Check for curl
        if ! command -v curl &> /dev/null; then
            echo "Error: curl is required"
            exit 1
        fi

        # Compile with libcurl if available
        CURL_FLAGS=""
        if command -v pkg-config &> /dev/null && pkg-config --exists libcurl; then
            CURL_FLAGS=$(pkg-config --cflags --libs libcurl)
            echo "[squidget] libcurl found via pkg-config, enabling..."
        elif [ -d "/opt/homebrew/opt/curl" ]; then
            CURL_FLAGS="-I/opt/homebrew/opt/curl/include -L/opt/homebrew/opt/curl/lib -lcurl"
            echo "[squidget] libcurl found via Homebrew, enabling..."
        fi
        
        # Compile
        gcc -std=c11 -Wall -Wextra -O2 \
            main.c api.c download.c tui.c json.c config.c platform.c tag.c \
            -lpthread $CURL_FLAGS -DSQT_USE_CURL -o squidget
        
        echo "[squidget] done."
        ./squidget
        ;;
    Linux*)
        # Linux
        echo "[squidget] Building on Linux..."
        
        # Check for GCC
        if ! command -v gcc &> /dev/null; then
            echo "Installing GCC..."
            if command -v apt &> /dev/null; then
                sudo apt update && sudo apt install -y gcc
            elif command -v pacman &> /dev/null; then
                sudo pacman -S gcc
            elif command -v dnf &> /dev/null; then
                sudo dnf install -y gcc
            else
                echo "Error: Could not install GCC. Please install it manually."
                exit 1
            fi
        fi
        
        # Check for curl
        if ! command -v curl &> /dev/null; then
            echo "Installing curl..."
            if command -v apt &> /dev/null; then
                sudo apt update && sudo apt install -y curl libcurl4-openssl-dev pkg-config
            elif command -v pacman &> /dev/null; then
                sudo pacman -S curl curl pkg-config
            elif command -v dnf &> /dev/null; then
                sudo dnf install -y curl libcurl-devel pkg-config
            else
                echo "Error: Could not install curl. Please install it manually."
                exit 1
            fi
        fi

        # Compile with libcurl if available
        CURL_FLAGS=""
        if command -v pkg-config &> /dev/null && pkg-config --exists libcurl; then
            CURL_FLAGS=$(pkg-config --cflags --libs libcurl)
            echo "[squidget] libcurl found via pkg-config, enabling..."
        elif [ -d "/opt/homebrew/opt/curl" ]; then
            CURL_FLAGS="-I/opt/homebrew/opt/curl/include -L/opt/homebrew/opt/curl/lib -lcurl"
            echo "[squidget] libcurl found via Homebrew, enabling..."
        fi
        
        # Compile
        gcc -std=c11 -Wall -Wextra -O2 \
            main.c api.c download.c tui.c json.c config.c platform.c tag.c \
            -lpthread $CURL_FLAGS -DSQT_USE_CURL -o squidget
        
        echo "[squidget] done."
        ./squidget
        ;;
    *)
        echo "Error: Unsupported OS: $OS"
        exit 1
        ;;
esac
