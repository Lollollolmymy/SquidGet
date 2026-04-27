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
    return (http_get_file(cover_url, dest) > 0) ? 1 : 0;
}

void sqt_sanitise(const char *in, char *out, size_t outsz) {
    size_t j = 0;
    for (const char *p = in; *p && j + 1 < outsz; p++) {
        char c = *p;
        if (c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"'  || c == '<' || c == '>' || c == '|')
            c = '_';
        out[j++] = c;
    }
    out[j] = '\0';
}

static int download_single_url(const char *url, const char *out_path,
                                void (*progress_cb)(const char *msg, void *ud), void *ud) {
    if (progress_cb) progress_cb("downloading…", ud);

    char tmp_path[1200];
    snprintf(tmp_path, sizeof(tmp_path), "%s.sqtmp", out_path);

    if (http_get_file(url, tmp_path) < 0) {
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

/* fetch cover art, tag the file, clean up temp cover */
static void tag_and_clean(const Track *t, const char *final_out,
                           const char *out_dir,
                           const char *preloaded_cover,
                           void (*progress_cb)(const char *msg, void *ud),
                           void *ud) {
    Track rich = *t;
    (void)api_get_track_info(t->id, &rich);

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
    (void)tag_ret;

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
