#include "squidget.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#ifndef _WIN32
#include <sys/stat.h>  /* mkdir */
#endif

/* ── Windows structured-exception crash handler ── */
#ifdef _WIN32
static LONG WINAPI sqt_crash_handler(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    void *addr = ep->ExceptionRecord->ExceptionAddress;
    const char *name = "UNKNOWN";
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:         name = "ACCESS_VIOLATION";       break;
        case EXCEPTION_STACK_OVERFLOW:           name = "STACK_OVERFLOW";         break;
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    name = "ARRAY_BOUNDS_EXCEEDED";  break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO:       name = "INT_DIVIDE_BY_ZERO";     break;
        case EXCEPTION_INT_OVERFLOW:             name = "INT_OVERFLOW";           break;
        case EXCEPTION_ILLEGAL_INSTRUCTION:      name = "ILLEGAL_INSTRUCTION";    break;
        case EXCEPTION_PRIV_INSTRUCTION:         name = "PRIVILEGED_INSTRUCTION"; break;
        case EXCEPTION_IN_PAGE_ERROR:            name = "IN_PAGE_ERROR";          break;
    }
    (void)name; (void)addr;
    /* Restore terminal before letting the OS handle the crash */
    HANDLE ho = GetStdHandle(STD_OUTPUT_HANDLE);
    /* show cursor, reset attrs, leave alt screen */
    const char *cleanup = "\033[?25h\033[0m\033[?1049l\n";
    DWORD written;
    WriteConsoleA(ho, cleanup, (DWORD)strlen(cleanup), &written, NULL);
    return EXCEPTION_CONTINUE_SEARCH;
}
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

static void dl_progress_cb(const char *msg, void *ud) {
    AppState *s = ud;
    sqt_mutex_lock(&s->lock);
    snprintf(s->status, sizeof(s->status), "%s", msg);
    s->dirty = 1;
    sqt_mutex_unlock(&s->lock);
}

typedef struct {
    Track      *tracks;
    int         n;
    char        album_path[512];
    char        cover_path[1024];
    char        quality[64];
    AppState   *s;
    int         next;
    int         done;
    int         failed;
    sqt_mutex_t mu;
} AlbumPool;

static SQT_THREAD_FN sqt_album_pool_worker(void *arg) {
    AlbumPool *pool = arg;
    while (1) {
        sqt_mutex_lock(&pool->mu);
        int idx = pool->next;
        if (idx >= pool->n) { sqt_mutex_unlock(&pool->mu); break; }
        pool->next++;
        sqt_mutex_unlock(&pool->mu);

        int ok = download_track(&pool->tracks[idx], pool->quality,
                                 pool->album_path,
                                 pool->cover_path[0] ? pool->cover_path : NULL,
                                 NULL, NULL);

        sqt_mutex_lock(&pool->mu);
        if (ok == 0) pool->done++; else pool->failed++;
        
        sqt_mutex_lock(&pool->s->lock);
        snprintf(pool->s->status, sizeof(pool->s->status),
                 "downloading album… %d/%d", pool->done, pool->n);
        pool->s->dirty = 1;
        sqt_mutex_unlock(&pool->s->lock);
        sqt_mutex_unlock(&pool->mu);
    }
#ifndef _WIN32
    return NULL;
#else
    return 0;
#endif
}

