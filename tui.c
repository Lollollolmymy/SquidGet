/* tui.c — optimized and cleaned */
#define _DEFAULT_SOURCE
#include "squidget.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef _WIN32
#  include <termios.h>
#  include <sys/ioctl.h>
#  include <unistd.h>
static struct termios g_orig;
#else
#  include <windows.h>
#  include <conio.h>
#  ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#    define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#  endif
static DWORD g_orig_in, g_orig_out;
#endif

/* ANSI codes */
#define A_RST   "\033[0m"
#define A_BOLD  "\033[1m"
#define A_DIM   "\033[2m"
#define A_HIDE  "\033[?25l"
#define A_SHOW  "\033[?25h"
#define A_CLR   "\033[2J\033[H"
#define A_ALT_ON  "\033[?1049h"
#define A_ALT_OFF "\033[?1049l"
#define A_EOL   "\033[K"

/* Color schemes */
typedef struct { const char *border, *logo, *hdr, *sel_num, *keyname, *prompt, *qsel, *cursor, *mode; } ColorScheme;

static const ColorScheme SONG_SCHEME = {
    .border = "\033[38;5;61m",
    .logo = "\033[38;5;111m",
    .hdr = "\033[38;5;67m",
    .sel_num = "\033[38;5;111m",
    .keyname = "\033[38;5;111m",
    .prompt = "\033[38;5;111m",
    .qsel = "\033[38;5;111m",
    .cursor = "\033[38;5;226m",
    .mode = "\033[38;5;67m"
};

static const ColorScheme ALBUM_SCHEME = {
    .border = "\033[38;5;130m",
    .logo = "\033[38;5;214m",
    .hdr = "\033[38;5;172m",
    .sel_num = "\033[38;5;214m",
    .keyname = "\033[38;5;214m",
    .prompt = "\033[38;5;214m",
    .qsel = "\033[38;5;214m",
    .cursor = "\033[38;5;208m",
    .mode = "\033[38;5;172m"
};

static const char *SPIN[] = {"⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"};
#define NSPIN 10
#define IND "▶"

#define TC(s) ((s)->search_type == SEARCH_ALBUMS ? &ALBUM_SCHEME : &SONG_SCHEME)

/* Row builder */
static char g_row[FB_ROW_SZ];
static int g_rp;

static void rb_reset(void) { g_rp = 0; g_row[0] = '\0'; }
static void rb_s(const char *s) {
    if (!s) return;
    int rem = FB_ROW_SZ - g_rp - 1;
    if (rem <= 0) return;
    int n = snprintf(g_row + g_rp, (size_t)(rem + 1), "%s", s);
    if (n > 0 && n <= rem) g_rp += n;
}
static void rb_pad(int n) {
    while (n-- > 0 && g_rp < FB_ROW_SZ - 1) g_row[g_rp++] = ' ';
    g_row[g_rp] = '\0';
}
static void rb_rep(const char *s, int n) { while (n-- > 0) rb_s(s); }
static const char *rb_done(void) { return g_row; }

/* UTF-8 width (simplified) */
static int utf8_width(const char *s) {
    int w = 0;
    while (*s) {
        if (*s == '\033') {
            s++;
            if (*s == '[') {
                s++;
                while (*s && (*s < 0x40 || *s > 0x7E)) s++;
                if (*s) s++;
            } else if (*s) s++;
            continue;
        }
        unsigned char c = (unsigned char)*s++;
        if (c < 0x80) w++;
        else if (c < 0xE0) { s++; w++; }
        else if (c < 0xF0) { s += 2; w += 2; }
        else { s += 3; w += 2; }
    }
    return w;
}

