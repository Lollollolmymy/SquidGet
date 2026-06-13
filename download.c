#define _POSIX_C_SOURCE 200809L
#include "squidget.h"
#include "tag.h"
#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#ifdef _WIN32
#  include <io.h>
#  include <windows.h>
#  define access(p,m) _access(p,m)
#else
#  include <unistd.h>
#  include <sys/stat.h>
#endif


static void dl_debug_path(char *path, size_t pathsz) {
    const char *custom = getenv("SQUIDGET_DEBUG_LOG");
    if (custom && custom[0]) {
        snprintf(path, pathsz, "%s", custom);
        return;
    }
#ifdef _WIN32
    const char *base = getenv("APPDATA");
    if (base && base[0]) snprintf(path, pathsz, "%s\\squidget\\debug.log", base);
    else snprintf(path, pathsz, "squidget-debug.log");
#else
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && xdg[0]) snprintf(path, pathsz, "%s/squidget/debug.log", xdg);
    else if (home && home[0]) snprintf(path, pathsz, "%s/.config/squidget/debug.log", home);
    else snprintf(path, pathsz, "/tmp/squidget-debug.log");
#endif
}

static int dl_debug_enabled(void) {
    const char *custom = getenv("SQUIDGET_DEBUG_LOG");
    const char *v = getenv("SQUIDGET_DEBUG");
    return (custom && custom[0]) || (v && v[0] && strcmp(v, "0") != 0);
}

static void dl_debug(const char *fmt, ...) {
    if (!dl_debug_enabled()) return;
    char path[1024];
    dl_debug_path(path, sizeof path);

    FILE *f = fopen(path, "a");
    if (!f) return;

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (tm) {
        char ts[32];
        strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", tm);
        fprintf(f, "[%s] [download] ", ts);
    } else {
        fprintf(f, "[download] ");
    }

    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);

    fputc('\n', f);
    fclose(f);
}

static void dl_debug_file_head(const char *path) {
    unsigned char buf[64] = {0};
    FILE *f = fopen(path, "rb");
    if (!f) {
        dl_debug("file head: could not open '%s': %s", path, strerror(errno));
        return;
    }
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);

    char hex[3 * 64 + 1];
    size_t j = 0;
    for (size_t i = 0; i < n && j + 3 < sizeof hex; i++) {
        snprintf(hex + j, sizeof(hex) - j, "%02x ", buf[i]);
        j += 3;
    }
    hex[j] = '\0';

    char ascii[65];
    for (size_t i = 0; i < n && i + 1 < sizeof ascii; i++) {
        unsigned char c = buf[i];
        ascii[i] = (c >= 32 && c <= 126) ? (char)c : '.';
        ascii[i + 1] = '\0';
    }

    dl_debug("file head: bytes=%zu hex='%s' ascii='%s'", n, hex, ascii);
}

static long long dl_file_size(const char *path) {
#ifdef _WIN32
    struct _stat st;
    if (_stat(path, &st) == 0) return (long long)st.st_size;
#else
    struct stat st;
    if (stat(path, &st) == 0) return (long long)st.st_size;
#endif
    return -1;
}

static void dl_report_log_location(void (*progress_cb)(const char *msg, void *ud), void *ud) {
    if (!progress_cb || !dl_debug_enabled()) return;
    char path[1024];
    dl_debug_path(path, sizeof path);
    char msg[1024 + 32];
    snprintf(msg, sizeof msg, "debug log: %s", path);
    progress_cb(msg, ud);
}

/* cover_url is now a full https:// URL (Qobuz album.image.large) */
static int fetch_cover(const char *cover_url, const char *dest) {
    if (!cover_url || !cover_url[0]) return 0;
    dl_debug("cover: GET %s -> %s", cover_url, dest);
    long rc = http_get_file(cover_url, dest, NULL, NULL);
    dl_debug("cover: result=%ld size=%lld", rc, rc > 0 ? dl_file_size(dest) : -1);
    return (rc > 0) ? 1 : 0;
}

static unsigned long long sqt_fnv1a64(const char *s) {
    unsigned long long h = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        h ^= (unsigned long long)*p;
        h *= 1099511628211ULL;
    }
    return h;
}

