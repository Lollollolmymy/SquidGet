# Squidget

A fast, minimal terminal music downloader. Search and download Spotify tracks and albums in lossless or high-res quality — no browser, straight from your terminal.

---

## What it does

Squidget is a terminal UI written in C that lets you search Spotify, pick what you want, and download it. Browse with your keyboard, select a quality level, and the download runs in the background while you keep working. Downloads can be organized by album automatically.

Uses Spotisaver to resolve Spotify metadata and fetch download URLs, handles quality fallback automatically, and saves everything to your configured folder.

---

## Features

- Search Spotify tracks by title, artist, or album
- Download full albums
- Multiple quality levels: HI_RES_LOSSLESS, LOSSLESS, HIGH, LOW, DOLBY_ATMOS
- Automatic quality fallback — always works even if hi-res isn't available
- Background downloads with live progress
- First-run setup wizard 
- Embedstrack metadata (title, artist, album tags)
- Cross-platform: Windows, macOS, Linux

---

## Platform Support

| Platform | Status |
|----------|--------|
| Windows  | Fully supported — TCC compiler included, no install needed |
| macOS    | Fully supported — auto-installs dependencies via build.sh |
| Linux    | Supported — requires GCC and curl CLI |

---

## Requirements

- **Windows**: Nothing. The TCC compiler is bundled in the repo.
- **macOS**: Xcode Command Line Tools (`xcode-select --install`). curl is pre-installed.
- **Linux**: GCC and curl. The run script will install them if missing.

No libcurl dev headers required. Squidget uses the curl CLI binary via popen on Unix systems, so there's no `-lcurl` link dependency.

---

## Installation

### Windows

**Option 1 — Precompiled binary:**  
Download the latest release and run `main.exe` directly.

**Option 2 — Build from source:**  
Run `squidget.bat`. It uses the bundled TCC compiler to build and launch automatically.

### macOS

```sh
git clone https://github.com/lollollolmymy/Squidget.git
cd Squidget
chmod +x build.sh
./build.sh
```

On first run it compiles the binary and removes the quarantine flag so macOS doesn't block it. Subsequent runs launch directly.

### Linux

```sh
git clone https://github.com/lollollolmymy/Squidget.git
cd Squidget
chmod +x build.sh
./build.sh
```

The script checks for GCC and curl, installs them if missing (apt, pacman, and dnf supported), then compiles and launches.

---

## First Run

When you launch Squidget for the first time, it walks you through choosing a save location for downloads. You can pick from preset paths like `~/Music/squidget` or enter a custom directory. The choice is saved and reused across sessions.

---

## Usage

1. Launch Squidget
2. **Tab key** to switch between Track Search and Album Search
3. Type a query and press Enter to search
4. Use arrow keys to browse results
5. Press Enter to select:
   - **Tracks**: Opens quality picker — choose quality and confirm to download
   - **Albums**: Opens action menu — download whole album or browse tracks first
6. Downloads run in the background — watch the progress bar
7. Press Ctrl+C to quit

### Album Downloads

Albums automatically create a subfolder with the album name inside your download directory. All tracks get organized there.

### Keyboard Shortcuts

| Key | Action |
|-----|--------|
| Arrow keys | Navigate lists |
| Enter | Select / confirm |
| Tab | Switch to albums ↔ tracks |
| Backspace | Edit search query |
| Ctrl+U | Clear search |
| Ctrl+C | Quit |

---

## Configuration

Settings are stored in a plain text config file:

- **Windows**: `%APPDATA%\squidget\config`
- **macOS / Linux**: `~/.config/squidget/config`

You can edit it manually:

```
out_dir=/path/to/your/music/folder
```Spotisaver API endpoint may be temporarily unavailable.

**Compilation fails on Linux**  
Make sure GCC and curl are installed: `sudo apt install gcc curl` (or pacman/dnf equivalent)

**macOS says the binary is damaged**  
Run `xattr -d com.apple.quarantine ./squidget` or just use `build.sh` which handles this automatically.

**Album downloads aren't working**  
Make sure your save directory exists and is writable. Squidget creates the album subfolder automatically.

---
**Compilation fails on Linux**  
Make sure GCC and curl are installed: `sudo apt install gcc curl`

**macOS says the binary is damaged or can't be opened**  
Run `xattr -d com.apple.quarantine ./squidget` to remove the quarantine flag, or just use `build.sh` which handles this automatically.

---

## License

MIT. See `LICENSE.txt` for details.