static void trunc_to(const char *src, char *dst, size_t dstsz, int maxw) {
    if (maxw <= 0) { dst[0] = '\0'; return; }
    if (utf8_width(src) <= maxw) { snprintf(dst, dstsz, "%s", src); return; }
    const char *p = src;
    size_t i = 0;
    int w = 0;
    while (*p && w < maxw - 1) {
        const char *prev = p;
        unsigned char c = (unsigned char)*p++;
        int cpw = 1;
        if (c < 0x80) cpw = 1;
        else if (c < 0xE0) { p++; cpw = 1; }
        else if (c < 0xF0) { p += 2; cpw = 2; }
        else { p += 3; cpw = 2; }
        if (w + cpw > maxw - 1) break;
        size_t seg = (size_t)(p - prev);
        if (i + seg + 4 >= dstsz) break;
        memcpy(dst + i, prev, seg);
        i += seg;
        w += cpw;
    }
    if (i + 4 < dstsz) { dst[i++] = (char)0xE2; dst[i++] = (char)0x80; dst[i++] = (char)0xA6; }
    dst[i] = '\0';
}

static void fmt_dur(int total_s, char *b, size_t bsz) {
    int s = total_s < 0 ? 0 : total_s;
    int m = s / 60, sec = s % 60;
    if (m >= 60) snprintf(b, bsz, "%d:%02d:%02d", (m / 60) % 100, m % 60, sec);
    else snprintf(b, bsz, "%d:%02d", m % 1000, sec);
}

/* Framebuffer */
static void fb_ensure(AppState *s) {
    if (s->fb && s->fb_rows == s->rows && s->fb_cols == s->cols) return;
    free(s->fb);
    s->fb = calloc((size_t)s->rows, sizeof(FBRow));
    if (!s->fb) { s->fb_rows = 0; s->fb_cols = 0; return; }
    s->fb_rows = s->rows;
    s->fb_cols = s->cols;
    for (int i = 0; i < s->rows; i++) s->fb[i].dirty = 1;
}

static void fb_put(AppState *s, int row, const char *line) {
    if (!s->fb || row < 0 || row >= s->fb_rows) return;
    FBRow *r = &s->fb[row];
    snprintf(r->cur, sizeof(r->cur), "%s", line);
    r->dirty = strcmp(r->cur, r->prev) != 0;
}

static void fb_flush(AppState *s) {
    fputs(A_HIDE, stdout);
    for (int i = 0; i < s->fb_rows; i++) {
        FBRow *r = &s->fb[i];
        if (!r->dirty) continue;
        printf("\033[%d;1H", i + 1);
        fputs(r->cur, stdout);
        fputs(A_EOL, stdout);
        memcpy(r->prev, r->cur, sizeof(r->cur));
        r->dirty = 0;
    }
    fputs(A_SHOW, stdout);
    fflush(stdout);
}

/* Raw mode */
static void raw_on(void) {
#ifndef _WIN32
    tcgetattr(STDIN_FILENO, &g_orig);
    struct termios r = g_orig;
    r.c_lflag &= (tcflag_t)~(ECHO | ICANON | ISIG);
    r.c_iflag &= (tcflag_t)~(IXON | ICRNL | BRKINT | ISTRIP);
    r.c_cflag |= CS8;
    r.c_cc[VMIN] = 0; r.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &r);
#else
    HANDLE hi = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE ho = GetStdHandle(STD_OUTPUT_HANDLE);
    GetConsoleMode(hi, &g_orig_in);
    GetConsoleMode(ho, &g_orig_out);
    SetConsoleMode(hi, g_orig_in & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT));
    SetConsoleMode(ho, g_orig_out | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
}

static void raw_off(void) {
#ifndef _WIN32
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig);
#else
    SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), g_orig_in);
    SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), g_orig_out);
#endif
}

static void term_size(int *rows, int *cols) {
#ifndef _WIN32
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
        return;
    }
#else
    CONSOLE_SCREEN_BUFFER_INFO c;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &c)) {
        *cols = c.srWindow.Right - c.srWindow.Left + 1;
        *rows = c.srWindow.Bottom - c.srWindow.Top + 1;
        return;
    }
#endif
    *rows = 24; *cols = 80;
}

/* Column width calculation */
static void list_col_widths(int W, int *tw, int *aw) {
    int inner = W - 2;
    int fixed = 1 + 3 + 1 + 2 + 1 + 5 + 1;
    int rem = inner - fixed;
    if (rem < 8) rem = 8;
    *tw = (rem * 55) / 100;
    *aw = rem - *tw;
}