static SQT_THREAD_FN bg_worker(void *arg) {
    AppState *s = arg;
    while (1) {
        SQTTask t;
        sqt_mutex_lock(&s->lock);
        if (s->q_count == 0) {
            s->bg_running = 0;
            sqt_mutex_unlock(&s->lock);
            break;
        }
        t = s->queue[s->q_head];
        s->q_head = (s->q_head + 1) % SQT_MAX_QUEUE;
        s->q_count--;
        sqt_mutex_unlock(&s->lock);

        switch (t.type) {

        case TASK_SEARCH: {
            Track *tmp = calloc(SQT_MAX_RESULTS, sizeof(Track));
            if (!tmp) {
                sqt_mutex_lock(&s->lock);
                snprintf(s->status, sizeof(s->status), "error: out of memory");
                s->dirty = 1;
                sqt_mutex_unlock(&s->lock);
                break;
            }
            int n = api_search_tracks(t.query, tmp, SQT_MAX_RESULTS);
            sqt_mutex_lock(&s->lock);
            memcpy(s->tracks, tmp, (size_t)n * sizeof(Track));
            s->track_count = n;
            s->cursor = s->scroll = 0;
            s->mode   = MODE_RESULTS;
            snprintf(s->status, sizeof(s->status), "%d track%s", n, n == 1 ? "" : "s");
            s->dirty = 1;
            sqt_mutex_unlock(&s->lock);
            free(tmp);
            break;
        }

        case TASK_DOWNLOAD: {
            sqt_mutex_lock(&s->lock);
            Track trk = {0};
            for (int i = 0; i < s->track_count; i++) {
                if (strcmp(s->tracks[i].id, t.track_id) == 0) { trk = s->tracks[i]; break; }
            }
            sqt_mutex_unlock(&s->lock);
            download_track(&trk, t.quality, s->out_dir, NULL, dl_progress_cb, s);
            break;
        }

        case TASK_ALBUM_SEARCH: {
            Album *tmp = calloc(SQT_MAX_RESULTS, sizeof(Album));
            if (!tmp) {
                sqt_mutex_lock(&s->lock);
                snprintf(s->status, sizeof(s->status), "error: out of memory");
                s->dirty = 1;
                sqt_mutex_unlock(&s->lock);
                break;
            }
            int n = api_search_albums(t.query, tmp, SQT_MAX_RESULTS);
            sqt_mutex_lock(&s->lock);
            memcpy(s->albums, tmp, (size_t)n * sizeof(Album));
            s->album_count = n;
            s->cursor = s->scroll = 0;
            s->mode   = MODE_RESULTS;
            snprintf(s->status, sizeof(s->status), "%d album%s", n, n == 1 ? "" : "s");
            s->dirty = 1;
            sqt_mutex_unlock(&s->lock);
            free(tmp);
            break;
        }

        case TASK_ALBUM_TRACKS: {
            Track *tmp = calloc(SQT_MAX_RESULTS, sizeof(Track));
            if (!tmp) {
                sqt_mutex_lock(&s->lock);
                snprintf(s->status, sizeof(s->status), "error: out of memory");
                s->dirty = 1;
                sqt_mutex_unlock(&s->lock);
                break;
            }
            int n = api_get_album_tracks(t.album_id, tmp, SQT_MAX_RESULTS);
            sqt_mutex_lock(&s->lock);
            memcpy(s->tracks, tmp, (size_t)n * sizeof(Track));
            s->track_count = n;
            s->cursor = s->scroll = 0;
            s->mode        = MODE_ALBUM_TRACKS;
            snprintf(s->status, sizeof(s->status), "%d track%s", n, n == 1 ? "" : "s");
            s->dirty = 1;
            sqt_mutex_unlock(&s->lock);
            free(tmp);
            break;
        }

        case TASK_ALBUM_DOWNLOAD: {
            Track *tracks = calloc(SQT_MAX_RESULTS, sizeof(Track));
            if (!tracks) {
                sqt_mutex_lock(&s->lock);
                snprintf(s->status, sizeof(s->status), "error: out of memory");
                s->dirty = 1;
                sqt_mutex_unlock(&s->lock);
                break;
            }
            int n = api_get_album_tracks(t.album_id, tracks, SQT_MAX_RESULTS);
            if (n <= 0) {
                sqt_mutex_lock(&s->lock);
                snprintf(s->status, sizeof(s->status), "error: could not fetch album tracks");
                s->dirty = 1;
                sqt_mutex_unlock(&s->lock);
                free(tracks);
                break;
            }

            char album_path[512];
            snprintf(album_path, sizeof(album_path), "%s" SQT_SEP "%s", s->out_dir, t.album_name);
            mkdir_p(album_path);

            char album_cover[1024] = {0};
            {
                Track rich = tracks[0];
                if (api_get_track_info(tracks[0].id, &rich) != 0 && rich.cover[0]) {
                    snprintf(album_cover, sizeof album_cover,
                             "%s" SQT_SEP ".sqt_cover_album_%s.jpg", album_path, t.album_id);
                    if (http_get_file(rich.cover, album_cover, NULL, NULL) <= 0) {
                        album_cover[0] = '\0';
                    }
                }
            }

            AlbumPool pool;
            memset(&pool, 0, sizeof pool);
            pool.tracks = tracks;
            pool.n      = n;
            pool.s      = s;
            snprintf(pool.album_path, sizeof pool.album_path, "%s", album_path);
            snprintf(pool.cover_path, sizeof pool.cover_path, "%s", album_cover);
            snprintf(pool.quality,    sizeof pool.quality,    "%s", t.quality);
            sqt_mutex_init(&pool.mu);

#define ALBUM_THREADS 4
            int nthreads = n < ALBUM_THREADS ? n : ALBUM_THREADS;
            sqt_thread_t workers[ALBUM_THREADS];
            for (int ti = 0; ti < nthreads; ti++)
                sqt_thread_create(&workers[ti], sqt_album_pool_worker, &pool);
            for (int ti = 0; ti < nthreads; ti++)
                sqt_thread_join(workers[ti]);
#undef ALBUM_THREADS

            sqt_mutex_destroy(&pool.mu);
            if (album_cover[0]) remove(album_cover);
            sqt_mutex_lock(&s->lock);
            snprintf(s->status, sizeof(s->status),
                     "album done! %d/%d tracks%s",
                     pool.done, n, pool.failed > 0 ? " (some errors)" : "");
            s->dirty = 1;
            sqt_mutex_unlock(&s->lock);
            free(tracks);
            break;
        }

        }
    }
#ifndef _WIN32
    return NULL;
#else
    return 0;
#endif
}