static void sqt_mkdir_one(const char *path) {
#ifdef _WIN32
    CreateDirectoryA(path, NULL);
#else
    mkdir(path, 0755);
#endif
}

static void sqt_mkdir_p_local(const char *path) {
    if (!path || !path[0]) return;
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = '\0';
            sqt_mkdir_one(tmp);
            *p = c;
        }
    }
    sqt_mkdir_one(tmp);
}

static int cover_cache_path(const char *cover_url, char *out, size_t outsz) {
    if (!cover_url || !cover_url[0] || !out || outsz == 0) return 0;
#ifdef _WIN32
    const char *base = getenv("APPDATA");
    if (!base || !base[0]) return 0;
    char dir[1024];
    snprintf(dir, sizeof dir, "%s\\squidget\\covers", base);
#else
    const char *xdg = getenv("XDG_CACHE_HOME");
    const char *home = getenv("HOME");
    char dir[1024];
    if (xdg && xdg[0]) snprintf(dir, sizeof dir, "%s/squidget/covers", xdg);
    else if (home && home[0]) snprintf(dir, sizeof dir, "%s/.cache/squidget/covers", home);
    else snprintf(dir, sizeof dir, "/tmp/squidget-covers");
#endif
    sqt_mkdir_p_local(dir);
    char name[32];
    snprintf(name, sizeof name, "%016llx.jpg", sqt_fnv1a64(cover_url));
    size_t dlen = strlen(dir);
    size_t nlen = strlen(name);
    if (dlen + 1 + nlen + 1 > outsz) return 0;
    memcpy(out, dir, dlen);
#ifdef _WIN32
    out[dlen] = '\\';
#else
    out[dlen] = '/';
#endif
    memcpy(out + dlen + 1, name, nlen + 1);
    return 1;
}

static int fetch_cover_cached(const char *cover_url, char *out_path, size_t outsz) {
    if (!cover_cache_path(cover_url, out_path, outsz)) return 0;
    if (dl_file_size(out_path) > 0) {
        dl_debug("cover cache hit: %s", out_path);
        return 1;
    }
    return fetch_cover(cover_url, out_path);
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
    if (!ctx || !ctx->cb) return;

    uint64_t now = sqt_time_ms();
    if (now - ctx->last_report_time < 250 && received < total && received > 0) return;
    ctx->last_report_time = now;

    double elapsed = (double)(now - ctx->start_time) / 1000.0;
    if (elapsed < 0.01) elapsed = 0.01;
    double speed = (double)received / (1024.0 * 1024.0 * elapsed);

    char msg[128];
    if (total > 0) {
        int pct = (int)((double)received * 100.0 / (double)total);
        double rem = 0.0;
        if (received > 0) rem = (double)(total - received) / ((double)received / elapsed);
        int rm = (int)rem / 60, rs = (int)rem % 60;
        snprintf(msg, sizeof msg, "downloading… %3d%% (%.1f MB/s) %02d:%02d left",
                 pct, speed, rm, rs);
    } else {
        snprintf(msg, sizeof msg, "downloading… %.1f MB (%.1f MB/s)",
                 (double)received / (1024.0 * 1024.0), speed);
    }
    ctx->cb(msg, ctx->ud);
}

typedef enum {
    DL_FILE_INVALID = 0,
    DL_FILE_FLAC,
    DL_FILE_M4A,
    DL_FILE_MP4,
    DL_FILE_MP3,
    DL_FILE_OGG,
    DL_FILE_JSON,
    DL_FILE_HTML,
    DL_FILE_TEXT
} DownloadFileType;

