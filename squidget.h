#pragma once
#include "thread.h"
#include <stddef.h>
#include <stdint.h>   /* uint32_t */

/* ── Path separator (used by download.c and main.c) ── */
#ifndef _WIN32
#  define SQT_SEP "/"
#else
#  define SQT_SEP "\\"
#endif

#define SQT_MAX_RESULTS  200
#define SQT_TITLE_SZ     256
#define SQT_URL_SZ      1024
#define SQT_ID_SZ         64
#define SQT_QUAL_SZ       64
#define SQT_STATUS_SZ    256
#define SQT_COVER_SZ     512
#define SQT_YEAR_SZ        8
#define SQT_ISRC_SZ       32
#define SQT_CPR_SZ       256
#define SQT_MAX_PLAYLISTS  64

/* quality labels */
#define QUAL_HIR  "HI_RES_LOSSLESS"
#define QUAL_LOS  "LOSSLESS"
#define QUAL_HIGH "HIGH"
#define QUAL_LOW  "LOW"
#define QUAL_ATM  "DOLBY_ATMOS"

#define QUALITY_COUNT 5
extern const char *const QUALITY_LABELS[QUALITY_COUNT];

/* ── Search type ── */
typedef enum {
    SEARCH_SONGS = 0,
    SEARCH_ALBUMS,
    SEARCH_PLAYLISTS,
} SearchType;

/* ── Track ── */
typedef struct Track {
    char  id[SQT_ID_SZ];
    char  title[SQT_TITLE_SZ];
    char  artist[SQT_TITLE_SZ];
    char  album[SQT_TITLE_SZ];
    char  cover[SQT_COVER_SZ];    /* UUID path "ab/cd/ef/…"  — empty if unknown */
    char  year[SQT_YEAR_SZ];      /* "2021" or ""                                */
    char  isrc[SQT_ISRC_SZ];
    char  copyright[SQT_CPR_SZ];
    int   duration;                /* seconds */
    int   track_num;
    int   disc_num;
    int   explicit_;
    float replay_gain;
    char  quality[SQT_QUAL_SZ];
    char *lyrics;  /* dynamically allocated LRC string or NULL */
} Track;

/* ── Album ── */
typedef struct {
    char id[SQT_ID_SZ];
    char title[SQT_TITLE_SZ];
    char artist[SQT_TITLE_SZ];
    int  num_tracks;
} Album;

typedef struct {
    char url[SQT_URL_SZ];
    char title[SQT_TITLE_SZ];
    int track_count;
} Playlist;

/* ── TUI modes ── */
typedef enum {
    MODE_SEARCH = 0,   /* typing a query                   */
    MODE_RESULTS,      /* browsing track/album list         */
    MODE_ALBUM_TRACKS, /* browsing tracks of an album       */
    MODE_QUALITY,      /* picking download quality          */
    MODE_SETUP,        /* first-run save-location UI        */
    MODE_ALBUM_ACTION, /* download album or browse songs    */
    MODE_HELP,         /* keybinding help overlay           */
    MODE_PLAYLIST_TRACKS,
} Mode;

/* frame-buffer row
 * prev_hash: FNV-1a 32-bit hash of the last-rendered content.
 * Replaces the old prev[FB_ROW_SZ] char array, halving per-row memory.
 * A hash collision (prob ~1 in 4 billion) causes one extra render — harmless. */
#define FB_ROW_SZ 8192  /* prevent ANSI truncation */
typedef struct {
    char     cur[FB_ROW_SZ];
    uint32_t prev_hash;   /* FNV-1a hash of content last flushed to terminal */
    int      dirty;
} FBRow;

typedef enum {
    TASK_SEARCH = 0,
    TASK_DOWNLOAD,
    TASK_ALBUM_SEARCH,
    TASK_ALBUM_TRACKS,
    TASK_ALBUM_DOWNLOAD,
    TASK_PLAYLIST_FETCH,
    TASK_PLAYLIST_DOWNLOAD
} TaskType;


typedef struct {
    TaskType type;
    char     query[512];
    char     track_id[SQT_ID_SZ];
    char     album_id[SQT_ID_SZ];
    char     album_name[SQT_TITLE_SZ];
    char     playlist_url[SQT_URL_SZ];
    char     playlist_name[SQT_TITLE_SZ];
    char     quality[SQT_QUAL_SZ];
    Track    track;
} SQTTask;

#define SQT_MAX_QUEUE 64

