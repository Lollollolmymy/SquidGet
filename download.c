#define _POSIX_C_SOURCE 200809L
#include "squidget.h"
#include "tag.h"
#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef _WIN32
#  include <io.h>
#  include <windows.h>
#  define access(p,m) _access(p,m)
#else
#  include <unistd.h>
#  include <sys/stat.h>
#endif

/* cover_url is now a full https:// URL (Qobuz album.image.large) */
static int fetch_cover(const char *cover_url, const char *dest) {
    if (!cover_url || !cover_url[0]) return 0;
    return (http_get_file(cover_url, dest, NULL, NULL) > 0) ? 1 : 0;
}

static int sqt_case_match(const char *s, const char *ref) {
    while (*s && *ref) {
        char c1 = *s;
        char c2 = *ref;
        if (c1 >= 'a' && c1 <= 'z') c1 -= 32;
        if (c2 >= 'a' && c2 <= 'z') c2 -= 32;
        if (c1 != c2) return 0;
        s++; ref++;
    }
    return *ref == '\0';
}

void sqt_sanitise(const char *in, char *out, size_t outsz) {
    size_t j = 0;
    for (const char *p = in; *p && j + 1 < outsz; p++) {
        char c = *p;
        /* illegal characters on windows/posix */
        if (c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"'  || c == '<' || c == '>' || c == '|')
            c = '_';
        /* control characters */
        if ((unsigned char)c < 32) c = '_';
        out[j++] = c;
    }
    out[j] = '\0';

    /* strip trailing dots and spaces (illegal on Windows) */
    while (j > 0 && (out[j-1] == '.' || out[j-1] == ' ')) {
        out[--j] = '\0';
    }

    if (j == 0) {
        snprintf(out, outsz, "unnamed");
        return;
    }

    /* check for windows reserved names (CON, PRN, AUX, NUL, COM1-9, LPT1-9) */
    static const char *res[] = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
    };
    for (size_t i = 0; i < sizeof(res)/sizeof(res[0]); i++) {
        size_t rlen = strlen(res[i]);
        if (sqt_case_match(out, res[i])) {
            if (out[rlen] == '\0' || out[rlen] == '.') {
                /* prepend underscore to bypass reserved name check */
                char tmp[1024];
                snprintf(tmp, sizeof(tmp), "_%s", out);
                snprintf(out, outsz, "%s", tmp);
                break;
            }
        }
    }
}

typedef struct {
    uint64_t start_time;
    uint64_t last_report_time;
    void (*cb)(const char *msg, void *ud);
    void *ud;
} AnalyticsCtx;

static void internal_progress_cb(size_t received, size_t total, void *ud) {
    AnalyticsCtx *ctx = ud;
    uint64_t now = sqt_time_ms();
    if (now - ctx->last_report_time < 250 && received < total && received > 0) return;
    ctx->last_report_time = now;

    double elapsed = (double)(now - ctx->start_time) / 1000.0;
    if (elapsed < 0.01) elapsed = 0.01;
    double speed = (double)received / (1024.0 * 1024.0 * elapsed);

    char msg[128];
    if (total > 0) {
        int pct = (int)((double)received * 100.0 / (double)total);
        double rem = (double)(total - received) / ((double)received / elapsed);
        if (received == 0) rem = 0;
        int rm = (int)rem / 60, rs = (int)rem % 60;
        snprintf(msg, sizeof msg, "downloading… %3d%% (%.1f MB/s) %02d:%02d left", 
                 pct, speed, rm, rs);
    } else {
        snprintf(msg, sizeof msg, "downloading… %.1f MB (%.1f MB/s)", 
                 (double)received / (1024.0 * 1024.0), speed);
    }
    ctx->cb(msg, ctx->ud);
}

static int download_single_url(const char *url, const char *out_path,
                                void (*progress_cb)(const char *msg, void *ud), void *ud) {
    AnalyticsCtx actx = { sqt_time_ms(), 0, progress_cb, ud };
    
    char tmp_path[1200];
    snprintf(tmp_path, sizeof(tmp_path), "%s.sqtmp", out_path);

    if (http_get_file(url, tmp_path, internal_progress_cb, &actx) < 0) {
        remove(tmp_path);
        return -1;
    }

    if (rename(tmp_path, out_path) != 0) {
        remove(tmp_path);
        return -1;
    }

    return 0;
}