static DownloadFileType sniff_downloaded_file(const char *path) {
    unsigned char buf[64] = {0};
    FILE *f = fopen(path, "rb");
    if (!f) return DL_FILE_INVALID;
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);

    if (n >= 4 && memcmp(buf, "fLaC", 4) == 0) return DL_FILE_FLAC;
    if (n >= 12 && memcmp(buf + 4, "ftyp", 4) == 0) {
        if (memcmp(buf + 8, "M4A ", 4) == 0 || memcmp(buf + 8, "M4B ", 4) == 0)
            return DL_FILE_M4A;
        return DL_FILE_MP4;
    }
    if (n >= 3 && memcmp(buf, "ID3", 3) == 0) return DL_FILE_MP3;
    if (n >= 2 && buf[0] == 0xff && (buf[1] & 0xe0) == 0xe0) return DL_FILE_MP3;
    if (n >= 4 && memcmp(buf, "OggS", 4) == 0) return DL_FILE_OGG;

    size_t i = 0;
    while (i < n && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n')) i++;
    if (i < n && (buf[i] == '{' || buf[i] == '[')) return DL_FILE_JSON;
    if (i + 5 <= n) {
        char h0 = (char)buf[i + 0], h1 = (char)buf[i + 1], h2 = (char)buf[i + 2], h3 = (char)buf[i + 3], h4 = (char)buf[i + 4];
        if (h0 >= 'A' && h0 <= 'Z') h0 = (char)(h0 - 'A' + 'a');
        if (h1 >= 'A' && h1 <= 'Z') h1 = (char)(h1 - 'A' + 'a');
        if (h2 >= 'A' && h2 <= 'Z') h2 = (char)(h2 - 'A' + 'a');
        if (h3 >= 'A' && h3 <= 'Z') h3 = (char)(h3 - 'A' + 'a');
        if (h4 >= 'A' && h4 <= 'Z') h4 = (char)(h4 - 'A' + 'a');
        if (h0 == '<' && h1 == 'h' && h2 == 't' && h3 == 'm' && h4 == 'l') return DL_FILE_HTML;
        if (h0 == '<' && h1 == '!' && h2 == 'd' && h3 == 'o' && h4 == 'c') return DL_FILE_HTML;
    }

    int printable = 0;
    for (size_t k = 0; k < n; k++) {
        if (buf[k] == 0) { printable = 0; break; }
        if ((buf[k] >= 32 && buf[k] <= 126) || buf[k] == '\r' || buf[k] == '\n' || buf[k] == '\t')
            printable++;
    }
    if (n > 0 && printable > (int)(n * 8 / 10)) return DL_FILE_TEXT;

    return DL_FILE_INVALID;
}


static const char *download_type_ext(DownloadFileType t) {
    switch (t) {
        case DL_FILE_FLAC: return "flac";
        case DL_FILE_M4A:  return "m4a";
        case DL_FILE_MP4:  return "mp4";
        case DL_FILE_MP3:  return "mp3";
        case DL_FILE_OGG:  return "ogg";
        case DL_FILE_JSON: return "json";
        case DL_FILE_HTML: return "html";
        case DL_FILE_TEXT: return "txt";
        case DL_FILE_INVALID:
        default: return "bin";
    }
}

static const char *download_save_ext(DownloadFileType t, const char *fallback_ext) {
    (void)fallback_ext;
    return download_type_ext(t);
}

static int download_type_is_media(DownloadFileType t) {
    return t == DL_FILE_FLAC || t == DL_FILE_M4A || t == DL_FILE_MP4 ||
           t == DL_FILE_MP3  || t == DL_FILE_OGG;
}

static int download_type_is_taggable_audio(DownloadFileType t) {
    return t == DL_FILE_FLAC || t == DL_FILE_M4A || t == DL_FILE_MP4;
}