static void push_task(AppState *s, const SQTTask *t) {
    sqt_mutex_lock(&s->lock);

    if (s->q_count >= SQT_MAX_QUEUE) {
        snprintf(s->status, sizeof(s->status), "error: queue full");
        s->dirty = 1;
        sqt_mutex_unlock(&s->lock);
        return;
    }

    s->queue[s->q_tail] = *t;
    s->q_tail = (s->q_tail + 1) % SQT_MAX_QUEUE;
    s->q_count++;

    if (!s->bg_running) {
        s->bg_running = 1;
        if (sqt_thread_create(&s->bg_thread, bg_worker, s) != 0) {
            s->bg_running = 0;
            snprintf(s->status, sizeof(s->status), "error: failed to start thread");
            s->dirty = 1;
        }
    }
    sqt_mutex_unlock(&s->lock);
}

static void do_search(AppState *s) {
    if (!*s->query) return;
    SQTTask t;
    memset(&t, 0, sizeof t);
    t.type = TASK_SEARCH;
    snprintf(t.query, sizeof(t.query), "%s", s->query);
    snprintf(s->status, sizeof(s->status), "searching…");
    s->dirty = 1;
    push_task(s, &t);
}

static void do_album_search(AppState *s) {
    if (!*s->query) return;
    SQTTask t;
    memset(&t, 0, sizeof t);
    t.type = TASK_ALBUM_SEARCH;
    snprintf(t.query, sizeof(t.query), "%s", s->query);
    snprintf(s->status, sizeof(s->status), "searching albums…");
    s->dirty = 1;
    push_task(s, &t);
}

static void start_album_track_browse(AppState *s, int cursor) {
    if (cursor < 0 || cursor >= s->album_count) return;
    SQTTask t;
    memset(&t, 0, sizeof t);
    t.type = TASK_ALBUM_TRACKS;
    snprintf(t.album_id, sizeof(t.album_id), "%s", s->albums[cursor].id);
    snprintf(s->status, sizeof(s->status), "fetching tracks…");
    s->dirty = 1;
    push_task(s, &t);
}

static void start_album_download(AppState *s, int cursor) {
    if (cursor < 0 || cursor >= s->album_count) return;
    SQTTask t;
    memset(&t, 0, sizeof t);
    t.type = TASK_ALBUM_DOWNLOAD;
    snprintf(t.album_id, sizeof(t.album_id), "%s", s->albums[cursor].id);
    sqt_sanitise(s->albums[cursor].title, t.album_name, sizeof(t.album_name));
    snprintf(t.quality,  sizeof(t.quality),  "%s", QUALITY_LABELS[0]);
    snprintf(s->status, sizeof(s->status), "queued album download");
    s->dirty = 1;
    push_task(s, &t);
}

static void start_download(AppState *s, int cursor, int qual_idx) {
    if (cursor < 0 || cursor >= s->track_count) return;
    SQTTask t;
    memset(&t, 0, sizeof t);
    t.type = TASK_DOWNLOAD;
    snprintf(t.track_id, sizeof(t.track_id), "%s", s->tracks[cursor].id);
    snprintf(t.quality,  sizeof(t.quality),  "%s", QUALITY_LABELS[qual_idx]);
    snprintf(s->status, sizeof(s->status), "queued download");
    s->dirty = 1;
    push_task(s, &t);
}

#ifndef _WIN32
#  include <unistd.h>
static volatile sig_atomic_t g_resize_pending = 0;
static void on_sigwinch(int _) { (void)_; g_resize_pending = 1; }
static void on_fatal_sig(int _) {
    (void)_;
    /* async-safe cleanup: use write() not fputs() */
    static const char cleanup[] = "\033[?25h\033[0m\033[?1049l\n";
    write(STDOUT_FILENO, cleanup, sizeof(cleanup) - 1);
    _exit(1);
}
#endif

int main(void) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    /* register crash handler BEFORE anything else */
    SetUnhandledExceptionFilter(sqt_crash_handler);