/* Render functions */
static void render_top_border(AppState *s, int W) {
    const char *lbl = " squidget ";
    int lblw = (int)strlen(lbl);
    int ld = (W - 2 - lblw) / 2;
    int rd = W - 2 - lblw - ld;
    if (ld < 0) ld = 0;
    if (rd < 0) rd = 0;
    const ColorScheme *c = TC(s);
    rb_reset();
    rb_s(c->border);
    rb_s("\xe2\x95\xad");
    rb_rep("\xe2\x94\x80", ld);
    rb_s(A_BOLD);
    rb_s(c->logo);
    rb_s(lbl);
    rb_s(A_RST);
    rb_s(c->border);
    rb_rep("\xe2\x94\x80", rd);
    rb_s("\xe2\x95\xaf");
    rb_s(A_RST);
    fb_put(s, 0, rb_done());
}

static void render_search_bar(AppState *s, int W) {
    const ColorScheme *c = TC(s);
    rb_reset();
    rb_s(c->border);
    rb_s("\xe2\x94\x82" A_RST " ");
    int used = 2;
    
    const char *badge = 
        s->mode == MODE_SEARCH  ? (s->search_type == SEARCH_ALBUMS ? " ALBUM SEARCH " : " SONG SEARCH ") :
        s->mode == MODE_RESULTS ? (s->search_type == SEARCH_ALBUMS ? " ALBUM RESULTS " : " SONG RESULTS ") :
        s->mode == MODE_SETUP   ? " SETUP " :
        s->mode == MODE_ALBUM_ACTION ? " ALBUM " : " QUALITY ";
    
    rb_s("\033[48;5;237m");
    rb_s(c->mode);
    rb_s(A_BOLD);
    rb_s(badge);
    rb_s(A_RST " ");
    used += utf8_width(badge) + 1;
    
    rb_s(c->prompt);
    rb_s(A_BOLD "/" A_RST " ");
    used += 2;
    
    if (s->bg_running) {
        rb_s("\033[38;5;179m");
        rb_s(SPIN[s->spin_frame % NSPIN]);
        rb_s(A_RST " ");
        used += 2;
    }
    
    int qspace = W - used - 2;
    if (qspace < 0) qspace = 0;
    char qbuf[512];
    trunc_to(s->query, qbuf, sizeof(qbuf), qspace);
    rb_s("\033[38;5;253m");
    rb_s(A_BOLD);
    rb_s(qbuf);
    rb_s(A_RST);
    used += utf8_width(qbuf);
    
    if ((s->mode == MODE_SEARCH || s->mode == MODE_RESULTS) && used < W - 2) {
        rb_s(c->prompt);
        rb_s("│" A_RST);
        used++;
    }
    
    rb_pad(W - 1 - used);
    rb_s(c->border);
    rb_s("\xe2\x94\x82" A_RST);
    fb_put(s, 1, rb_done());
}

static void render_header(AppState *s, int inner, int tw, int aw) {
    const ColorScheme *c = TC(s);
    rb_reset();
    rb_s(c->border);
    rb_s("\xe2\x94\x9c" A_RST);
    
    char hdr[512];
    if (s->search_type == SEARCH_ALBUMS)
        snprintf(hdr, sizeof(hdr), " %3s  %-*s  %-*s  %s", "#", tw, "album", aw, "artist", "trk");
    else
        snprintf(hdr, sizeof(hdr), " %3s  %-*s  %-*s  %s", "#", tw, "title", aw, "artist", "dur");
    
    char hdr_t[512];
    trunc_to(hdr, hdr_t, sizeof(hdr_t), inner);
    rb_s(c->hdr);
    rb_s(A_DIM);
    rb_s(hdr_t);
    rb_pad(inner - utf8_width(hdr_t));
    rb_s(A_RST);
    rb_s(c->border);
    rb_s("\xe2\x94\xa4" A_RST);
    fb_put(s, 2, rb_done());
}

