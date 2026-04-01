#include "squidget.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#ifndef _WIN32
#  include <curl/curl.h>
#endif

/* ── resolve_out_dir ────────────────────────────────────────────────────
   Always resolve to a fixed, predictable output directory regardless of
   how the app was launched (terminal, Finder, double-click, etc).

   Root cause of the original bug: when launched from Finder on macOS,
   the CWD is set by Terminal.app to the user's HOME directory (or even
   "/" on some paths), so "." as an output dir produced files in an
   unexpected or read-only location.

   Priority:
     1. ~/Music/squidget/   — created on first use; logical for a music app
     2. ~/Downloads/        — common fallback
     3. ~/Desktop/          — always visible and writable
     4. ~                   — home directory itself as last resort
     5. /tmp                — absolute last resort (on POSIX)

   We deliberately skip the CWD so the output location is the same
   whether the user runs from a terminal, double-clicks in Finder,
   or uses any other launch method.
   ──────────────────────────────────────────────────────────────────────── */
static int try_dir(const char *dir, char *buf, size_t bufsz) {
    /* try to write a probe file; return 1 on success and fill buf */
    char probe[600];
    snprintf(probe, sizeof(probe), "%s/.sqt_probe", dir);
    FILE *f = fopen(probe, "wb");
    if (!f) return 0;
    fclose(f); remove(probe);
    snprintf(buf, bufsz, "%s", dir);
    return 1;
}

#ifndef _WIN32
#include <sys/stat.h>  /* mkdir */
#endif

static void resolve_out_dir(char *buf, size_t bufsz) {
#ifdef _WIN32
    /* On Windows the CWD is always the expected location */
    buf[0] = '.'; buf[1] = '\0';
#else
    const char *home = getenv("HOME");

    if (home && *home) {
        /* Preferred: ~/Music/squidget/ — create if absent */
        char music_squidget[600];
        snprintf(music_squidget, sizeof(music_squidget),
                 "%s/Music/squidget", home);
        /* mkdir -p equivalent: create ~/Music/ then ~/Music/squidget/ */
        char music[600];
        snprintf(music, sizeof(music), "%s/Music", home);
        mkdir(music,          0755);   /* ignore EEXIST */
        mkdir(music_squidget, 0755);   /* ignore EEXIST */
        if (try_dir(music_squidget, buf, bufsz)) return;

        /* Fallback: ~/Downloads/ */
        char downloads[600];
        snprintf(downloads, sizeof(downloads), "%s/Downloads", home);
        if (try_dir(downloads, buf, bufsz)) return;

        /* Fallback: ~/Desktop/ */
        char desktop[600];
        snprintf(desktop, sizeof(desktop), "%s/Desktop", home);
        if (try_dir(desktop, buf, bufsz)) return;

        /* Fallback: ~ itself */
        if (try_dir(home, buf, bufsz)) return;
    }

    /* Absolute last resort: /tmp */
    snprintf(buf, bufsz, "/tmp");
#endif
}

/* ── mkdir_p: create a directory (and its parent) cross-platform ── */
static void mkdir_p(const char *path) {
#ifdef _WIN32
    /* CreateDirectoryA will fail if parent doesn't exist; make parent first */
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '\\' || *p == '/') {
            char c = *p; *p = '\0';
            CreateDirectoryA(tmp, NULL);
            *p = c;
        }
    }
    CreateDirectoryA(tmp, NULL);
#else
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
#endif
}

