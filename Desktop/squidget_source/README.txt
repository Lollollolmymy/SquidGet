# Squidget

> Download high-quality music from squid.wtf — right from your terminal.

Squidget is a cross-platform terminal user interface (TUI) application that lets you search for and download Spotify tracks in various quality levels, from your command line. No browser, no ads — just fast, direct downloads.

---

## Features

- 🔍 Search Spotify tracks by query
- 💿 Download in multiple qualities: HI_RES_LOSSLESS, LOSSLESS, HIGH, LOW, DOLBY_ATMOS
- 🎨 Intuitive TUI with keyboard navigation
- 📁 Automatic output directory setup and file management
- ⚡ Background downloads with progress updates
- 🔄 Quality fallback — always succeeds even if high-res isn't available
- 🛡️ Filename sanitization and overwrite protection
- 🌐 Cross-platform: Windows, macOS, Linux

---

## Supported Platforms

- **Windows** (with included TCC compiler)
- **macOS** (with Xcode or GCC)
- **Linux** (with GCC)

---

## Requirements

- **Compiler**: GCC, Clang, or TCC (included for Windows)
- **Libraries**: libcurl (for HTTP requests on Unix; WinHTTP on Windows)
- **Internet**: For API access and downloads

---

## Installation

### Windows
1. Download or clone the repository
2. Run `squidget.bat` — it downloads a portable GCC, compiles, and launches

### macOS / Linux
1. Download or clone the repository
2. Run `./run.sh` — it auto-installs dependencies, compiles, and launches

### Manual Build
1. Ensure GCC/Clang and libcurl are installed
2. Run `make` to build
3. Execute `./squidget` (or `squidget.exe` on Windows)

---

## Setup

1. On first run, Squidget prompts you to choose a save location for downloads
2. Select from presets (e.g., ~/Music/squidget) or browse to a custom folder
3. Your choice is saved and reused for future sessions

---

## Usage

1. Launch Squidget
2. Type a search query (artist, song, album) and press Enter
3. Browse results with arrow keys
4. Press Enter on a track to select quality
5. Choose quality level and confirm download
6. Downloads happen in the background — watch progress in the status bar
7. Press Ctrl+C to quit

### Keyboard Shortcuts
- **↑↓**: Navigate lists
- **Enter**: Select
- **/**: Start search
- **Ctrl+C**: Quit

---

## Configuration

Settings are stored in:
- **Windows**: `%APPDATA%\squidget\config`
- **macOS**: `~/.config/squidget/config`
- **Linux**: `~/.config/squidget/config` or `$XDG_CONFIG_HOME/squidget/config`

Edit the `config` file manually if needed:
```
out_dir=/path/to/your/music/folder
```

---

## Troubleshooting

- **Compilation fails**: Ensure GCC/Clang and libcurl are installed
- **Downloads fail**: Check internet connection and API availability

---

## License

MIT — see source code for details.