/* ── application state ── */
typedef struct {
    /* terminal */
    int rows, cols;

    /* mode */
    Mode mode;
    Mode prev_mode;

    /* search */
    char query[512];
    int  query_len;

    /* results */
    Track tracks[SQT_MAX_RESULTS];
    int   track_count;

    /* search type: songs or albums */
    SearchType search_type;

    /* album results */
    Album albums[SQT_MAX_RESULTS];
    int   album_count;

    Playlist playlists[SQT_MAX_PLAYLISTS];
    int      playlist_count;
    char     current_playlist_url[SQT_URL_SZ];
    int      current_playlist_index;

    /* album action picker */
    int album_action_cursor; /* 0=download complete, 1=browse songs */

    /* list navigation */
    int cursor;
    int scroll;

    /* quality picker */
    int qual_cursor;

    /* frame buffer */
    FBRow *fb;
    int    fb_rows;
    int    fb_cols;

    /* resolved output directory for downloads */
    char out_dir[512];

    /* ── first-run setup ── */
#define SETUP_MAX 5
    int  setup_cursor;
    char setup_presets[SETUP_MAX][512]; /* candidate paths; "" = Browse…  */
    char setup_labels[SETUP_MAX][128];  /* display strings for each row   */
    int  setup_count;

    /* status bar */
    char status[SQT_STATUS_SZ];

    /* configuration */
    int default_quality; /* -1 = unset, 0..4 = index in QUALITY_LABELS */

    /* background queue */
    SQTTask      queue[SQT_MAX_QUEUE];
    int          q_head, q_tail, q_count;

    /* background thread */
    sqt_mutex_t  lock;
    sqt_thread_t bg_thread;
    int          bg_running;
    int          dirty;
    unsigned int spin_frame;   /* unsigned: signed overflow is UB; was int */
} AppState;

/* ── config.c ── */
int  config_load(AppState *s);
void config_save(AppState *s);
void config_load_playlists(AppState *s);
void config_save_playlists(AppState *s);

/* ── platform.c ── */
/* Opens the OS-native folder picker; returns 1 + fills buf on success. */
int  gui_pick_folder(char *buf, size_t bufsz);

/* ── api.c ── */
/* Initialise / teardown HTTP layer. POSIX builds require libcurl; Windows uses WinHTTP. Call once. */
void  http_init(void);
void  http_cleanup(void);
char *http_get(const char *url);
typedef struct {
    long http_code;
    long retry_after_sec;
    char content_type[128];
    char effective_url[SQT_URL_SZ];
} HttpFileInfo;
long  http_get_file(const char *url, const char *path,
                    void (*progress_cb)(size_t received, size_t total, void *ud), void *ud);
long  http_get_file_ex(const char *url, const char *path,
                       void (*progress_cb)(size_t received, size_t total, void *ud), void *ud,
                       HttpFileInfo *info);
/* POST request with JSON body — returns allocated response body or NULL */
char *http_post(const char *url, const char *json_body);
const char *api_last_error(void);
int   api_search_tracks(const char *query, Track *out, int max);
int   api_search_albums(const char *query, Album *out, int max);
int   api_get_album_tracks(const char *album_id, Track *out, int max);
int   api_fetch_playlist(const char *url, Playlist *meta, Track *tracks, int max);
/* fetches /info/ for a single track — fills cover, year, ISRC, etc. */
int   api_get_track_info(const char *track_id, Track *out);
char *api_get_lyrics(const char *isrc);
/* Qobuz resolver: resolves ISRC or title metadata through the configured non-Amazon resolver.
   Returns 1 + fills out_url (size sz) on success; 0 on failure. */
int   api_qobuz_get_stream_url(const char *isrc, const char *quality, char *out_url, size_t sz);
int   api_qobuz_get_stream_url_err(const char *isrc, const char *quality,
                                   char *out_url, size_t sz,
                                   char *err, size_t errsz);
/* Try to resolve stream URL for a track using title+artist (Qobuz fallback for iTunes results) */
int   api_qobuz_get_stream_url_by_title_err(const char *title, const char *artist,
                                            int duration, const char *quality,
                                            char *out_url, size_t sz,
                                            char *err, size_t errsz);
void  api_qobuz_invalidate_stream_url(const char *url);
void  api_qobuz_note_download_response(long http_code, const char *content_type,
                                       long retry_after_sec, int media_ok);
int   api_qobuz_retry_delay_ms(int attempt);
int   api_qobuz_cooldown_remaining_ms(void);
const char *sqt_quality_expected_ext(const char *quality);

/* ── download.c ── */
/* sanitise a filename/directory component — strips / \ : * ? " < > | */
void sqt_sanitise(const char *in, char *out, size_t outsz);
/* preloaded_cover: path to a cached cover JPEG (album mode) or NULL to fetch per-track */
int download_track(const Track *t, const char *quality, const char *out_dir,
                   const char *preloaded_cover,
                   void (*progress_cb)(const char *msg, void *ud), void *ud);

/* ── layout constants ── */
#define HEADER_ROWS  2   /* top border + search bar */
#define FOOTER_ROWS  2   /* col header + keybind bar */

/* ── tui.c ── */
void tui_init(AppState *s);
void tui_cleanup(AppState *s);
void tui_render(AppState *s);
void tui_resize(AppState *s);
int  tui_read_key(AppState *s);

/* special keycodes */
#define KEY_TAB       9
#define KEY_UP        256
#define KEY_DOWN      257
#define KEY_LEFT      258
#define KEY_RIGHT     259
#define KEY_ENTER     260
#define KEY_ESC       261
#define KEY_BACKSPACE 262
#define KEY_CTRL_C    3
#define KEY_CTRL_U    21
#define KEY_HOME      263
#define KEY_END       264
#define KEY_PGUP      265
#define KEY_PGDN      266
