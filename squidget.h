#pragma once
#include "thread.h"
#include <stddef.h>

#ifndef SQT_LOG
#  define SQT_LOG(...) ((void)0)
#endif

/* ── Path separator (used by download.c and main.c) ── */
#ifndef _WIN32
#  define SQT_SEP "/"
#else
#  define SQT_SEP "\\"
#endif

/* ── API endpoints (split by benchmark results) ── */
/* search / album / info  →  api.monochrome  (won all 3 API rounds, 172–195 ms) */
#define SQT_BASE_API      "https://api.monochrome.tf"
/* /track/ manifest fetch →  hifi.geeked.wtf (score 20, avg 89 ms — fastest download) */
#define SQT_BASE_DOWNLOAD "https://hifi.geeked.wtf"
/* Tidal CDN for cover art */
#define SQT_TIDAL_IMG     "https://resources.tidal.com/images"

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

/* quality labels (must match API strings) */
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
} Track;

/* ── Album ── */
typedef struct {
    char id[SQT_ID_SZ];
    char title[SQT_TITLE_SZ];
    char artist[SQT_TITLE_SZ];
    int  num_tracks;
} Album;

/* ── TUI modes ── */
typedef enum {
    MODE_SEARCH = 0,   /* typing a query                   */
    MODE_RESULTS,      /* browsing track/album list         */
    MODE_QUALITY,      /* picking download quality          */
    MODE_SETUP,        /* first-run save-location UI        */
    MODE_ALBUM_ACTION, /* download album or browse songs    */
} Mode;

/* frame-buffer row */
#define FB_ROW_SZ 8192  /* prevent ANSI truncation */
typedef struct {
    char cur[FB_ROW_SZ];
    char prev[FB_ROW_SZ];
    int  dirty;
} FBRow;

/* ── application state ── */
typedef struct {
    /* terminal */
    int rows, cols;

    /* mode */
    Mode mode;

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

    /* background thread */
    sqt_mutex_t  lock;
    sqt_thread_t bg_thread;
    int          bg_running;
    int          dirty;
    unsigned int spin_frame;   /* unsigned: signed overflow is UB; was int */
} AppState;

/* ── config.c ── */
int  config_load(char *out_dir, size_t sz);
void config_save(const char *out_dir);

/* ── platform.c ── */
/* Opens the OS-native folder picker; returns 1 + fills buf on success. */
int  gui_pick_folder(char *buf, size_t bufsz);

/* ── api.c ── */
char *http_get(const char *url);
long  http_get_file(const char *url, const char *path);
/* POST request with JSON body — returns allocated response body or NULL */
char *http_post(const char *url, const char *json_body);
int   api_search_tracks(const char *query, Track *out, int max);
int   api_search_albums(const char *query, Album *out, int max);
int   api_get_album_tracks(const char *album_id, Track *out, int max);
/* fetches /info/ for a single track — fills cover, year, ISRC, etc. */
int   api_get_track_info(const char *track_id, Track *out);
/* Qobuz fallback: resolves ISRC → direct Akamai FLAC URL via zarz.moe.
   Returns 1 + fills out_url (size sz) on success; 0 on failure. */
int   api_qobuz_get_stream_url(const char *isrc, char *out_url, size_t sz);

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
#define KEY_HOME      263
#define KEY_END       264
#define KEY_PGUP      265
#define KEY_PGDN      266
