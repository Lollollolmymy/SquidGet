#include "squidget.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#ifndef _WIN32
#include <sys/stat.h>  /* mkdir */
#endif

// make dirs recursively (windows & unix)
static void mkdir_p(const char *path) {
#ifdef _WIN32
    // gotta make parent dirs first on windows
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

// default save locations for each os
static void build_setup_presets(AppState *s) {
    int n = 0;

#ifdef _WIN32
    // windows uses userprofile
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
    // mac/linux use home dir
    const char *home = getenv("HOME");
    if (!home || !*home) home = "/tmp";

    snprintf(s->setup_presets[n], sizeof(s->setup_presets[0]),
             "%s/Music/squidget", home);

    snprintf(s->setup_labels[n], sizeof(s->setup_labels[0]),
             "~/Music/squidget  [recommended]");
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

    // last option: custom folder picker
    s->setup_presets[n][0] = '\0';
    snprintf(s->setup_labels[n], sizeof(s->setup_labels[0]),
             "Browse\xe2\x80\xa6");
    n++;

    s->setup_count  = n;
    s->setup_cursor = 0;
}

typedef struct {
    AppState *s;
    char      query[512];
    char      track_id[SQT_ID_SZ];
    char      album_id[SQT_ID_SZ];
    char      album_name[SQT_TITLE_SZ];
    char      quality[SQT_QUAL_SZ];
    enum { BG_SEARCH, BG_DOWNLOAD, BG_ALBUM_SEARCH, BG_ALBUM_TRACKS, BG_ALBUM_DOWNLOAD } task;
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

    case BG_ALBUM_SEARCH: {
        Album tmp[SQT_MAX_RESULTS];
        int n = api_search_albums(ctx->query, tmp, SQT_MAX_RESULTS);
        sqt_mutex_lock(&s->lock);
        memcpy(s->albums, tmp, (size_t)n * sizeof(Album));
        s->album_count = n;
        s->cursor = s->scroll = 0;
        s->mode   = MODE_RESULTS;
        snprintf(s->status, sizeof(s->status), "%d album%s", n, n == 1 ? "" : "s");
        s->dirty = 1;
        sqt_mutex_unlock(&s->lock);
        break;
    }

    case BG_ALBUM_TRACKS: {
        Track tmp[SQT_MAX_RESULTS];
        int n = api_get_album_tracks(ctx->album_id, tmp, SQT_MAX_RESULTS);
        sqt_mutex_lock(&s->lock);
        memcpy(s->tracks, tmp, (size_t)n * sizeof(Track));
        s->track_count = n;
        s->cursor = s->scroll = 0;
        s->search_type = SEARCH_SONGS;   /* switch to song view to browse */
        s->mode        = MODE_RESULTS;
        snprintf(s->status, sizeof(s->status), "%d track%s  (TAB to return to album search)", n, n == 1 ? "" : "s");
        s->dirty = 1;
        sqt_mutex_unlock(&s->lock);
        break;
    }

    case BG_ALBUM_DOWNLOAD: {
        /* fetch all tracks for the album, then download each to album subfolder */
        Track tracks[SQT_MAX_RESULTS];
        int n = api_get_album_tracks(ctx->album_id, tracks, SQT_MAX_RESULTS);
        if (n <= 0) {
            sqt_mutex_lock(&s->lock);
            snprintf(s->status, sizeof(s->status), "error: could not fetch album tracks");
            s->dirty = 1;
            sqt_mutex_unlock(&s->lock);
            break;
        }
        
        /* Create album subfolder */
        char album_path[512];
        snprintf(album_path, sizeof(album_path), "%s/%s", s->out_dir, ctx->album_name);
        mkdir_p(album_path);
        
        /* Download each track to album subfolder */
        for (int i = 0; i < n; i++) {
            sqt_mutex_lock(&s->lock);
            snprintf(s->status, sizeof(s->status),
                     "album: %d/%d — %s", i + 1, n, tracks[i].title);
            s->dirty = 1;
            sqt_mutex_unlock(&s->lock);
            download_track(&tracks[i], ctx->quality, album_path, dl_progress_cb, s);
        }
        sqt_mutex_lock(&s->lock);
        snprintf(s->status, sizeof(s->status), "album done! (%d tracks)", n);
        s->dirty = 1;
        sqt_mutex_unlock(&s->lock);
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
    if (!ctx) {
        snprintf(s->status, sizeof(s->status), "error: out of memory");
        s->dirty = 1;
        return;
    }
    ctx->s = s;
    ctx->task = BG_SEARCH;
    snprintf(ctx->query, sizeof(ctx->query), "%s", s->query);
    snprintf(s->status, sizeof(s->status), "searching…");
    s->dirty = 1;
    launch_bg(s, ctx);
}

static void do_album_search(AppState *s) {
    if (!*s->query) return;
    BgCtx *ctx = calloc(1, sizeof(BgCtx));
    if (!ctx) {
        snprintf(s->status, sizeof(s->status), "error: out of memory");
        s->dirty = 1;
        return;
    }
    ctx->s = s;
    ctx->task = BG_ALBUM_SEARCH;
    snprintf(ctx->query, sizeof(ctx->query), "%s", s->query);
    snprintf(s->status, sizeof(s->status), "searching albums…");
    s->dirty = 1;
    launch_bg(s, ctx);
}

static void start_album_track_browse(AppState *s, int cursor) {
    if (cursor < 0 || cursor >= s->album_count) return;
    BgCtx *ctx = calloc(1, sizeof(BgCtx));
    if (!ctx) return;
    ctx->s = s;
    ctx->task = BG_ALBUM_TRACKS;
    snprintf(ctx->album_id, sizeof(ctx->album_id), "%s", s->albums[cursor].id);
    snprintf(s->status, sizeof(s->status), "fetching tracks…");
    s->dirty = 1;
    launch_bg(s, ctx);
}

static void start_album_download(AppState *s, int cursor) {
    if (cursor < 0 || cursor >= s->album_count) return;
    BgCtx *ctx = calloc(1, sizeof(BgCtx));
    if (!ctx) return;
    ctx->s = s;
    ctx->task = BG_ALBUM_DOWNLOAD;
    snprintf(ctx->album_id, sizeof(ctx->album_id), "%s", s->albums[cursor].id);
    snprintf(ctx->album_name, sizeof(ctx->album_name), "%s", s->albums[cursor].title);
    snprintf(ctx->quality,  sizeof(ctx->quality),  "%s", QUALITY_LABELS[0]);  /* default: best */
    snprintf(s->status, sizeof(s->status), "starting album download…");
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

#ifndef _WIN32
#  include <unistd.h>
static AppState *g_sig_state = NULL;
static volatile sig_atomic_t g_resize_pending = 0;
static void on_sigwinch(int _) { (void)_; g_resize_pending = 1; }
static void on_fatal_sig(int _) {
    (void)_;
    /* async-safe cleanup: use write() not fputs() */
    const char *cleanup = "\033[?25h\033[0m\033[?1049l\n";
    write(STDOUT_FILENO, cleanup, 30);
    _exit(1);
}
#endif

int main(void) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    AppState s;
    memset(&s, 0, sizeof(s));
    sqt_mutex_init(&s.lock);

    // check if configured
    if (config_load(s.out_dir, sizeof(s.out_dir))) {
        /* config found — ensure the directory exists, then go straight to search */
        mkdir_p(s.out_dir);
        snprintf(s.status, sizeof(s.status), "saving to: %.230s", s.out_dir);
        s.mode = MODE_SEARCH;
    } else {
        /* no config — show the setup screen */
        build_setup_presets(&s);
        s.mode = MODE_SETUP;
        snprintf(s.status, sizeof(s.status), "choose a save location to get started");
    }

#ifndef _WIN32
    g_sig_state = &s;
    signal(SIGWINCH, on_sigwinch);
    signal(SIGPIPE,  SIG_IGN);
    signal(SIGINT,  on_fatal_sig);
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
            if (g_resize_pending) { g_resize_pending = 0; s.dirty = 1; }
#else
            // windows doesn't have SIGWINCH so poll manually
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
            /* TAB: toggle between song search and album search */
            if (key == KEY_TAB) {
                s.search_type = (s.search_type == SEARCH_SONGS) ? SEARCH_ALBUMS : SEARCH_SONGS;
                s.track_count = 0;  /* clear counts first */
                s.album_count = 0;
                s.mode        = MODE_SEARCH;  /* then mode */
                s.cursor = s.scroll = 0;
                s.dirty = 1;
                break;
            }
            /* enter: select track/album or run search */
            if (key == KEY_ENTER) {
                if (s.mode == MODE_RESULTS) {
                    if (s.search_type == SEARCH_SONGS &&
                        s.cursor >= 0 && s.cursor < s.track_count) {
                        s.mode = MODE_QUALITY;
                        s.qual_cursor = 0;
                        s.dirty = 1;
                        break;
                    }
                    if (s.search_type == SEARCH_ALBUMS &&
                        s.cursor >= 0 && s.cursor < s.album_count) {
                        s.mode = MODE_ALBUM_ACTION;
                        s.album_action_cursor = 0;
                        s.dirty = 1;
                        break;
                    }
                }
                sqt_mutex_unlock(&s.lock);
                if (s.search_type == SEARCH_ALBUMS) do_album_search(&s);
                else                                 do_search(&s);
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
                int list_len = (s.search_type == SEARCH_ALBUMS) ? s.album_count : s.track_count;
                if (s.mode == MODE_RESULTS && s.cursor < list_len - 1) {
                    s.cursor++;
                    int vis = s.rows - HEADER_ROWS - FOOTER_ROWS - 1;
                    if (s.cursor >= s.scroll + vis) s.scroll = s.cursor - vis + 1;
                    s.dirty = 1;
                }
                break;
            }
            if (key == KEY_HOME) { if (s.mode == MODE_RESULTS) { s.cursor = 0; s.scroll = 0; s.dirty = 1; } break; }
            if (key == KEY_END) {
                int list_len = (s.search_type == SEARCH_ALBUMS) ? s.album_count : s.track_count;
                if (s.mode == MODE_RESULTS && list_len > 0) {
                    int vis = s.rows - HEADER_ROWS - FOOTER_ROWS - 1;
                    s.cursor = list_len - 1;
                    if (s.cursor >= s.scroll + vis) s.scroll = s.cursor - vis + 1;
                    s.dirty = 1;
                }
                break;
            }
            if (key == KEY_PGUP) {
                if (s.mode == MODE_RESULTS) {
                    int vis = s.rows - HEADER_ROWS - FOOTER_ROWS - 1;
                    s.cursor = s.cursor > vis ? s.cursor - vis : 0;
                    if (s.cursor < s.scroll) s.scroll = s.cursor;
                    s.dirty = 1;
                }
                break;
            }
            if (key == KEY_PGDN) {
                int list_len = (s.search_type == SEARCH_ALBUMS) ? s.album_count : s.track_count;
                if (s.mode == MODE_RESULTS && list_len > 0) {
                    int vis = s.rows - HEADER_ROWS - FOOTER_ROWS - 1;
                    s.cursor += vis;
                    if (s.cursor >= list_len) s.cursor = list_len - 1;
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
            /* Ctrl+U clears query manually */
            if (key == 21) {  /* Ctrl+U */
                s.query[0] = '\0'; s.query_len = 0;
                s.mode = MODE_SEARCH;
                s.track_count = 0;
                s.album_count = 0;
                s.cursor = s.scroll = 0;
                s.dirty = 1;
                break;
            }
            /* any printable char: append to query (don't auto-clear on results) */
            if (key >= 32 && key < 256 && s.query_len < (int)sizeof(s.query) - 2) {
                if (s.mode == MODE_RESULTS) {
                    s.mode = MODE_SEARCH;  /* switch to search mode, keep query */
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

        /* ── album action picker ── */
        case MODE_ALBUM_ACTION: {
            if (key == KEY_ESC) {
                s.mode = MODE_RESULTS; s.dirty = 1; break;
            }
            if ((key == KEY_UP || key == 'k') && s.album_action_cursor > 0) {
                s.album_action_cursor--; s.dirty = 1; break;
            }
            if ((key == KEY_DOWN || key == 'j') && s.album_action_cursor < 1) {
                s.album_action_cursor++; s.dirty = 1; break;
            }
            if (key == '1') { s.album_action_cursor = 0; s.dirty = 1; break; }
            if (key == '2') { s.album_action_cursor = 1; s.dirty = 1; break; }
            if (key == KEY_ENTER) {
                int action = s.album_action_cursor;
                int cursor = s.cursor;
                s.mode  = MODE_RESULTS;
                s.dirty = 1;
                sqt_mutex_unlock(&s.lock);
                if (action == 0) start_album_download(&s, cursor);
                else             start_album_track_browse(&s, cursor);
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
    if (was_running)
        sqt_thread_join(s.bg_thread);

    if (s.fb) free(s.fb);
    sqt_mutex_destroy(&s.lock);
    return 0;
}