/* ── build_setup_presets: populate OS-appropriate preset list ── */
static void build_setup_presets(AppState *s) {
    int n = 0;

#ifdef _WIN32
    /* Windows presets use %USERPROFILE% */
    const char *up = getenv("USERPROFILE");
    if (!up || !*up) up = "C:\\Users\\User";

    snprintf(s->setup_presets[n], sizeof(s->setup_presets[0]),
             "%s\\Music\\squidget", up);
    snprintf(s->setup_labels[n],  sizeof(s->setup_labels[0]),
             "Music\\squidget  [recommended]");
    n++;

    snprintf(s->setup_presets[n], sizeof(s->setup_presets[0]),
             "%s\\Downloads\\squidget", up);
    snprintf(s->setup_labels[n],  sizeof(s->setup_labels[0]),
             "Downloads\\squidget");
    n++;

    snprintf(s->setup_presets[n], sizeof(s->setup_presets[0]),
             "%s\\Desktop\\squidget", up);
    snprintf(s->setup_labels[n],  sizeof(s->setup_labels[0]),
             "Desktop\\squidget");
    n++;

#else
    /* macOS / Linux presets use $HOME */
    const char *home = getenv("HOME");
    if (!home || !*home) home = "/tmp";

    snprintf(s->setup_presets[n], sizeof(s->setup_presets[0]),
             "%s/Music/squidget", home);

#  ifdef __APPLE__
    snprintf(s->setup_labels[n],  sizeof(s->setup_labels[0]),
             "~/Music/squidget  [recommended]");
#  else
    /* On Linux, check whether ~/Music already exists (XDG convention) */
    char music[512];
    snprintf(music, sizeof(music), "%s/Music", home);
    snprintf(s->setup_labels[n], sizeof(s->setup_labels[0]),
             "~/Music/squidget  [recommended]");
#  endif
    n++;

    snprintf(s->setup_presets[n], sizeof(s->setup_presets[0]),
             "%s/Downloads", home);
    snprintf(s->setup_labels[n],  sizeof(s->setup_labels[0]),
             "~/Downloads");
    n++;

    snprintf(s->setup_presets[n], sizeof(s->setup_presets[0]),
             "%s/Desktop", home);
    snprintf(s->setup_labels[n],  sizeof(s->setup_labels[0]),
             "~/Desktop");
    n++;
#endif

    /* Last option: Browse… (empty path = signal to open GUI picker) */
    s->setup_presets[n][0] = '\0';
    snprintf(s->setup_labels[n], sizeof(s->setup_labels[0]),
             "Browse\xe2\x80\xa6");   /* "Browse…" in UTF-8 */
    n++;

    s->setup_count  = n;
    s->setup_cursor = 0;
}

typedef struct {
    AppState *s;
    char      query[512];
    char      track_id[SQT_ID_SZ];
    char      quality[SQT_QUAL_SZ];
    enum { BG_SEARCH, BG_DOWNLOAD } task;
} BgCtx;

static void dl_progress_cb(const char *msg, void *ud) {
    AppState *s = ud;
    sqt_mutex_lock(&s->lock);
    snprintf(s->status, sizeof(s->status), "%s", msg);
    s->dirty = 1;
    sqt_mutex_unlock(&s->lock);
}

static SQT_THREAD_FN bg_worker(void *arg) {
    BgCtx    *ctx = arg;
    AppState *s   = ctx->s;

    switch (ctx->task) {

    case BG_SEARCH: {
        Track tmp[SQT_MAX_RESULTS];
        int n = api_search_tracks(ctx->query, tmp, SQT_MAX_RESULTS);
        sqt_mutex_lock(&s->lock);
        memcpy(s->tracks, tmp, (size_t)n * sizeof(Track));
        s->track_count = n;
        s->cursor = s->scroll = 0;
        s->mode   = MODE_RESULTS;
        snprintf(s->status, sizeof(s->status), "%d track%s", n, n == 1 ? "" : "s");
        s->dirty = 1;
        sqt_mutex_unlock(&s->lock);
        break;
    }

    case BG_DOWNLOAD: {
        /* find the track by id */
        sqt_mutex_lock(&s->lock);
        Track t = {0};
        for (int i = 0; i < s->track_count; i++) {
            if (strcmp(s->tracks[i].id, ctx->track_id) == 0) { t = s->tracks[i]; break; }
        }
        sqt_mutex_unlock(&s->lock);
        download_track(&t, ctx->quality, s->out_dir, dl_progress_cb, s);
        break;
    }

    }

    sqt_mutex_lock(&s->lock);
    s->bg_running = 0;
    sqt_mutex_unlock(&s->lock);
    free(ctx);
#ifndef _WIN32
    return NULL;
#else
    return 0;
#endif
}