/* build sanitised output path, create dir, handle collisions */
static void build_out_path(const Track *t, const char *out_dir,
                            const char *ext, char *final_out, size_t outsz) {
    char artist_s[512], title_s[512];
    sqt_sanitise(t->artist, artist_s, sizeof(artist_s));
    sqt_sanitise(t->title,  title_s,  sizeof(title_s));

#ifdef _WIN32
    CreateDirectoryA(out_dir, NULL);
#else
    mkdir(out_dir, 0755);
#endif

    snprintf(final_out, outsz, "%s" SQT_SEP "%s - %s.%s",
             out_dir, artist_s, title_s, ext);

    if (access(final_out, F_OK) == 0) {
        char base[1032];
        snprintf(base, sizeof(base), "%s" SQT_SEP "%s - %s",
                 out_dir, artist_s, title_s);
        for (int n = 2; n < 1000; n++) {
            snprintf(final_out, outsz, "%s (%d).%s", base, n, ext);
            if (access(final_out, F_OK) != 0) break;
        }
    }
}

/* Returns 1 if the track already has all key metadata fields populated,
   meaning a redundant api_get_track_info call can be skipped. */
static int track_is_fully_populated(const Track *t) {
    return t->title[0] && t->artist[0] && t->album[0] &&
           t->isrc[0]  && t->cover[0];
}

/* fetch cover art, tag the file, clean up temp cover */
static void tag_and_clean(const Track *t, const char *final_out,
                           const char *out_dir,
                           const char *preloaded_cover,
                           void (*progress_cb)(const char *msg, void *ud),
                           void *ud) {
    Track rich = *t;
    /* Skip redundant network fetch when all key fields are already present
       (e.g. album mode: api_get_album_tracks already returned full metadata) */
    if (!track_is_fully_populated(t)) {
        (void)api_get_track_info(t->id, &rich);
    }

    /* Fetch lyrics by ISRC */
    if (rich.isrc[0]) {
        if (progress_cb) progress_cb("fetching lyrics…", ud);
        rich.lyrics = api_get_lyrics(rich.isrc);
    }

    char cover_tmp[1024] = {0};
    int cover_owned = 0;
    if (preloaded_cover && preloaded_cover[0]) {
        snprintf(cover_tmp, sizeof cover_tmp, "%s", preloaded_cover);
    } else if (rich.cover[0]) {
        snprintf(cover_tmp, sizeof cover_tmp, "%s" SQT_SEP ".sqt_cover_%s.jpg", out_dir, t->id);
        if (!fetch_cover(rich.cover, cover_tmp)) {
            cover_tmp[0] = '\0';
        } else {
            cover_owned = 1;
        }
    }

    if (progress_cb) progress_cb("tagging...", ud);
    int tag_ret = sqt_tag(final_out, &rich, cover_tmp[0] ? cover_tmp : NULL);
    if (tag_ret != 0 && progress_cb)
        progress_cb("warning: tagging failed (file still saved)", ud);

    if (rich.lyrics) free(rich.lyrics);
    if (cover_tmp[0] && cover_owned) remove(cover_tmp);
}

int download_track(const Track *t, const char *quality,
                   const char *out_dir,
                   const char *preloaded_cover,
                   void (*progress_cb)(const char *msg, void *ud),
                   void *ud) {
    (void)quality; /* Qobuz resolves quality automatically via hi-res-max */

    if (!t->isrc[0]) {
        if (progress_cb) progress_cb("error: no ISRC for qobuz lookup", ud);
        return -1;
    }

    if (progress_cb) progress_cb("fetching stream url…", ud);

    char qbz_url[SQT_URL_SZ] = {0};
    if (!api_qobuz_get_stream_url(t->isrc, qbz_url, sizeof qbz_url)) {
        if (progress_cb) progress_cb("error: qobuz lookup failed", ud);
        return -1;
    }

    char final_out[1040];
    build_out_path(t, out_dir, "flac", final_out, sizeof(final_out));

    if (download_single_url(qbz_url, final_out, progress_cb, ud) != 0) {
        if (progress_cb) progress_cb("error: download failed", ud);
        return -1;
    }

    tag_and_clean(t, final_out, out_dir, preloaded_cover, progress_cb, ud);
    if (progress_cb) progress_cb("done!", ud);
    return 0;
}
