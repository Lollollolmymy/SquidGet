#include "squidget.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <ctype.h>
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


static void build_child_path(const char *parent, const char *child, char *out, size_t outsz) {
    char safe_child[256];
    sqt_sanitise(child && child[0] ? child : "Untitled", safe_child, sizeof safe_child);
    int written = snprintf(out, outsz, "%s" SQT_SEP "%s", parent, safe_child);
    if (written < 0 || (size_t)written >= outsz) {
        snprintf(out, outsz, "%s" SQT_SEP "Untitled", parent);
    }
}

static int env_int_clamped_main(const char *name, int defv, int minv, int maxv) {
    const char *v = getenv(name);
    if (!v || !v[0]) return defv;
    char *end = NULL;
    long n = strtol(v, &end, 10);
    if (end == v) return defv;
    if (n < minv) n = minv;
    if (n > maxv) n = maxv;
    return (int)n;
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

static const char *quality_from_arg(const char *arg) {
    if (!arg || !arg[0]) return QUALITY_LABELS[0];
    if (strcmp(arg, "1") == 0 || strcmp(arg, "hires") == 0 ||
        strcmp(arg, "hi-res") == 0 || strcmp(arg, "hi-res-lossless") == 0)
        return QUALITY_LABELS[0];
    if (strcmp(arg, "2") == 0 || strcmp(arg, "lossless") == 0)
        return QUALITY_LABELS[1];
    if (strcmp(arg, "3") == 0 || strcmp(arg, "high") == 0 || strcmp(arg, "320") == 0)
        return QUALITY_LABELS[2];
    if (strcmp(arg, "4") == 0 || strcmp(arg, "low") == 0 || strcmp(arg, "96") == 0)
        return QUALITY_LABELS[3];
    if (strcmp(arg, "5") == 0 || strcmp(arg, "atmos") == 0 || strcmp(arg, "dolby-atmos") == 0)
        return QUALITY_LABELS[4];
    return NULL;
}

static void trim_line(char *s) {
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

static void track_query(const Track *t, char *out, size_t outsz) {
    if (t->artist[0])
        snprintf(out, outsz, "%s %s", t->artist, t->title);
    else
        snprintf(out, outsz, "%s", t->title);
}

static int resolve_download_track(const Track *in, Track *out) {
    if (!in || !out) return 0;
    *out = *in;
    if (out->id[0]) return 1;

    char q[512];
    track_query(in, q, sizeof(q));
    Track found[1];
    memset(found, 0, sizeof(found));
    if (api_search_tracks(q, found, 1) <= 0 || !found[0].id[0]) return 0;
    *out = found[0];
    return out->id[0] != '\0';
}

static int run_playlist_cli(const char *playlist_path, const char *quality,
                            const char *out_dir) {
    FILE *f = fopen(playlist_path, "r");
    if (!f) {
        fprintf(stderr, "squidget: could not open playlist: %s\n", playlist_path);
        return 1;
    }

    mkdir_p(out_dir);
    http_init();

    char line[512];
    int total = 0, done = 0, failed = 0;
    while (fgets(line, sizeof line, f)) {
        trim_line(line);
        if (!line[0] || line[0] == '#') continue;
        total++;

        Track results[1];
        memset(results, 0, sizeof results);
        printf("[%d] searching: %s\n", total, line);
        int n = api_search_tracks(line, results, 1);
        if (n <= 0 || !results[0].id[0]) {
            printf("    not found\n");
            failed++;
            continue;
        }

        printf("    downloading: %s - %s (%s)\n",
               results[0].artist, results[0].title, quality);
        if (download_track(&results[0], quality, out_dir, NULL, NULL, NULL) == 0) {
            done++;
            printf("    done\n");
        } else {
            failed++;
            printf("    failed\n");
        }
    }

    fclose(f);
    http_cleanup();
    printf("playlist complete: %d/%d downloaded", done, total);
    if (failed > 0) printf(" (%d failed)", failed);
    printf("\n");
    return failed > 0 ? 1 : 0;
}

static void print_usage(void) {
    printf("usage:\n");
    printf("  squidget\n");
    printf("  squidget playlist <file> [quality] [out_dir]\n\n");
    printf("qualities: hi-res, lossless, high, low, atmos\n");
}

typedef struct {
    Track      *tracks;
    int         n;
    char        out_path[512];
    char        cover_path[1024];
    char        quality[64];
    char        label[32];
    AppState   *s;
    int         next;
    int         done;
    int         failed;
    int         resolve_missing;
    sqt_mutex_t mu;
} DownloadPool;

typedef struct {
    DownloadPool *pool;
    int idx;
} BatchProgressCtx;

static void batch_progress_cb(const char *msg, void *ud) {
    BatchProgressCtx *ctx = ud;
    if (!ctx || !ctx->pool || !ctx->pool->s || !msg) return;
    DownloadPool *pool = ctx->pool;

    sqt_mutex_lock(&pool->mu);
    int finished = pool->done + pool->failed;
    sqt_mutex_unlock(&pool->mu);

    const char *title = "track";
    if (ctx->idx >= 0 && ctx->idx < pool->n && pool->tracks[ctx->idx].title[0])
        title = pool->tracks[ctx->idx].title;
    int cooldown = api_qobuz_cooldown_remaining_ms();

    sqt_mutex_lock(&pool->s->lock);
    if (cooldown > 500) {
        snprintf(pool->s->status, sizeof(pool->s->status),
                 "downloading %.16s… %d/%d | %.40s: %.80s | cooldown %.1fs",
                 pool->label[0] ? pool->label : "batch", finished, pool->n,
                 title, msg, (double)cooldown / 1000.0);
    } else {
        snprintf(pool->s->status, sizeof(pool->s->status),
                 "downloading %.16s… %d/%d | %.40s: %.100s",
                 pool->label[0] ? pool->label : "batch", finished, pool->n,
                 title, msg);
    }
    pool->s->dirty = 1;
    sqt_mutex_unlock(&pool->s->lock);
}

static SQT_THREAD_FN sqt_download_pool_worker(void *arg) {
    DownloadPool *pool = arg;
    while (1) {
        sqt_mutex_lock(&pool->mu);
        int idx = pool->next;
        if (idx >= pool->n) { sqt_mutex_unlock(&pool->mu); break; }
        pool->next++;
        sqt_mutex_unlock(&pool->mu);

        Track work = pool->tracks[idx];
        int ok = 1;
        if (pool->resolve_missing) {
            ok = resolve_download_track(&pool->tracks[idx], &work);
        }
        if (ok) {
            BatchProgressCtx pctx = { pool, idx };
            ok = (download_track(&work, pool->quality,
                                 pool->out_path,
                                 pool->cover_path[0] ? pool->cover_path : NULL,
                                 batch_progress_cb, &pctx) == 0);
        }

        sqt_mutex_lock(&pool->mu);
        if (ok) pool->done++; else pool->failed++;
        int finished = pool->done + pool->failed;

        sqt_mutex_lock(&pool->s->lock);
        {
            int cooldown = api_qobuz_cooldown_remaining_ms();
            if (cooldown > 500)
                snprintf(pool->s->status, sizeof(pool->s->status),
                         "downloading %s… %d/%d | cooldown %.1fs",
                         pool->label[0] ? pool->label : "batch", finished, pool->n,
                         (double)cooldown / 1000.0);
            else
                snprintf(pool->s->status, sizeof(pool->s->status),
                         "downloading %s… %d/%d",
                         pool->label[0] ? pool->label : "batch", finished, pool->n);
        }
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
            if (n == 0 && api_last_error()[0])
                snprintf(s->status, sizeof(s->status), "error: %s", api_last_error());
            else
                snprintf(s->status, sizeof(s->status), "%d track%s", n, n == 1 ? "" : "s");
            s->dirty = 1;
            sqt_mutex_unlock(&s->lock);
            free(tmp);
            break;
        }

        case TASK_DOWNLOAD: {
            Track trk = t.track;
            if (!resolve_download_track(&t.track, &trk)) {
                sqt_mutex_lock(&s->lock);
                snprintf(s->status, sizeof(s->status), "error: could not match playlist track");
                s->dirty = 1;
                sqt_mutex_unlock(&s->lock);
                break;
            }
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
            if (n == 0 && api_last_error()[0])
                snprintf(s->status, sizeof(s->status), "error: %s", api_last_error());
            else
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
            if (n == 0 && api_last_error()[0])
                snprintf(s->status, sizeof(s->status), "error: %s", api_last_error());
            else
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
            build_child_path(s->out_dir, t.album_name, album_path, sizeof album_path);
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

            DownloadPool pool;
            memset(&pool, 0, sizeof pool);
            pool.tracks = tracks;
            pool.n      = n;
            pool.s      = s;
            snprintf(pool.out_path, sizeof pool.out_path, "%s", album_path);
            snprintf(pool.cover_path, sizeof pool.cover_path, "%s", album_cover);
            snprintf(pool.quality,    sizeof pool.quality,    "%s", t.quality);
            snprintf(pool.label,      sizeof pool.label,      "%s", "album");
            pool.resolve_missing = 0;
            sqt_mutex_init(&pool.mu);

#define ALBUM_THREADS_MAX 4
            int max_threads = env_int_clamped_main("SQUIDGET_ALBUM_THREADS", 2, 1, ALBUM_THREADS_MAX);
            int nthreads = n < max_threads ? n : max_threads;
            sqt_thread_t workers[ALBUM_THREADS_MAX];
            for (int ti = 0; ti < nthreads; ti++)
                sqt_thread_create(&workers[ti], sqt_download_pool_worker, &pool);
            for (int ti = 0; ti < nthreads; ti++)
                sqt_thread_join(workers[ti]);
#undef ALBUM_THREADS_MAX

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

        case TASK_PLAYLIST_FETCH: {
            Playlist pl;
            Track *tracks = calloc(SQT_MAX_RESULTS, sizeof(Track));
            if (!tracks) {
                sqt_mutex_lock(&s->lock);
                snprintf(s->status, sizeof(s->status), "error: out of memory");
                s->dirty = 1;
                sqt_mutex_unlock(&s->lock);
                break;
            }
            int n = api_fetch_playlist(t.playlist_url, &pl, tracks, SQT_MAX_RESULTS);
            sqt_mutex_lock(&s->lock);
            if (n <= 0) {
                snprintf(s->status, sizeof(s->status), "error: could not read playlist");
                s->dirty = 1;
                sqt_mutex_unlock(&s->lock);
                free(tracks);
                break;
            }

            int slot = -1;
            for (int i = 0; i < s->playlist_count; i++) {
                if (strcmp(s->playlists[i].url, pl.url) == 0) { slot = i; break; }
            }
            if (slot < 0 && s->playlist_count < SQT_MAX_PLAYLISTS)
                slot = s->playlist_count++;
            if (slot >= 0) {
                s->playlists[slot] = pl;
                memcpy(s->tracks, tracks, (size_t)n * sizeof(Track));
                s->track_count = n;
                snprintf(s->current_playlist_url, sizeof(s->current_playlist_url), "%s", pl.url);
                s->current_playlist_index = slot;
                s->album_action_cursor = 0;
                s->cursor = slot;
                s->scroll = 0;
                s->mode = MODE_ALBUM_ACTION;
                config_save_playlists(s);
                snprintf(s->status, sizeof(s->status), "%d playlist tracks", n);
            } else {
                snprintf(s->status, sizeof(s->status), "error: playlist history full");
            }
            s->dirty = 1;
            sqt_mutex_unlock(&s->lock);
            free(tracks);
            break;
        }

        case TASK_PLAYLIST_DOWNLOAD: {
            Playlist pl;
            Track *tracks = calloc(SQT_MAX_RESULTS, sizeof(Track));
            if (!tracks) {
                sqt_mutex_lock(&s->lock);
                snprintf(s->status, sizeof(s->status), "error: out of memory");
                s->dirty = 1;
                sqt_mutex_unlock(&s->lock);
                break;
            }
            int n = api_fetch_playlist(t.playlist_url, &pl, tracks, SQT_MAX_RESULTS);
            if (n <= 0) {
                sqt_mutex_lock(&s->lock);
                snprintf(s->status, sizeof(s->status), "error: could not read playlist");
                s->dirty = 1;
                sqt_mutex_unlock(&s->lock);
                free(tracks);
                break;
            }
            char folder[512];
            build_child_path(s->out_dir, pl.title[0] ? pl.title : "Playlist", folder, sizeof folder);
            mkdir_p(folder);
            DownloadPool pool;
            memset(&pool, 0, sizeof pool);
            pool.tracks = tracks;
            pool.n      = n;
            pool.s      = s;
            snprintf(pool.out_path, sizeof pool.out_path, "%s", folder);
            snprintf(pool.quality,  sizeof pool.quality,  "%s", t.quality);
            snprintf(pool.label,    sizeof pool.label,    "%s", "playlist");
            pool.resolve_missing = 1;
            sqt_mutex_init(&pool.mu);

#define PLAYLIST_THREADS_MAX 4
            int max_threads = env_int_clamped_main("SQUIDGET_PLAYLIST_THREADS", 2, 1, PLAYLIST_THREADS_MAX);
            int nthreads = n < max_threads ? n : max_threads;
            sqt_thread_t workers[PLAYLIST_THREADS_MAX];
            for (int ti = 0; ti < nthreads; ti++)
                sqt_thread_create(&workers[ti], sqt_download_pool_worker, &pool);
            for (int ti = 0; ti < nthreads; ti++)
                sqt_thread_join(workers[ti]);
#undef PLAYLIST_THREADS_MAX
            sqt_mutex_destroy(&pool.mu);

            sqt_mutex_lock(&s->lock);
            snprintf(s->status, sizeof(s->status), "playlist done! %d/%d%s",
                     pool.done, n, pool.failed ? " (some errors)" : "");
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

static void open_quality_picker(AppState *s, int cursor) {
    s->prev_mode = s->mode;
    s->mode = MODE_QUALITY;
    s->qual_cursor = 0;
    s->cursor = cursor;
    snprintf(s->status, sizeof(s->status), "choose quality; returned file type will be saved as-is");
    s->dirty = 1;
}

static void start_download(AppState *s, int cursor, int qual_idx) {
    if (cursor < 0 || cursor >= s->track_count) return;
    if (qual_idx < 0 || qual_idx >= QUALITY_COUNT) return;
    /* Availability preflight is intentionally disabled. The selected tier is sent
       to the resolver, and the downloader accepts whatever file type comes back. */
    SQTTask t;
    memset(&t, 0, sizeof t);
    t.type = TASK_DOWNLOAD;
    snprintf(t.track_id, sizeof(t.track_id), "%s", s->tracks[cursor].id);
    snprintf(t.quality,  sizeof(t.quality),  "%s", QUALITY_LABELS[qual_idx]);
    t.track = s->tracks[cursor];
    snprintf(s->status, sizeof(s->status), "queued download");
    s->dirty = 1;
    push_task(s, &t);
}

static void start_playlist_fetch(AppState *s) {
    if (!*s->query) return;
    SQTTask t;
    memset(&t, 0, sizeof t);
    t.type = TASK_PLAYLIST_FETCH;
    snprintf(t.playlist_url, sizeof(t.playlist_url), "%s", s->query);
    snprintf(s->status, sizeof(s->status), "fetching playlist…");
    s->dirty = 1;
    push_task(s, &t);
}

static void start_playlist_download(AppState *s, const char *url) {
    if (!url || !url[0]) return;
    SQTTask t;
    memset(&t, 0, sizeof t);
    t.type = TASK_PLAYLIST_DOWNLOAD;
    snprintf(t.playlist_url, sizeof(t.playlist_url), "%s", url);
    snprintf(t.quality, sizeof(t.quality), "%s",
             s->default_quality >= 0 ? QUALITY_LABELS[s->default_quality] : QUALITY_LABELS[0]);
    snprintf(s->status, sizeof(s->status), "queued playlist download");
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

int main(int argc, char **argv) {
    if (argc > 1) {
        if (strcmp(argv[1], "playlist") == 0) {
            if (argc < 3 || argc > 5) {
                print_usage();
                return 1;
            }
            const char *quality = argc >= 4 ? quality_from_arg(argv[3]) : QUALITY_LABELS[0];
            if (!quality) {
                fprintf(stderr, "squidget: unknown quality: %s\n", argv[3]);
                print_usage();
                return 1;
            }
            const char *out_dir = argc >= 5 ? argv[4] : "SquidGet Playlist";
            return run_playlist_cli(argv[2], quality, out_dir);
        }
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            print_usage();
            return 0;
        }
        print_usage();
        return 1;
    }

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
    config_load_playlists(&s);

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
            /* TAB: cycle song, album, and playlist search */
            if (key == KEY_TAB) {
                s.search_type = (s.search_type == SEARCH_SONGS) ? SEARCH_ALBUMS :
                                (s.search_type == SEARCH_ALBUMS) ? SEARCH_PLAYLISTS : SEARCH_SONGS;
                s.track_count = 0;  /* clear counts first */
                s.album_count = 0;
                s.mode        = s.search_type == SEARCH_PLAYLISTS && s.playlist_count > 0 ? MODE_RESULTS : MODE_SEARCH;
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
                        {
                            int cursor = s.cursor;
                            s.dirty = 1;
                            sqt_mutex_unlock(&s.lock);
                            open_quality_picker(&s, cursor);
                            continue;
                        }
                    }
                    if (s.search_type == SEARCH_ALBUMS &&
                        s.cursor >= 0 && s.cursor < s.album_count) {
                        s.mode = MODE_ALBUM_ACTION;
                        s.album_action_cursor = 0;
                        s.dirty = 1;
                        break;
                    }
                if (s.search_type == SEARCH_PLAYLISTS &&
                    s.cursor >= 0 && s.cursor < s.playlist_count) {
                    Playlist *pl = &s.playlists[s.cursor];
                    s.current_playlist_index = s.cursor;
                    snprintf(s.query, sizeof(s.query), "%s", pl->url);
                    s.query_len = (int)strlen(s.query);
                    sqt_mutex_unlock(&s.lock);
                        start_playlist_fetch(&s);
                        continue;
                    }
                }
                sqt_mutex_unlock(&s.lock);
                if (s.search_type == SEARCH_ALBUMS) do_album_search(&s);
                else if (s.search_type == SEARCH_PLAYLISTS) start_playlist_fetch(&s);
                else do_search(&s);
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
                int list_len = (s.search_type == SEARCH_PLAYLISTS) ? s.playlist_count :
                               (s.search_type == SEARCH_ALBUMS) ? s.album_count : s.track_count;
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
                int list_len = (s.search_type == SEARCH_PLAYLISTS) ? s.playlist_count :
                               (s.search_type == SEARCH_ALBUMS) ? s.album_count : s.track_count;
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
                int list_len = (s.search_type == SEARCH_PLAYLISTS) ? s.playlist_count :
                               (s.search_type == SEARCH_ALBUMS) ? s.album_count : s.track_count;
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

        case MODE_PLAYLIST_TRACKS: {
            if (key == KEY_ESC) {
                s.mode = MODE_RESULTS;
                s.cursor = s.scroll = 0;
                s.dirty = 1;
                break;
            }
            if (key == 'a' || key == 'A') {
                char url[SQT_URL_SZ] = {0};
                snprintf(url, sizeof(url), "%s", s.current_playlist_url);
                sqt_mutex_unlock(&s.lock);
                start_playlist_download(&s, url);
                continue;
            }
            if (key == KEY_UP && s.cursor > 0) {
                s.cursor--;
                if (s.cursor < s.scroll) s.scroll = s.cursor;
                s.dirty = 1; break;
            }
            if (key == KEY_DOWN && s.cursor < s.track_count - 1) {
                s.cursor++;
                int vis = s.rows - HEADER_ROWS - FOOTER_ROWS - 1;
                if (s.cursor >= s.scroll + vis) s.scroll = s.cursor - vis + 1;
                s.dirty = 1; break;
            }
            if (key == KEY_ENTER && s.cursor >= 0 && s.cursor < s.track_count) {
                int qi = s.default_quality >= 0 ? s.default_quality : 0;
                int cursor = s.cursor;
                sqt_mutex_unlock(&s.lock);
                start_download(&s, cursor, qi);
                continue;
            }
            break;
        }

        /* ── quality picker ── */
        case MODE_QUALITY: {
            if (key == KEY_ESC) {
                s.mode = s.prev_mode == MODE_ALBUM_TRACKS ? MODE_ALBUM_TRACKS : MODE_RESULTS;
                s.dirty = 1; break;
            }
            if (key == KEY_UP || key == 'k') {
                s.qual_cursor = (s.qual_cursor + QUALITY_COUNT - 1) % QUALITY_COUNT;
                s.dirty = 1; break;
            }
            if (key == KEY_DOWN || key == 'j') {
                s.qual_cursor = (s.qual_cursor + 1) % QUALITY_COUNT;
                s.dirty = 1; break;
            }
            if (key >= '1' && key <= '0' + QUALITY_COUNT) {
                s.qual_cursor = key - '1';
                s.dirty = 1;
            }
            if (key == 's' || key == 'S') {
                s.default_quality = s.qual_cursor;
                config_save(&s);
                snprintf(s.status, sizeof(s.status), "saved preferred quality: %s", QUALITY_LABELS[s.default_quality]);
                s.dirty = 1;
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
                if (s.search_type == SEARCH_PLAYLISTS) {
                    if (action == 0) start_playlist_download(&s, s.current_playlist_url);
                    else {
                        sqt_mutex_lock(&s.lock);
                        s.mode = MODE_PLAYLIST_TRACKS;
                        s.cursor = s.scroll = 0;
                        s.dirty = 1;
                        sqt_mutex_unlock(&s.lock);
                    }
                } else {
                    if (action == 0) start_album_download(&s, cursor);
                    else             start_album_track_browse(&s, cursor);
                }
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
                    {
                        int cursor = s.cursor;
                        s.dirty = 1;
                        sqt_mutex_unlock(&s.lock);
                        open_quality_picker(&s, cursor);
                        continue;
                    }
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