static void launch_bg(AppState *s, BgCtx *ctx) {
    sqt_mutex_lock(&s->lock);
    if (s->bg_running) { sqt_mutex_unlock(&s->lock); free(ctx); return; }
    s->bg_running = 1;
    sqt_mutex_unlock(&s->lock);
    if (sqt_thread_create(&s->bg_thread, bg_worker, ctx) != 0) {
        /* thread failed to start — reset flag so app isn't permanently locked */
        sqt_mutex_lock(&s->lock);
        s->bg_running = 0;
        snprintf(s->status, sizeof(s->status), "error: failed to start thread");
        s->dirty = 1;
        sqt_mutex_unlock(&s->lock);
        free(ctx);
    }
}

static void do_search(AppState *s) {
    if (!*s->query) return;
    BgCtx *ctx = calloc(1, sizeof(BgCtx));
    if (!ctx) return;
    ctx->s = s;
    ctx->task = BG_SEARCH;
    snprintf(ctx->query, sizeof(ctx->query), "%s", s->query);
    snprintf(s->status, sizeof(s->status), "searching…");
    s->dirty = 1;
    launch_bg(s, ctx);
}

static void start_download(AppState *s, int cursor, int qual_idx) {
    if (cursor < 0 || cursor >= s->track_count) return;
    BgCtx *ctx = calloc(1, sizeof(BgCtx));
    if (!ctx) return;
    ctx->s = s;
    ctx->task = BG_DOWNLOAD;
    snprintf(ctx->track_id, sizeof(ctx->track_id), "%s", s->tracks[cursor].id);
    snprintf(ctx->quality,  sizeof(ctx->quality),  "%s", QUALITY_LABELS[qual_idx]);
    snprintf(s->status, sizeof(s->status), "starting download…");
    s->dirty = 1;
    launch_bg(s, ctx);
}

static AppState *g_sig_state = NULL;
#ifndef _WIN32
/* Bug 12: use sig_atomic_t so signal handler write is safe without lock */
static volatile sig_atomic_t g_resize_pending = 0;
static void on_sigwinch(int _) { (void)_; g_resize_pending = 1; }
/* Bug 13: restore terminal on kill/hangup */
static void on_fatal_sig(int _) {
    (void)_;
    if (g_sig_state) tui_cleanup(g_sig_state);
    _exit(1);
}
#endif

