<a name="readme-top"></a>

[![License][license-shield]][license-url]
[![Stars][stars-shield]][stars-url]
[![Forks][forks-shield]][forks-url]
[![Issues][issues-shield]][issues-url]

<br />
<div align="center">
  <h1>SquidGet</h1>
  <p><strong>A fast terminal music downloader written in C11.</strong><br/>
  No Electron. No Python. No FFmpeg. No external downloader process.</p>

  <p>
    <a href="https://github.com/Lollollolmymy/SquidGet/issues/new?template=bug_report.md">Report a Bug</a>
    ·
    <a href="https://github.com/Lollollolmymy/SquidGet/issues/new?template=feature_request.md">Request a Feature</a>
    ·
    <a href="https://github.com/Lollollolmymy/SquidGet/wiki">Documentation</a>
  </p>
</div>

---

<details>
  <summary>Table of Contents</summary>
  <ol>
    <li><a href="#about">About</a></li>
    <li><a href="#features">Features</a></li>
    <li><a href="#getting-started">Getting Started</a></li>
    <li><a href="#usage">Usage</a></li>
    <li><a href="#keybindings">Keybindings</a></li>
    <li><a href="#quality-levels">Quality Levels</a></li>
    <li><a href="#configuration">Configuration</a></li>
    <li><a href="#project-structure">Project Structure</a></li>
    <li><a href="#contributing">Contributing</a></li>
    <li><a href="#license">License</a></li>
  </ol>
</details>

---

## <a name="about"></a> About

SquidGet is a lightweight terminal UI music downloader built in **C11**. It supports song search, album search, playlist import, quality selection, parallel album and playlist downloads, automatic tagging, and cover-art embedding.

The current backend is built around direct/public resolver paths. Lucida/Lica is intentionally not used in the normal download path because it was slow and could route downloads through a server-side FFmpeg pipeline.

### Built With