static void render_list_row(AppState *s, int r, int idx, int inner, int tw, int aw, int is_sel, int list_top) {
    const ColorScheme *c = TC(s);
    rb_reset();
    rb_s(c->border);
    rb_s("\xe2\x94\x82" A_RST);
    
    if (is_sel) rb_s("\033[48;5;237m");
    
    char nstr[8];
    snprintf(nstr, sizeof(nstr), "%3d", (idx + 1) % 1000);
    rb_s(" ");
    rb_s(is_sel ? c->sel_num : "\033[38;5;238m");
    if (is_sel) rb_s(A_BOLD);
    rb_s(nstr);
    rb_s(A_RST);
    if (is_sel) rb_s("\033[48;5;237m");
    rb_s(" ");
    
    if (is_sel) {
        rb_s(c->cursor);
        rb_s(A_BOLD);
        rb_s(IND " ");
        rb_s(A_RST);
        rb_s("\033[48;5;237m");
    } else {
        rb_s("  ");
    }
    
    if (s->search_type == SEARCH_ALBUMS && idx < s->album_count) {
        char atitl[SQT_TITLE_SZ];
        trunc_to(s->albums[idx].title, atitl, sizeof(atitl), tw);
        rb_s(is_sel ? "\033[38;5;255m" A_BOLD : "\033[38;5;253m");
        rb_s(atitl);
        rb_pad(tw - utf8_width(atitl) + 1);
        
        char artst[SQT_TITLE_SZ];
        trunc_to(s->albums[idx].artist, artst, sizeof(artst), aw);
        rb_s(is_sel ? "\033[38;5;255m" : "\033[38;5;246m");
        rb_s(artst);
        rb_pad(aw - utf8_width(artst) + 1);
        
        char trkcnt[8];
        snprintf(trkcnt, sizeof(trkcnt), "%3d", s->albums[idx].num_tracks % 1000);
        rb_s(is_sel ? "\033[38;5;255m" : "\033[38;5;242m");
        rb_s(trkcnt);
    } else if (s->search_type == SEARCH_SONGS && idx < s->track_count) {
        char titl[SQT_TITLE_SZ];
        trunc_to(s->tracks[idx].title, titl, sizeof(titl), tw);
        rb_s(is_sel ? "\033[38;5;255m" A_BOLD : "\033[38;5;253m");
        rb_s(titl);
        rb_pad(tw - utf8_width(titl) + 1);
        
        char artst[SQT_TITLE_SZ];
        trunc_to(s->tracks[idx].artist, artst, sizeof(artst), aw);
        rb_s(is_sel ? "\033[38;5;255m" : "\033[38;5;246m");
        rb_s(artst);
        rb_pad(aw - utf8_width(artst) + 1);
        
        char dur[12];
        fmt_dur(s->tracks[idx].duration, dur, sizeof(dur));
        rb_s(is_sel ? "\033[38;5;255m" : "\033[38;5;242m");
        rb_s(dur);
    } else {
        rb_pad(inner);
    }
    
    rb_s(A_RST);
    rb_s(c->border);
    rb_s("\xe2\x94\x82" A_RST);
    fb_put(s, list_top + r, rb_done());
}

static void render_bottom_border(AppState *s, int inner) {
    const ColorScheme *c = TC(s);
    rb_reset();
    rb_s(c->border);
    rb_s("\xe2\x95\xb0");
    rb_rep("\xe2\x94\x80", inner);
    rb_s("\xe2\x95\xaf");
    rb_s(A_RST);
    fb_put(s, s->rows - 2, rb_done());
}