#endif

    AppState s;
    memset(&s, 0, sizeof(s));
    sqt_mutex_init(&s.lock);

    // check if configured
    if (config_load(&s)) {
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
            if (key == '?') {
                s.prev_mode = s.mode;
                s.mode = MODE_HELP;
                s.dirty = 1;
                break;
            }
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
                        if (s.default_quality >= 0) {
                            int qi     = s.default_quality;
                            int cursor = s.cursor;
                            s.dirty = 1;
                            sqt_mutex_unlock(&s.lock);
                            start_download(&s, cursor, qi);
                            continue;
                        }
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
            if (key == KEY_CTRL_U) {
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
                s.mode = s.prev_mode == MODE_ALBUM_TRACKS ? MODE_ALBUM_TRACKS : MODE_RESULTS;
                s.dirty = 1; break;
            }
            if ((key == KEY_UP || key == 'k') && s.qual_cursor > 0) {
                s.qual_cursor--; s.dirty = 1; break;
            }
            if ((key == KEY_DOWN || key == 'j') && s.qual_cursor < QUALITY_COUNT - 1) {
                s.qual_cursor++; s.dirty = 1; break;
            }
            if (key >= '1' && key <= '0' + QUALITY_COUNT)
                s.qual_cursor = key - '1';
            if (key == 's' || key == 'S') {
                s.default_quality = s.qual_cursor;
                config_save(&s);
                snprintf(s.status, sizeof(s.status), "saved preferred quality: %s", QUALITY_LABELS[s.default_quality]);
                s.dirty = 1;
                /* download now too */
                key = KEY_ENTER;
            }
            if (key == KEY_ENTER || (key >= '1' && key <= '0' + QUALITY_COUNT)) {
                int qi     = s.qual_cursor;
                int cursor = s.cursor;
                s.mode  = s.prev_mode == MODE_ALBUM_TRACKS ? MODE_ALBUM_TRACKS : MODE_RESULTS;
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

        /* ── album track browser ── */
        case MODE_ALBUM_TRACKS: {
            if (key == KEY_ESC) {
                s.mode = MODE_RESULTS; s.cursor = 0; s.scroll = 0; s.dirty = 1; break;
            }
            if (key == '?') {
                s.prev_mode = s.mode;
                s.mode = MODE_HELP;
                s.dirty = 1;
                break;
            }
            if (key == KEY_UP) {
                if (s.cursor > 0) {
                    s.cursor--;
                    if (s.cursor < s.scroll) s.scroll = s.cursor;
                    s.dirty = 1;
                }
                break;
            }
            if (key == KEY_DOWN) {
                if (s.cursor < s.track_count - 1) {
                    s.cursor++;
                    int vis = s.rows - HEADER_ROWS - FOOTER_ROWS - 1;
                    if (s.cursor >= s.scroll + vis) s.scroll = s.cursor - vis + 1;
                    s.dirty = 1;
                }
                break;
            }
            if (key == KEY_HOME) { s.cursor = 0; s.scroll = 0; s.dirty = 1; break; }
            if (key == KEY_END) {
                if (s.track_count > 0) {
                    int vis = s.rows - HEADER_ROWS - FOOTER_ROWS - 1;
                    s.cursor = s.track_count - 1;
                    if (s.cursor >= s.scroll + vis) s.scroll = s.cursor - vis + 1;
                    s.dirty = 1;
                }
                break;
            }
            if (key == KEY_PGUP) {
                int vis = s.rows - HEADER_ROWS - FOOTER_ROWS - 1;
                s.cursor = s.cursor > vis ? s.cursor - vis : 0;
                if (s.cursor < s.scroll) s.scroll = s.cursor;
                s.dirty = 1;
                break;
            }
            if (key == KEY_PGDN) {
                if (s.track_count > 0) {
                    int vis = s.rows - HEADER_ROWS - FOOTER_ROWS - 1;
                    s.cursor += vis;
                    if (s.cursor >= s.track_count) s.cursor = s.track_count - 1;
                    if (s.cursor >= s.scroll + vis) s.scroll = s.cursor - vis + 1;
                    s.dirty = 1;
                }
                break;
            }
            if (key == KEY_ENTER) {
                if (s.cursor >= 0 && s.cursor < s.track_count) {
                    if (s.default_quality >= 0) {
                        int qi     = s.default_quality;
                        int cursor = s.cursor;
                        s.dirty = 1;
                        sqt_mutex_unlock(&s.lock);
                        start_download(&s, cursor, qi);
                        continue;
                    }
                    s.prev_mode = MODE_ALBUM_TRACKS;
                    s.mode = MODE_QUALITY;
                    s.qual_cursor = 0;
                    s.dirty = 1;
                }
                break;
            }
            break;
        }

        /* ── help overlay ── */
        case MODE_HELP: {
            if (key == KEY_ESC || key == '?' || key == KEY_ENTER || key == 'q') {
                s.mode = s.prev_mode;
                s.dirty = 1;
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
                        config_save(&s);
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
                    config_save(&s);
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

        /* only mark dirty if we reached here without an early continue/break
           that already set dirty itself; actual state changes set dirty inline */
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