static int content_type_looks_non_media(const char *ctype) {
    if (!ctype || !ctype[0]) return 0;
    char low[128];
    size_t i = 0;
    for (; ctype[i] && i + 1 < sizeof low; i++) {
        char c = ctype[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        low[i] = c;
    }
    low[i] = '\0';
    return strstr(low, "text/html") || strstr(low, "application/json") ||
           strstr(low, "text/plain") || strstr(low, "text/xml") || strstr(low, "application/xml");
}

static void path_with_ext_dedup(const char *path, const char *ext, char *out, size_t outsz) {
    if (!path || !path[0] || !ext || !ext[0] || !out || outsz == 0) return;

    char base[4096];
    snprintf(base, sizeof base, "%s", path);

    char *last_sep1 = strrchr(base, '/');
#ifdef _WIN32
    char *last_sep2 = strrchr(base, '\\');
    char *last_sep = last_sep1 > last_sep2 ? last_sep1 : last_sep2;
#else
    char *last_sep = last_sep1;
#endif
    char *dot = strrchr(base, '.');
    if (!dot || (last_sep && dot < last_sep)) {
        dot = base + strlen(base);
    }
    *dot = '\0';

    snprintf(out, outsz, "%s.%s", base, ext);
    if (access(out, F_OK) != 0) return;

    for (int n = 2; n < 1000; n++) {
        int written = snprintf(out, outsz, "%s (%d).%s", base, n, ext);
        if (written < 0 || (size_t)written >= outsz) break;
        if (access(out, F_OK) != 0) return;
    }
}

static int quality_is_high(const char *quality) {
    return quality && (strcmp(quality, QUAL_HIGH) == 0 ||
                       strcmp(quality, QUALITY_LABELS[2]) == 0);
}

static int quality_is_low(const char *quality) {
    return quality && (strcmp(quality, QUAL_LOW) == 0 ||
                       strcmp(quality, QUALITY_LABELS[3]) == 0);
}

static int quality_is_atmos(const char *quality) {
    return quality && (strcmp(quality, QUAL_ATM) == 0 ||
                       strcmp(quality, QUALITY_LABELS[4]) == 0);
}

static const char *quality_expected_ext(const char *quality) {
    if (quality_is_high(quality) || quality_is_low(quality)) return "mp3";
    if (quality_is_atmos(quality)) return "mp4";
    return "flac";
}

const char *sqt_quality_expected_ext(const char *quality) {
    return quality_expected_ext(quality);
}

enum {
    DL_ATTEMPT_OK = 0,
    DL_ATTEMPT_HTTP = -1,
    DL_ATTEMPT_NON_MEDIA = -2
};

static int download_single_url(const char *url, const char *fallback_out_path,
                               const char *fallback_ext,
                               char *saved_out, size_t saved_outsz,
                               DownloadFileType *saved_kind,
                               void (*progress_cb)(const char *msg, void *ud), void *ud) {
    AnalyticsCtx actx = { sqt_time_ms(), 0, progress_cb, ud };

    char tmp_path[8192];
    snprintf(tmp_path, sizeof(tmp_path), "%s.sqtmp", fallback_out_path);

    dl_debug("download: accept-any-media enabled url=%s", url);
    dl_debug("download: tmp_path=%s", tmp_path);
    dl_debug("download: fallback_path=%s fallback_ext=%s", fallback_out_path, fallback_ext ? fallback_ext : "(null)");

    if (progress_cb) progress_cb("requesting final media stream…", ud);
    HttpFileInfo hinfo;
    long get_rc = http_get_file_ex(url, tmp_path, internal_progress_cb, &actx, &hinfo);
    dl_debug("download: http_get_file result=%ld http=%ld type='%s' retry_after=%ld tmp_size=%lld",
             get_rc, hinfo.http_code, hinfo.content_type, hinfo.retry_after_sec,
             get_rc > 0 ? dl_file_size(tmp_path) : -1);
    if (get_rc <= 0) {
        api_qobuz_note_download_response(hinfo.http_code, hinfo.content_type,
                                         hinfo.retry_after_sec, 0);
        dl_debug("download: http_get_file failed; removing tmp file");
        remove(tmp_path);
        return DL_ATTEMPT_HTTP;
    }

    dl_debug_file_head(tmp_path);
    DownloadFileType kind = sniff_downloaded_file(tmp_path);
    const char *actual_ext = download_save_ext(kind, fallback_ext);
    int media_ok = download_type_is_media(kind) && !content_type_looks_non_media(hinfo.content_type);
    api_qobuz_note_download_response(hinfo.http_code, hinfo.content_type,
                                     hinfo.retry_after_sec, media_ok);
    dl_debug("download: sniffed=%s ctype='%s' media_ok=%d save_ext=%s",
             download_type_ext(kind), hinfo.content_type, media_ok, actual_ext);

    if (!media_ok) {
        dl_debug("download: rejected non-media resolver/CDN response; deleting tmp");
        remove(tmp_path);
        if (progress_cb) {
            char msg[220];
            snprintf(msg, sizeof msg, "resolver returned non-audio (%s); retrying…",
                     hinfo.content_type[0] ? hinfo.content_type : download_type_ext(kind));
            progress_cb(msg, ud);
        }
        return DL_ATTEMPT_NON_MEDIA;
    }

    char final_path[4096];
    path_with_ext_dedup(fallback_out_path, actual_ext, final_path, sizeof final_path);
    if (!final_path[0]) snprintf(final_path, sizeof final_path, "%s", fallback_out_path);

    if (rename(tmp_path, final_path) != 0) {
        dl_debug("download: rename tmp -> final failed: %s", strerror(errno));
        remove(tmp_path);
        return DL_ATTEMPT_HTTP;
    }

    if (saved_out && saved_outsz) snprintf(saved_out, saved_outsz, "%s", final_path);
    if (saved_kind) *saved_kind = kind;

    dl_debug("download: saved final file size=%lld path=%s", dl_file_size(final_path), final_path);
    if (progress_cb) {
        char msg[180];
        snprintf(msg, sizeof msg, "saved as .%s", actual_ext);
        progress_cb(msg, ud);
    }
    return DL_ATTEMPT_OK;
}



/* build sanitised output path, create dir, handle collisions */
static void build_out_path(const Track *t, const char *out_dir,
                            const char *ext, char *final_out, size_t outsz) {
    char artist_s[220], title_s[220];
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
        char base[960];
        snprintf(base, sizeof(base), "%s" SQT_SEP "%s - %s",
                 out_dir, artist_s, title_s);
        for (int n = 2; n < 1000; n++) {
            int written = snprintf(final_out, outsz, "%s (%d).%s", base, n, ext);
            if (written < 0 || (size_t)written >= outsz) {
                snprintf(final_out, outsz, "%s" SQT_SEP "track-%d.%s", out_dir, n, ext);
            }
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
    (void)out_dir;
    Track rich = *t;
    /* Skip redundant network fetch when all key fields are already present
       (e.g. album mode: api_get_album_tracks already returned full metadata) */
    if (!track_is_fully_populated(t)) {
        (void)api_get_track_info(t->id, &rich);
    }

    /* Lyrics are optional because they add an extra network call after download. */
    const char *lyrics_env = getenv("SQUIDGET_FETCH_LYRICS");
    if (lyrics_env && lyrics_env[0] && strcmp(lyrics_env, "0") != 0 && rich.isrc[0]) {
        if (progress_cb) progress_cb("fetching lyrics…", ud);
        rich.lyrics = api_get_lyrics(rich.isrc);
    }

    char cover_tmp[1024] = {0};
    if (preloaded_cover && preloaded_cover[0]) {
        snprintf(cover_tmp, sizeof cover_tmp, "%s", preloaded_cover);
    } else if (rich.cover[0]) {
        if (!fetch_cover_cached(rich.cover, cover_tmp, sizeof cover_tmp)) {
            cover_tmp[0] = '\0';
        }
    }

    if (progress_cb) progress_cb("tagging...", ud);
    int tag_ret = sqt_tag(final_out, &rich, cover_tmp[0] ? cover_tmp : NULL);
    if (tag_ret != 0 && progress_cb)
        progress_cb("warning: tagging failed (file still saved)", ud);

    if (rich.lyrics) free(rich.lyrics);
}

int download_track(const Track *t, const char *quality,
                   const char *out_dir,
                   const char *preloaded_cover,
                   void (*progress_cb)(const char *msg, void *ud),
                   void *ud) {
    dl_debug("---- download_track begin ----");
    dl_debug("track: id='%s' title='%s' artist='%s' album='%s' isrc='%s' duration=%d quality_arg='%s' out_dir='%s'",
             t ? t->id : "(null)",
             t ? t->title : "(null)",
             t ? t->artist : "(null)",
             t ? t->album : "(null)",
             t ? t->isrc : "(null)",
             t ? t->duration : -1,
             quality ? quality : "(null)",
             out_dir ? out_dir : "(null)");
    dl_report_log_location(progress_cb, ud);

    if (!t || (!t->isrc[0] && !t->title[0])) {
        dl_debug("error: missing track metadata for non-Amazon stream lookup");
        if (progress_cb) progress_cb("error: missing track metadata", ud);
        return -1;
    }

    char fallback_out[4096];
    const char *fallback_ext = quality_expected_ext(quality);
    build_out_path(t, out_dir, fallback_ext, fallback_out, sizeof(fallback_out));

    DownloadFileType saved_kind = DL_FILE_INVALID;
    char final_out[4096] = {0};
    int saved_ok = 0;
    int track_retries = 2;
    const char *retry_env = getenv("SQUIDGET_TRACK_RETRIES");
    if (retry_env && retry_env[0]) {
        char *endp = NULL;
        long v = strtol(retry_env, &endp, 10);
        if (endp != retry_env && v >= 0 && v <= 8) track_retries = (int)v;
    }

    for (int attempt = 0; attempt <= track_retries; attempt++) {
        if (progress_cb) {
            if (attempt == 0) progress_cb("fetching stream url…", ud);
            else {
                char msg[120];
                snprintf(msg, sizeof msg, "retrying stream… %d/%d", attempt, track_retries);
                progress_cb(msg, ud);
            }
        }

        char stream_url[SQT_URL_SZ] = {0};
        char lookup_err[160] = {0};
        int got_stream = 0;

        /* Non-Amazon path: prefer ISRC when available, then title+artist Qobuz lookup.
           iTunes search IDs are metadata IDs only, so they are intentionally not
           treated as stream IDs here. */
        if (t->isrc[0]) {
            dl_debug("stream lookup: qobuz by isrc=%s quality=%s attempt=%d",
                     t->isrc, quality ? quality : "(null)", attempt);
            got_stream = api_qobuz_get_stream_url_err(t->isrc, quality,
                                                      stream_url, sizeof stream_url,
                                                      lookup_err, sizeof lookup_err);
        }

        if (!got_stream && t->title[0]) {
            char qobuz_err[160] = {0};
            dl_debug("stream lookup: qobuz by title='%s' artist='%s' quality=%s attempt=%d",
                     t->title, t->artist, quality ? quality : "(null)", attempt);
            got_stream = api_qobuz_get_stream_url_by_title_err(t->title, t->artist,
                                                               t->duration, quality,
                                                               stream_url, sizeof stream_url,
                                                               qobuz_err, sizeof qobuz_err);
            if (!got_stream && qobuz_err[0])
                snprintf(lookup_err, sizeof lookup_err, "%s", qobuz_err);
        }

        if (!got_stream) {
            dl_debug("stream lookup: failed lookup_err='%s' attempt=%d", lookup_err, attempt);
            if (attempt >= track_retries) {
                if (progress_cb) {
                    char msg[220];
                    if (lookup_err[0]) snprintf(msg, sizeof msg, "error: %s", lookup_err);
                    else snprintf(msg, sizeof msg, "error: could not fetch stream URL");
                    progress_cb(msg, ud);
                }
                return -1;
            }
        } else {
            dl_debug("stream lookup: success stream_url=%s", stream_url);
            dl_debug("output: fallback_out=%s fallback_ext=%s", fallback_out, fallback_ext);
            int drc = download_single_url(stream_url, fallback_out, fallback_ext,
                                          final_out, sizeof final_out, &saved_kind,
                                          progress_cb, ud);
            if (drc == DL_ATTEMPT_OK) {
                saved_ok = 1;
                break;
            }
            api_qobuz_invalidate_stream_url(stream_url);
            if (drc == DL_ATTEMPT_NON_MEDIA) {
                dl_debug("download attempt returned non-media; invalidated stream URL");
            } else {
                dl_debug("download attempt failed; invalidated stream URL");
            }
            if (attempt >= track_retries) {
                dl_debug("---- download_track failed during download ----");
                return -1;
            }
        }

        int delay = api_qobuz_retry_delay_ms(attempt);
        if (delay > 0) {
            if (progress_cb) {
                char msg[160];
                snprintf(msg, sizeof msg, "resolver cooling down %.1fs…", (double)delay / 1000.0);
                progress_cb(msg, ud);
            }
            sqt_sleep_ms((unsigned)delay);
        }
    }

    if (!saved_ok) {
        dl_debug("---- download_track failed after retries ----");
        return -1;
    }

    if (download_type_is_taggable_audio(saved_kind)) {
        dl_debug("tagging: begin");
        tag_and_clean(t, final_out, out_dir, preloaded_cover, progress_cb, ud);
        dl_debug("tagging: finished");
    } else {
        dl_debug("tagging: skipped for saved type=%s", download_type_ext(saved_kind));
        if (progress_cb) progress_cb("warning: tagging skipped for this file type", ud);
    }
    if (progress_cb) progress_cb("done!", ud);
    dl_debug("---- download_track success ----");
    return 0;
}