static void render_status_bar(AppState *s, int W) {
    rb_reset();
    rb_s(" ");
    int kw = 1;
    
    typedef struct { const char *k, *v; } KB;
    KB kb[8]; int nkb = 0;
    
    if (s->mode == MODE_SEARCH || s->mode == MODE_RESULTS) {
        kb[nkb++] = (KB){"↑↓", "nav"};
        kb[nkb++] = (KB){"enter", "select"};
        kb[nkb++] = (KB){"^U", "clear"};
        kb[nkb++] = (KB){"tab", s->search_type == SEARCH_ALBUMS ? "songs" : "albums"};
        kb[nkb++] = (KB){"^C", "quit"};
    } else if (s->mode == MODE_QUALITY) {
        kb[nkb++] = (KB){"↑↓", "move"};
        kb[nkb++] = (KB){"1-5", "pick"};
        kb[nkb++] = (KB){"enter", "dl"};
        kb[nkb++] = (KB){"esc", "cancel"};
    } else if (s->mode == MODE_ALBUM_ACTION) {
        kb[nkb++] = (KB){"↑↓", "move"};
        kb[nkb++] = (KB){"1-2", "pick"};
        kb[nkb++] = (KB){"enter", "confirm"};
        kb[nkb++] = (KB){"esc", "cancel"};
    } else if (s->mode == MODE_SETUP) {
        kb[nkb++] = (KB){"↑↓", "move"};
        kb[nkb++] = (KB){"1-4", "shortcut"};
        kb[nkb++] = (KB){"enter", "confirm"};
    }
    
    const ColorScheme *c = TC(s);
    for (int i = 0; i < nkb; i++) {
        rb_s(c->keyname);
        rb_s(A_BOLD);
        rb_s(kb[i].k);
        rb_s(A_RST);
        rb_s("\033[38;5;242m:");
        rb_s(A_RST);
        rb_s("\033[38;5;246m");
        rb_s(kb[i].v);
        rb_s(A_RST);
        kw += utf8_width(kb[i].k) + 1 + utf8_width(kb[i].v);
        if (i < nkb - 1) { rb_s("  "); kw += 2; }
    }
    
    if (*s->status) {
        const char *sc = strncmp(s->status, "error", 5) == 0 ? "\033[38;5;167m" : "\033[38;5;179m";
        int avail = W - 1 - kw;
        if (avail >= 4) {
            char st[256];
            trunc_to(s->status, st, sizeof(st), avail - 1);
            int stw = utf8_width(st);
            int gap = avail - stw;
            if (gap > 0) rb_pad(gap);
            rb_s(sc);
            rb_s(A_BOLD);
            rb_s(st);
            rb_s(A_RST);
        }
    }
    
    fb_put(s, s->rows - 1, rb_done());
}

/* Setup mode rendering */
static void render_setup_row(AppState *s, int r, int inner, int list_top) {
    const ColorScheme *c = TC(s);
    rb_reset();
    rb_s(c->border);
    rb_s("\xe2\x94\x82" A_RST);
    
    if (r == 0) {
        const char *hd = "  \xe2\x99\xaa  Where should squidget save music?";
        rb_s(c->logo);
        rb_s(A_BOLD);
        rb_s(hd);
        rb_pad(inner - utf8_width(hd));
        rb_s(A_RST);
    } else if (r == 1) {
        const char *sh = "  use \xe2\x86\x91\xe2\x86\x93 or number keys, press enter to confirm";
        rb_s(A_DIM);
        rb_s(sh);
        rb_pad(inner - utf8_width(sh));
        rb_s(A_RST);
    } else {
        int pi = r - 2;
        if (pi >= 0 && pi < s->setup_count) {
            int sel = (pi == s->setup_cursor);
            int is_browse = (s->setup_presets[pi][0] == '\0');
            char line[256];
            if (sel) snprintf(line, sizeof(line), "  " IND " %d. %s", pi + 1, s->setup_labels[pi]);
            else snprintf(line, sizeof(line), "     %d. %s", pi + 1, s->setup_labels[pi]);
            if (sel && is_browse) {
                rb_s("\033[48;5;237m");
                rb_s("\033[38;5;255m");
                rb_s(A_BOLD);
            } else if (sel) {
                rb_s(c->qsel);
                rb_s(A_BOLD);
            } else if (is_browse) {
                rb_s(c->keyname);
            } else {
                rb_s("\033[38;5;253m");
            }
            rb_s(line);
            rb_pad(inner - utf8_width(line));
            rb_s(A_RST);
        } else {
            rb_pad(inner);
        }
    }
    
    rb_s(c->border);
    rb_s("\xe2\x94\x82" A_RST);
    fb_put(s, list_top + r, rb_done());
}