int main(void) {
#ifdef _WIN32
    /* Force UTF-8 output so box-drawing / Unicode renders correctly
       regardless of the system's default OEM codepage (e.g. CP437/CP1252). */
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#else
    curl_global_init(CURL_GLOBAL_DEFAULT);
#endif

    AppState s;
    memset(&s, 0, sizeof(s));
    sqt_mutex_init(&s.lock);

    /* ── first-run setup vs. existing config ── */
    if (config_load(s.out_dir, sizeof(s.out_dir))) {
        /* config found — go straight to search */
        snprintf(s.status, sizeof(s.status), "saving to: %.230s", s.out_dir);
        s.mode = MODE_SEARCH;
    } else {
        /* no config — show the setup screen */
        build_setup_presets(&s);
        s.mode = MODE_SETUP;
        snprintf(s.status, sizeof(s.status), "choose a save location to get started");
    }

    g_sig_state = &s;
#ifndef _WIN32
    signal(SIGWINCH, on_sigwinch);
    signal(SIGPIPE,  SIG_IGN);
    signal(SIGTERM,  on_fatal_sig);
    signal(SIGHUP,   on_fatal_sig);
#endif

    tui_init(&s);
    tui_render(&s);

    while (1) {
        int key = tui_read_key(&s);

        if (key == 0) {
            sqt_mutex_lock(&s.lock);
            if (s.bg_running) { s.spin_frame++; s.dirty = 1; }
#ifndef _WIN32
            /* Bug 12: drain resize flag written by signal handler */
            if (g_resize_pending) { g_resize_pending = 0; s.dirty = 1; }
#else
            /* Windows has no SIGWINCH — poll console size each idle tick */
            {
                CONSOLE_SCREEN_BUFFER_INFO _cbi;
                if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &_cbi)) {
                    int _c = _cbi.srWindow.Right  - _cbi.srWindow.Left + 1;
                    int _r = _cbi.srWindow.Bottom - _cbi.srWindow.Top  + 1;
                    if (_r != s.rows || _c != s.cols) s.dirty = 1;
                }
            }
#endif
            sqt_mutex_unlock(&s.lock);
            tui_render(&s);
            continue;
        }

        if (key == KEY_CTRL_C) break;

        sqt_mutex_lock(&s.lock);

        switch (s.mode) {

        /* ── search / results ── */
        case MODE_SEARCH:
        case MODE_RESULTS: {
            /* enter: select track or run search */
            if (key == KEY_ENTER) {
                if (s.mode == MODE_RESULTS && s.cursor >= 0 && s.cursor < s.track_count) {
                    s.mode = MODE_QUALITY;
                    s.qual_cursor = 0;
                    s.dirty = 1;
                    break;
                }
                sqt_mutex_unlock(&s.lock);
                do_search(&s);
                continue;
            }
            if (key == KEY_UP) {
                if (s.mode == MODE_RESULTS && s.cursor > 0) {
                    s.cursor--;
                    if (s.cursor < s.scroll) s.scroll = s.cursor;
                    s.dirty = 1;
                }
                break;
            }
            if (key == KEY_DOWN) {
                if (s.mode == MODE_RESULTS && s.cursor < s.track_count - 1) {
                    s.cursor++;
                    int vis = s.rows - HEADER_ROWS - FOOTER_ROWS;
                    if (s.cursor >= s.scroll + vis) s.scroll = s.cursor - vis + 1;
                    s.dirty = 1;
                }
                break;
            }
            if (key == KEY_HOME) { if (s.mode == MODE_RESULTS) { s.cursor = 0; s.scroll = 0; s.dirty = 1; } break; }
            if (key == KEY_END)  { if (s.mode == MODE_RESULTS) { s.cursor = s.track_count - 1; s.dirty = 1; } break; }
            if (key == KEY_PGUP) {
                if (s.mode == MODE_RESULTS) {
                    int vis = s.rows - HEADER_ROWS - FOOTER_ROWS;
                    s.cursor = s.cursor > vis ? s.cursor - vis : 0;
                    if (s.cursor < s.scroll) s.scroll = s.cursor;
                    s.dirty = 1;
                }
                break;
            }
            if (key == KEY_PGDN) {
                if (s.mode == MODE_RESULTS && s.track_count > 0) {
                    int vis = s.rows - HEADER_ROWS - FOOTER_ROWS;
                    s.cursor += vis;
                    if (s.cursor >= s.track_count) s.cursor = s.track_count - 1;
                    if (s.cursor >= s.scroll + vis) s.scroll = s.cursor - vis + 1;
                    s.dirty = 1;
                }
                break;
            }
            if (key == KEY_BACKSPACE) {
                if (s.query_len > 0) {
                    s.query_len--;
                    while (s.query_len > 0 && ((unsigned char)s.query[s.query_len] & 0xC0) == 0x80)
                        s.query_len--;
                    s.query[s.query_len] = '\0';
                    s.dirty = 1;
                }
                break;
            }
            /* / clears query and restarts search */
            if (key == '/' && s.mode == MODE_RESULTS) {
                s.query[0] = '\0'; s.query_len = 0;
                s.mode = MODE_SEARCH;
                s.track_count = 0;
                s.cursor = s.scroll = 0;
                s.dirty = 1;
                break;
            }
            /* any printable char: if in results, clear and start new search */
            if (key >= 32 && key < 256 && s.query_len < (int)sizeof(s.query) - 2) {
                if (s.mode == MODE_RESULTS) {
                    s.query[0] = '\0'; s.query_len = 0;
                    s.mode = MODE_SEARCH;
                    s.track_count = 0;
                    s.cursor = s.scroll = 0;
                }
                s.query[s.query_len++] = (char)key;
                s.query[s.query_len]   = '\0';
                s.dirty = 1;
            }
            break;
        }

        /* ── quality picker ── */
        case MODE_QUALITY: {
            if (key == KEY_ESC) {
                s.mode = MODE_RESULTS; s.dirty = 1; break;
            }
            if ((key == KEY_UP || key == 'k') && s.qual_cursor > 0) {
                s.qual_cursor--; s.dirty = 1; break;
            }
            if ((key == KEY_DOWN || key == 'j') && s.qual_cursor < QUALITY_COUNT - 1) {
                s.qual_cursor++; s.dirty = 1; break;
            }
            if (key >= '1' && key <= '0' + QUALITY_COUNT)
                s.qual_cursor = key - '1';
            if (key == KEY_ENTER || (key >= '1' && key <= '0' + QUALITY_COUNT)) {
                int qi     = s.qual_cursor;
                int cursor = s.cursor;
                s.mode  = MODE_RESULTS;
                s.dirty = 1;
                sqt_mutex_unlock(&s.lock);
                start_download(&s, cursor, qi);
                continue;
            }
            break;
        }

        /* ── setup (first-run) ── */
        case MODE_SETUP: {
            if (key == KEY_UP && s.setup_cursor > 0) {
                s.setup_cursor--; s.dirty = 1; break;
            }
            if (key == KEY_DOWN && s.setup_cursor < s.setup_count - 1) {
                s.setup_cursor++; s.dirty = 1; break;
            }
            /* number keys 1..N as shortcuts */
            if (key >= '1' && key < '1' + s.setup_count) {
                s.setup_cursor = key - '1'; s.dirty = 1; break;
            }
            if (key == KEY_ENTER) {
                int sel = s.setup_cursor;
                int is_browse = (s.setup_presets[sel][0] == '\0');

                if (is_browse) {
                    /* ── open native folder picker ──
                     * We must leave raw/TUI mode before calling the external
                     * picker, then come back afterwards.                     */
                    sqt_mutex_unlock(&s.lock);
                    tui_cleanup(&s);

                    /* Free the old frame buffer so tui_render gets a clean
                     * slate — the terminal was cleared by tui_cleanup, and
                     * fb_flush skips rows whose prev==cur, which would leave
                     * the screen blank if dimensions haven't changed.        */
                    if (s.fb) { free(s.fb); s.fb = NULL; }

                    char picked[512] = {0};
                    int ok = gui_pick_folder(picked, sizeof(picked));

                    tui_init(&s);
                    sqt_mutex_lock(&s.lock);

                    if (ok && picked[0]) {
                        snprintf(s.out_dir, sizeof(s.out_dir), "%s", picked);
                        mkdir_p(s.out_dir);
                        config_save(s.out_dir);
                        snprintf(s.status, sizeof(s.status),
                                 "saving to: %.230s", s.out_dir);
                        s.mode  = MODE_SEARCH;
                        s.dirty = 1;
                    } else {
                        /* user cancelled — stay in setup */
                        snprintf(s.status, sizeof(s.status),
                                 "no folder chosen — pick again or use a preset");
                        s.dirty = 1;
                    }
                    break;

                } else {
                    /* preset chosen */
                    snprintf(s.out_dir, sizeof(s.out_dir), "%s",
                             s.setup_presets[sel]);
                    mkdir_p(s.out_dir);
                    config_save(s.out_dir);
                    snprintf(s.status, sizeof(s.status),
                             "saving to: %.230s", s.out_dir);
                    s.mode  = MODE_SEARCH;
                    s.dirty = 1;
                }
                break;
            }
            break;
        }

        default: break;
        }

        s.dirty = 1;
        sqt_mutex_unlock(&s.lock);
        tui_render(&s);
    }

    tui_cleanup(&s);

    sqt_mutex_lock(&s.lock);
    int was_running = s.bg_running;
    sqt_mutex_unlock(&s.lock);
    if (was_running) sqt_thread_join(s.bg_thread);

    if (s.fb) free(s.fb);
    sqt_mutex_destroy(&s.lock);
#ifndef _WIN32
    curl_global_cleanup();
#endif
    return 0;
}
