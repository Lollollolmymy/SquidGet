<a name="readme-top"></a>

<!-- SHIELDS -->
[![License][license-shield]][license-url]
[![Stars][stars-shield]][stars-url]
[![Forks][forks-shield]][forks-url]
[![Issues][issues-shield]][issues-url]

<!-- HEADER -->
<br />
<div align="center">
  <h1>🦑 SquidGet</h1>
  <p><strong>A blazing-fast, cross-platform TUI music downloader written in pure C.</strong><br/>
  No Electron. No Python. No runtime dependencies. Just a single native binary.</p>

  <p>
    <a href="https://github.com/Lollollolmymy/SquidGet/issues/new?template=bug_report.md">Report a Bug</a>
    ·
    <a href="https://github.com/Lollollolmymy/SquidGet/issues/new?template=feature_request.md">Request a Feature</a>
    ·
    <a href="https://github.com/Lollollolmymy/SquidGet/wiki">Documentation</a>
  </p>
</div>

---

<!-- TABLE OF CONTENTS -->
<details>
  <summary>Table of Contents</summary>
  <ol>
    <li><a href="#about">About The Project</a></li>
    <li><a href="#features">Features</a></li>
    <li><a href="#getting-started">Getting Started</a>
      <ul>
        <li><a href="#prerequisites">Prerequisites</a></li>
        <li><a href="#installation">Installation</a></li>
      </ul>
    </li>
    <li><a href="#usage">Usage</a></li>
    <li><a href="#keybindings">Keybindings</a></li>
    <li><a href="#quality-levels">Quality Levels</a></li>
    <li><a href="#project-structure">Project Structure</a></li>
    <li><a href="#contributing">Contributing</a></li>
    <li><a href="#license">License</a></li>
  </ol>
</details>

---

<!-- ABOUT -->
## <a name="about"></a> About The Project

SquidGet is a lightweight, terminal-based music downloader built entirely in **C11** with zero external library dependencies. It features a fully interactive TUI (Terminal User Interface), parallel album downloads, automatic audio tagging (FLAC/Vorbis), and cover art embedding — all packed into a single compiled binary.

It runs natively on **macOS**, **Linux**, and **Windows** using only the tools you already have.

### Built With