/* Quality picker rendering */
static void render_quality_row(AppState *s, int r, int inner, int list_top) {
    const ColorScheme *c = TC(s);
    rb_reset();
    rb_s(c->border);
    rb_s("\xe2\x94\x82" A_RST);
    
    if (r == 0) {
        char titl[256];
        trunc_to(s->tracks[s->cursor].title, titl, sizeof(titl), inner - 4);
        rb_s(A_DIM "  ");
        rb_s(titl);
        rb_pad(inner - 2 - utf8_width(titl));
        rb_s(A_RST);
    } else if (r == 1) {
        rb_s(A_DIM "  choose quality:" A_RST);
        rb_pad(inner - 17);
    } else if (r >= 2 && r < 2 + QUALITY_COUNT) {
        int qi = r - 2;
        int sel = (qi == s->qual_cursor);
        char ql[128];
        if (sel) snprintf(ql, sizeof(ql), "  " IND " %d. %s", qi + 1, QUALITY_LABELS[qi]);
        else snprintf(ql, sizeof(ql), "     %d. %s", qi + 1, QUALITY_LABELS[qi]);
        if (sel) {
            rb_s(c->qsel);
            rb_s(A_BOLD);
        } else {
            rb_s("\033[38;5;238m");
        }
        rb_s(ql);
        rb_pad(inner - utf8_width(ql));
        rb_s(A_RST);
    } else if (r == 2 + QUALITY_COUNT + 1) {
        const char *hint = "  enter or 1-5 to confirm   esc to cancel";
        rb_s(A_DIM);
        rb_s(hint);
        rb_pad(inner - utf8_width(hint));
        rb_s(A_RST);
    } else {
        rb_pad(inner);
    }
    
    rb_s(c->border);
    rb_s("\xe2\x94\x82" A_RST);
    fb_put(s, list_top + r, rb_done());
}

/* Album action rendering */
static void render_album_action_row(AppState *s, int r, int inner, int list_top) {
    const ColorScheme *c = TC(s);
    rb_reset();
    rb_s(c->border);
    rb_s("\xe2\x94\x82" A_RST);
    
    if (r == 0) {
        char atitl[256];
        trunc_to(s->albums[s->cursor].title, atitl, sizeof(atitl), inner - 4);
        rb_s(c->logo);
        rb_s(A_BOLD "  ");
        rb_s(atitl);
        rb_pad(inner - 2 - utf8_width(atitl));
        rb_s(A_RST);
    } else if (r == 1) {
        char art[256];
        trunc_to(s->albums[s->cursor].artist, art, sizeof(art), inner - 6);
        rb_s(A_DIM "  by ");
        rb_s(art);
        rb_pad(inner - 5 - utf8_width(art));
        rb_s(A_RST);
    } else if (r == 2) {
        rb_s(A_DIM "  choose action:" A_RST);
        rb_pad(inner - 17);
    } else if (r == 3 || r == 4) {
        int ai = r - 3;
        int sel = (ai == s->album_action_cursor);
        const char *labels[2] = {"Download complete album", "Browse individual songs"};
        char ql[128];
        if (sel) snprintf(ql, sizeof(ql), "  " IND " %d. %s", ai + 1, labels[ai]);
        else snprintf(ql, sizeof(ql), "     %d. %s", ai + 1, labels[ai]);
        if (sel) {
            rb_s(c->qsel);
            rb_s(A_BOLD);
        } else {
            rb_s("\033[38;5;238m");
        }
        rb_s(ql);
        rb_pad(inner - utf8_width(ql));
        rb_s(A_RST);
    } else if (r == 6) {
        const char *hint = "  enter or 1-2 to confirm   esc to cancel";
        rb_s(A_DIM);
        rb_s(hint);
        rb_pad(inner - utf8_width(hint));
        rb_s(A_RST);
    } else {
        rb_pad(inner);
    }
    
    rb_s(c->border);
    rb_s("\xe2\x94\x82" A_RST);
    fb_put(s, list_top + r, rb_done());
}

