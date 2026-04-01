#pragma once
#include "thread.h"
#include <stddef.h>

/* ── API endpoint ── */
#define SQT_BASE        "https://hifi-one.spotisaver.net"
#define SQT_MAX_RESULTS  200
#define SQT_TITLE_SZ     256
#define SQT_URL_SZ      1024
#define SQT_ID_SZ         64
#define SQT_QUAL_SZ       64
#define SQT_STATUS_SZ    256

/* quality labels (must match API strings) */
#define QUAL_HIR  "HI_RES_LOSSLESS"
#define QUAL_LOS  "LOSSLESS"
#define QUAL_HIGH "HIGH"
#define QUAL_LOW  "LOW"
#define QUAL_ATM  "DOLBY_ATMOS"

static const char *const QUALITY_LABELS[] = {
    QUAL_HIR, QUAL_LOS, QUAL_HIGH, QUAL_LOW, QUAL_ATM
};
#define QUALITY_COUNT 5

/* ── Track ── */
typedef struct {
    char id[SQT_ID_SZ];
    char title[SQT_TITLE_SZ];
    char artist[SQT_TITLE_SZ];
    char album[SQT_TITLE_SZ];
    int  duration;           /* seconds */
    char quality[SQT_QUAL_SZ];
} Track;

/* ── TUI modes ── */
typedef enum {
    MODE_SEARCH = 0,   /* typing a query             */
    MODE_RESULTS,      /* browsing track list         */
    MODE_QUALITY,      /* picking download quality    */
    MODE_SETUP,        /* first-run save-location UI  */
} Mode;

/* frame-buffer row */
#define FB_ROW_SZ 4096
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
    int          spin_frame;
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
int   api_search_tracks(const char *query, Track *out, int max);

/* ── download.c ── */
int download_track(const Track *t, const char *quality, const char *out_dir,
                   void (*progress_cb)(const char *msg, void *ud), void *ud);

/* ── layout constants ── */
#define HEADER_ROWS  2   /* top border + search bar */
#define FOOTER_ROWS  2   /* col header + keybind bar */

/* ── tui.c ── */
void tui_init(AppState *s);
void tui_cleanup(AppState *s);
void tui_render(AppState *s);
int  tui_read_key(AppState *s);
void tui_resize(AppState *s);

/* special keycodes */
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