![C](https://img.shields.io/badge/C11-00599C?style=for-the-badge&logo=c&logoColor=white)
![Shell](https://img.shields.io/badge/Bash-4EAA25?style=for-the-badge&logo=gnubash&logoColor=white)
![Windows](https://img.shields.io/badge/Windows-WinHTTP-0078D6?style=for-the-badge&logo=windows&logoColor=white)
![macOS](https://img.shields.io/badge/macOS-curl-000000?style=for-the-badge&logo=apple&logoColor=white)
![Linux](https://img.shields.io/badge/Linux-curl-FCC624?style=for-the-badge&logo=linux&logoColor=black)

<p align="right">(<a href="#readme-top">back to top</a>)</p>

---

<!-- FEATURES -->
## <a name="features"></a> Features

| Feature | Description |
|---|---|
| **Interactive TUI** | Fully keyboard-driven terminal interface — no mouse required |
| **Track & Album Search** | Search songs or full albums and browse results instantly |
| **Parallel Downloads** | Album downloads use a 4-thread worker pool for maximum speed |
| **Multi-Quality Support** | Choose from Hi-Res Lossless, Lossless, High, Low, and Dolby Atmos |
| **Auto-Tagging** | Embeds title, artist, album, year, ISRC, track number, and replay gain |
| **Cover Art** | Fetches and embeds album artwork into every downloaded file |
| **Cross-Platform** | Single codebase compiles and runs on macOS, Linux, and Windows |
| **Smart Save Paths** | First-run wizard with OS-appropriate preset directories |
| **Crash Safe** | Structured exception handler on Windows + signal cleanup on POSIX |
| **Zero Dependencies** | No external libraries — uses `curl` (POSIX) or `WinHTTP` (Windows) |

<p align="right">(<a href="#readme-top">back to top</a>)</p>

---

<!-- GETTING STARTED -->
## <a name="getting-started"></a> Getting Started

### <a name="prerequisites"></a> Prerequisites

| Platform | Requirement |
|---|---|
| **macOS (Homebrew)** | `brew` — [install Homebrew](https://brew.sh) if you don't have it |
| **macOS (source)** | Xcode Command Line Tools (`xcode-select --install`) + `curl` (pre-installed) |
| **Linux** | `gcc` + `curl` (install via your package manager) |
| **Windows** | MinGW-w64 or MSVC — the `.bat` build script handles the rest |

> [!NOTE]
> No additional C libraries are needed. SquidGet uses only the C standard library and platform HTTP APIs.

### <a name="installation"></a> Installation

#### � macOS — Homebrew

```bash
brew tap Lollollolmymy/squidget
brew install squidget
```

#### 🪟 Windows — Chocolatey

```cmd
choco install squidget
```

#### 🐧 Linux — Build from Source

```bash
git clone https://github.com/Lollollolmymy/SquidGet.git
cd SquidGet
./build.sh
```

<p align="right">(<a href="#readme-top">back to top</a>)</p>

---

<!-- USAGE -->
## <a name="usage"></a> Usage

```
┌─────────────────────────────────────────┐
│  squidget                               │
│─────────────────────────────────────────│
│  Search: kendrick lamar▌                │
│─────────────────────────────────────────│
│  >  HUMBLE.                  3:02       │
│     DNA.                     3:05       │
│     ELEMENT.                 3:36       │
│     LOYALTY. (feat. Rihanna) 3:47       │
│─────────────────────────────────────────│
│  [↑↓] navigate  [↵] select  [TAB] albums│
│  [^U] clear     [^C] quit               │
└─────────────────────────────────────────┘
```

1. **Type** to search — results appear after pressing `Enter`
2. **Navigate** the list with `↑` / `↓`
3. **Press `Enter`** on a track to open the quality picker
4. **Select a quality** and the download starts in the background
5. **Press `TAB`** to switch between track search and album search

<p align="right">(<a href="#readme-top">back to top</a>)</p>

---

<!-- KEYBINDINGS -->
## <a name="keybindings"></a> Keybindings

| Key | Action |
|---|---|
| `↑` / `↓` | Move cursor up / down |
| `Enter` | Confirm selection / run search |
| `Tab` | Toggle between Song search and Album search |
| `PgUp` / `PgDn` | Scroll results by one page |
| `Home` / `End` | Jump to first / last result |
| `Backspace` | Delete last character from search query |
| `Ctrl+U` | Clear entire search query |
| `Esc` | Go back / cancel current menu |
| `Ctrl+C` | Quit SquidGet |

<p align="right">(<a href="#readme-top">back to top</a>)</p>

---

<!-- QUALITY LEVELS -->
## <a name="quality-levels"></a> Quality Levels

| # | Label | Format | Description |
|---|---|---|---|
| 1 | `HI_RES_LOSSLESS` | FLAC | 24-bit / 96kHz+ Master quality |
| 2 | `LOSSLESS` | FLAC | 16-bit / 44.1kHz CD quality |
| 3 | `HIGH` | AAC | 320 kbps |
| 4 | `LOW` | AAC | 96 kbps |
| 5 | `DOLBY_ATMOS` | AC-4 / MHA1 | Spatial / Dolby Atmos |

> [!TIP]
> The quality picker appears when you select a track. Use number keys `1`–`5` as shortcuts to pick instantly.

<p align="right">(<a href="#readme-top">back to top</a>)</p>

---

<!-- PROJECT STRUCTURE -->
## <a name="project-structure"></a> Project Structure

```
SquidGet/
├── main.c          # Entry point, TUI event loop, background thread dispatch
├── api.c           # HTTP layer + all API calls (search, album, track info)
├── download.c      # Track download logic + atomic file writes
├── tui.c           # Terminal UI rendering (frame buffer, input handling)
├── tag.c           # Audio tagging: FLAC/Vorbis comment writer + cover art
├── config.c        # Persistent save-path config (~/.config/squidget)
├── platform.c      # OS-native folder picker (AppleScript / zenity / Win32)
├── json.c / json.h # Minimal hand-written JSON parser (no cJSON dependency)
├── squidget.h      # Shared types, constants, and function declarations
├── thread.h        # Cross-platform thread/mutex abstraction (pthreads + Win32)
├── build.sh        # Universal build script (macOS + Linux auto-detection)
└── squidget.bat    # Windows build script (MinGW / MSVC)
```

<p align="right">(<a href="#readme-top">back to top</a>)</p>

---

<!-- CONTRIBUTING -->
## <a name="contributing"></a> Contributing

Contributions are what make open source great. Any contribution you make is **greatly appreciated**.

1. **Fork** the repository
2. **Create** your feature branch
   ```bash
   git checkout -b feature/YourFeatureName
   ```
3. **Commit** your changes with a clear message
   ```bash
   git commit -m "feat: add download queue support"
   ```
4. **Push** to your branch
   ```bash
   git push origin feature/YourFeatureName
   ```
5. **Open a Pull Request** and describe what you changed and why

> [!IMPORTANT]
> Please make sure your code compiles cleanly with `gcc -std=c11 -Wall -Wextra` and that you haven't introduced any memory leaks (run with AddressSanitizer: add `-fsanitize=address` to `build.sh`).

<p align="right">(<a href="#readme-top">back to top</a>)</p>

---

<!-- LICENSE -->
## <a name="license"></a> License

Distributed under the **MIT License**. See [`LICENSE.txt`](LICENSE.txt) for full details.

> [!WARNING]
> SquidGet is intended for **personal, non-commercial use only**. Always respect the terms of service of any streaming platform and only download content you have the right to access. The authors are not responsible for misuse.

<p align="right">(<a href="#readme-top">back to top</a>)</p>

---

<!-- MARKDOWN LINKS -->
[license-shield]: https://img.shields.io/github/license/Lollollolmymy/SquidGet?style=for-the-badge
[license-url]: https://github.com/Lollollolmymy/SquidGet/blob/main/LICENSE.txt
[stars-shield]: https://img.shields.io/github/stars/Lollollolmymy/SquidGet?style=for-the-badge
[stars-url]: https://github.com/Lollollolmymy/SquidGet/stargazers
[forks-shield]: https://img.shields.io/github/forks/Lollollolmymy/SquidGet?style=for-the-badge
[forks-url]: https://github.com/Lollollolmymy/SquidGet/network/members
[issues-shield]: https://img.shields.io/github/issues/Lollollolmymy/SquidGet?style=for-the-badge
[issues-url]: https://github.com/Lollollolmymy/SquidGet/issues