/* Public TUI functions */
void tui_render(AppState *s) {
    sqt_mutex_lock(&s->lock);
    if (!s->dirty) { sqt_mutex_unlock(&s->lock); return; }
    s->dirty = 0;
    
    tui_resize(s);
    fb_ensure(s);
    
    int W = s->cols;
    int H = s->rows;
    int inner = W - 2;
    int list_top = 3;
    int nlist = H - 5;
    if (nlist < 1) nlist = 1;
    
    int tw, aw;
    list_col_widths(W, &tw, &aw);
    
    render_top_border(s, W);
    render_search_bar(s, W);
    render_header(s, inner, tw, aw);
    
    /* Render list rows based on mode */
    for (int r = 0; r < nlist; r++) {
        int idx = s->scroll + r;
        int in_rng = (s->search_type == SEARCH_ALBUMS) ? (idx < s->album_count) : (idx < s->track_count);
        int is_sel = in_rng && (idx == s->cursor);
        
        if (s->mode == MODE_SETUP) {
            render_setup_row(s, r, inner, list_top);
        } else if (s->mode == MODE_QUALITY) {
            render_quality_row(s, r, inner, list_top);
        } else if (s->mode == MODE_ALBUM_ACTION) {
            render_album_action_row(s, r, inner, list_top);
        } else {
            render_list_row(s, r, idx, inner, tw, aw, is_sel, list_top);
        }
    }
    
    render_bottom_border(s, inner);
    render_status_bar(s, W);
    
    sqt_mutex_unlock(&s->lock);
    fb_flush(s);
}

void tui_resize(AppState *s) {
    int r, c;
    term_size(&r, &c);
    if (r != s->rows || c != s->cols) {
        s->rows = r;
        s->cols = c;
        fputs(A_CLR, stdout);
        fflush(stdout);
    }
}

void tui_init(AppState *s) {
    term_size(&s->rows, &s->cols);
    raw_on();
    fputs(A_ALT_ON A_CLR A_HIDE, stdout);
    fflush(stdout);
    s->dirty = 1;
}

void tui_cleanup(AppState *s) {
    (void)s;
    fputs(A_SHOW A_RST A_ALT_OFF "\n", stdout);
    fflush(stdout);
    raw_off();
}

int tui_read_key(AppState *s) {
    (void)s;
#ifndef _WIN32
    unsigned char buf[8] = {0};
    int n = (int)read(STDIN_FILENO, buf, sizeof(buf));
    if (n <= 0) return 0;
    if (n == 1) {
        if (buf[0] == 127 || buf[0] == 8) return KEY_BACKSPACE;
        if (buf[0] == '\r' || buf[0] == '\n') return KEY_ENTER;
        if (buf[0] == 27) return KEY_ESC;
        if (buf[0] == 3) return KEY_CTRL_C;
        return buf[0];
    }
    if (n >= 3 && buf[0] == 27 && buf[1] == '[') {
        switch (buf[2]) {
            case 'A': return KEY_UP;
            case 'B': return KEY_DOWN;
            case 'C': return KEY_RIGHT;
            case 'D': return KEY_LEFT;
        }
        if (buf[2] >= '0' && buf[2] <= '9' && n >= 4 && buf[3] == '~') {
            switch (buf[2]) {
                case '1': case '7': return KEY_HOME;
                case '4': case '8': return KEY_END;
                case '5': return KEY_PGUP;
                case '6': return KEY_PGDN;
            }
        }
    }
    if (n >= 2 && buf[0] == 27 && buf[1] == 'O') {
        if (n >= 3) {
            switch (buf[2]) {
                case 'A': return KEY_UP;
                case 'B': return KEY_DOWN;
                case 'C': return KEY_RIGHT;
                case 'D': return KEY_LEFT;
                case 'H': return KEY_HOME;
                case 'F': return KEY_END;
            }
        }
    }
    return buf[0];
#else
    if (!_kbhit()) { Sleep(10); return 0; }
    int c = _getch();
    if (c == 0 || c == 0xE0) {
        c = _getch();
        switch (c) {
            case 72: return KEY_UP;
            case 80: return KEY_DOWN;
            case 75: return KEY_LEFT;
            case 77: return KEY_RIGHT;
            case 71: return KEY_HOME;
            case 79: return KEY_END;
            case 73: return KEY_PGUP;
            case 81: return KEY_PGDN;
        }
        return 0;
    }
    if (c == '\r') return KEY_ENTER;
    if (c == 8) return KEY_BACKSPACE;
    if (c == 27) return KEY_ESC;
    if (c == 3) return KEY_CTRL_C;
    return c;
#endif
}