![C](https://img.shields.io/badge/C11-00599C?style=for-the-badge&logo=c&logoColor=white)
![Shell](https://img.shields.io/badge/Bash-4EAA25?style=for-the-badge&logo=gnubash&logoColor=white)
![Windows](https://img.shields.io/badge/Windows-WinHTTP-0078D6?style=for-the-badge&logo=windows&logoColor=white)
![macOS](https://img.shields.io/badge/macOS-libcurl-000000?style=for-the-badge&logo=apple&logoColor=white)
![Linux](https://img.shields.io/badge/Linux-libcurl-FCC624?style=for-the-badge&logo=linux&logoColor=black)

<p align="right">(<a href="#readme-top">back to top</a>)</p>

---

## <a name="features"></a> Features

| Feature | Description |
|---|---|
| **Interactive TUI** | Keyboard-driven terminal interface for searching, browsing, and downloading |
| **Song Search** | Search individual tracks and choose a requested quality before downloading |
| **Album Search** | Browse albums and download full releases with parallel workers |
| **Playlist Import** | Import public Spotify and Apple Music playlist pages without API keys |
| **Spotify Embed Fallback** | Uses Spotify embed metadata when the normal playlist page hides track rows |
| **Public Resolver First** | Prefers the public/community Qobuz resolver path before other direct fallbacks |
| **Lucida Disabled** | Avoids the old slow Lucida/Lica proxy path in normal operation |
| **Accept Any Real Media Type** | Selected quality is a request, not a hard file-type gate; SquidGet saves the actual media type returned |
| **HTML/Error Guard** | Rejects HTML, JSON, XML, and text error pages so they are not saved as songs |
| **Resolver Retry and Cooldown** | Handles temporary resolver failures and rate-limit style responses more safely |
| **Caching** | Caches Qobuz IDs, stream URLs, and cover-art fetches to reduce duplicate network work |
| **Auto Tagging** | Writes common metadata and embeds cover art when possible |
| **Optional Lyrics** | Lyrics fetching is off by default for speed and can be enabled with an environment variable |
| **Cross Platform** | Builds on macOS, Linux, and Windows |

<p align="right">(<a href="#readme-top">back to top</a>)</p>

---

## <a name="getting-started"></a> Getting Started

### Prerequisites

| Platform | Requirement |
|---|---|
| **macOS** | Xcode Command Line Tools, Homebrew `curl`, and `pkg-config` |
| **Linux** | `gcc`, `pkg-config`, and libcurl development headers |
| **Windows** | Bundled TCC toolchain or a compatible C compiler |

> [!NOTE]
> macOS and Linux builds link against libcurl. SquidGet does not shell out to the `curl` command at runtime.

### macOS

```bash
brew install curl pkg-config
./build.sh
```

### Linux

Debian/Ubuntu:

```bash
sudo apt update
sudo apt install -y gcc pkg-config libcurl4-openssl-dev
./build.sh
```

Arch:

```bash
sudo pacman -S gcc pkg-config curl
./build.sh
```

Fedora:

```bash
sudo dnf install -y gcc pkg-config libcurl-devel
./build.sh
```

### Windows

```cmd
squidget.bat
```

<p align="right">(<a href="#readme-top">back to top</a>)</p>

---

## <a name="usage"></a> Usage

Run SquidGet:

```bash
./squidget
```

Basic flow:

1. Type a song, album, or playlist URL.
2. Press `Enter` to search.
3. Use `Tab` to switch between songs, albums, and playlists.
4. Use the arrow keys to select a result.
5. Press `Enter` to open the quality picker or browse a result.
6. Pick a quality and SquidGet downloads in the background.

### Playlist Mode

Paste a public Spotify or Apple Music playlist URL in playlist mode. SquidGet reads public page metadata, opens a playlist track list, and lets you download one track or the entire playlist.

Plain-text playlist downloads are also supported:

```bash
squidget playlist tracks.txt lossless ~/Music/SquidGet
```

Each non-empty line is searched as a track query. Lines starting with `#` are ignored.

> [!NOTE]
> Private playlists and pages that hide metadata may not be readable.

<p align="right">(<a href="#readme-top">back to top</a>)</p>

---

## <a name="keybindings"></a> Keybindings

| Key | Action |
|---|---|
| `Up` / `Down` | Move cursor up or down |
| `Enter` | Search, select, or confirm |
| `Tab` | Toggle between Song, Album, and Playlist search |
| `PgUp` / `PgDn` | Scroll by one page |
| `Home` / `End` | Jump to first or last result |
| `Backspace` | Delete last character |
| `Ctrl+U` | Clear the search field |
| `Esc` | Go back or cancel current menu |
| `Ctrl+C` | Quit SquidGet |

<p align="right">(<a href="#readme-top">back to top</a>)</p>

---

## <a name="quality-levels"></a> Quality Levels

| # | Label | Request |
|---|---|---|
| 1 | Hi-Res Lossless | Highest available lossless stream |
| 2 | Lossless | CD-quality/lossless stream when available |
| 3 | High | High-quality lossy stream when available |
| 4 | Low | Low-bandwidth lossy stream when available |
| 5 | Dolby Atmos | Spatial/Atmos stream when available |

The quality picker does not gray out tiers anymore. SquidGet sends the selected tier to the resolver, downloads the stream that comes back, sniffs the returned media/container, and saves it with the actual extension.

Accepted media includes FLAC, MP4/M4A, MP3, Ogg, and related real audio/video container responses. HTML, JSON, XML, and plain-text error responses are rejected and deleted instead of being saved as songs.

<p align="right">(<a href="#readme-top">back to top</a>)</p>

---

## <a name="configuration"></a> Configuration

SquidGet stores its config under your normal user config folder, for example:

```text
~/.config/squidget/
```

Useful environment variables:

| Variable | Default | Description |
|---|---:|---|
| `SQUIDGET_DEBUG` | `0` | Set to `1` to enable debug logging |
| `SQUIDGET_FETCH_LYRICS` | `0` | Set to `1` to fetch lyrics during tagging |
| `SQUIDGET_ALBUM_THREADS` | `2` | Album worker count |
| `SQUIDGET_PLAYLIST_THREADS` | `2` | Playlist worker count |
| `SQUIDGET_SERIAL_RESOLVER` | `1` | Serializes resolver calls to avoid burst throttling |
| `SQUIDGET_RESOLVER_GAP_MS` | `250` | Minimum spacing between resolver calls |
| `SQUIDGET_TRACK_RETRIES` | `2` | Retry count for failed stream responses |
| `SQUIDGET_RETRY_DELAY_MS` | `1500` | Initial retry delay |
| `SQUIDGET_RETRY_DELAY_MAX_MS` | `15000` | Maximum retry delay |
| `SQUIDGET_QOBUZ_RESOLVER_URL` | unset | Optional custom direct resolver URL. Lucida/Lica URLs are rejected. |

Example:

```bash
SQUIDGET_DEBUG=1 SQUIDGET_FETCH_LYRICS=1 ./squidget
```

<p align="right">(<a href="#readme-top">back to top</a>)</p>

---

## <a name="project-structure"></a> Project Structure

```text
SquidGet/
├── main.c          # Entry point, TUI event loop, background task dispatch
├── api.c           # HTTP layer, search, resolver, album, and playlist metadata
├── download.c      # Stream download logic, file sniffing, retry handling
├── tui.c           # Terminal UI rendering and input handling
├── tag.c           # Audio tagging and cover art embedding
├── config.c        # Persistent save-path config
├── platform.c      # OS-native helpers
├── json.c / json.h # Minimal JSON parser
├── squidget.h      # Shared types and declarations
├── thread.h        # Cross-platform thread/mutex abstraction
├── build.sh        # macOS/Linux build script
├── squidget.bat    # Windows build script
└── tcc/            # Bundled Windows TCC toolchain
```

<p align="right">(<a href="#readme-top">back to top</a>)</p>

---

## <a name="contributing"></a> Contributing

Pull requests are welcome. Keep changes focused, avoid adding runtime command-line dependencies, and prefer simple resolver behavior over complicated fuzzy matching.

1. Fork the repo.
2. Create a feature branch.
3. Build and test on your platform.
4. Open a pull request with a clear summary.

<p align="right">(<a href="#readme-top">back to top</a>)</p>

---

## <a name="license"></a> License

Distributed under the MIT License. See `LICENSE.txt` for more information.

> [!IMPORTANT]
> Use SquidGet only with content you have the right to access and download. Respect the terms of the services and sources you use.

<p align="right">(<a href="#readme-top">back to top</a>)</p>

---

[license-shield]: https://img.shields.io/github/license/Lollollolmymy/SquidGet.svg?style=for-the-badge
[license-url]: https://github.com/Lollollolmymy/SquidGet/blob/main/LICENSE.txt
[stars-shield]: https://img.shields.io/github/stars/Lollollolmymy/SquidGet.svg?style=for-the-badge
[stars-url]: https://github.com/Lollollolmymy/SquidGet/stargazers
[forks-shield]: https://img.shields.io/github/forks/Lollollolmymy/SquidGet.svg?style=for-the-badge
[forks-url]: https://github.com/Lollollolmymy/SquidGet/network/members
[issues-shield]: https://img.shields.io/github/issues/Lollollolmymy/SquidGet.svg?style=for-the-badge
[issues-url]: https://github.com/Lollollolmymy/SquidGet/issues
