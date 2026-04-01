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
/* ENABLE_VIRTUAL_TERMINAL_PROCESSING was added in the Windows 10 SDK;
   older toolchain headers (e.g. bundled TCC) omit it. */
#  ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#    define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#  endif
static DWORD g_orig_in, g_orig_out;
#endif

/* ── ANSI ── */
#define A_RST   "\033[0m"
#define A_BOLD  "\033[1m"
#define A_DIM   "\033[2m"
#define A_HIDE  "\033[?25l"
#define A_SHOW  "\033[?25h"
#define A_CLR   "\033[2J\033[H"
#define A_ALT_ON  "\033[?1049h"   /* enter alternate screen buffer */
#define A_ALT_OFF "\033[?1049l"   /* leave  alternate screen buffer */
#define A_EOL   "\033[K"

/* ── Palette ── */
#define C_BORDER  "\033[38;5;61m"   /* muted indigo  — box borders      */
#define C_LOGO    "\033[38;5;111m"  /* sky blue      — title            */
#define C_HDR     "\033[38;5;67m"   /* steel blue    — column headers   */
#define C_TITLE   "\033[38;5;253m"  /* near-white    — track titles     */
#define C_ARTIST  "\033[38;5;246m"  /* medium gray   — artist names     */
#define C_DUR     "\033[38;5;242m"  /* dark gray     — durations        */
#define C_NUM     "\033[38;5;238m"  /* very dark     — row numbers      */
#define C_KEYNAME "\033[38;5;111m"  /* sky blue      — keybind key      */
#define C_KEYVAL  "\033[38;5;246m"  /* medium gray   — keybind action   */
#define C_KEY     "\033[38;5;242m"  /* dark gray     — keybind colon    */
#define C_SEL_BG  "\033[48;5;237m"  /* dark bg       — selected row     */
#define C_SEL_FG  "\033[38;5;255m"  /* white         — selected text    */
#define C_SEL_NUM "\033[38;5;111m"  /* sky blue      — selected rownum  */
#define C_STATUS  "\033[38;5;179m"  /* warm gold     — status text      */
#define C_ERR     "\033[38;5;167m"  /* soft red      — error text       */
#define C_PROMPT  "\033[38;5;111m"  /* sky blue      — "/" prompt       */
#define C_QUERY   "\033[38;5;253m"  /* near-white    — query text       */
#define C_CURSOR  "\033[38;5;226m"  /* yellow        — ▶ indicator      */
#define C_QSEL    "\033[38;5;111m"  /* sky blue      — quality selected */
#define C_QDIM    "\033[38;5;238m"  /* very dark     — quality dim      */
#define C_SPIN    "\033[38;5;179m"  /* warm gold     — spinner          */
#define C_MODE    "\033[38;5;67m"   /* steel blue    — mode badge       */
#define C_MODEBG  "\033[48;5;237m"  /* dark bg       — mode badge bg    */

/* ── Box-drawing ── */
#define BH  "\xe2\x94\x80"   /* ─ */
#define BV  "\xe2\x94\x82"   /* │ */
#define BTL "\xe2\x95\xad"   /* ╭ */
#define BTR "\xe2\x95\xae"   /* ╮ */
#define BBL "\xe2\x95\xb0"   /* ╰ */
#define BBR "\xe2\x95\xaf"   /* ╯ */
#define BLT "\xe2\x94\x9c"   /* ├ */
#define BRT "\xe2\x94\xa4"   /* ┤ */

static const char *SPIN[] = {"⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"};
#define NSPIN 10
#define IND "▶"

/* ── raw mode ── */
static void raw_on(void) {
#ifndef _WIN32
    tcgetattr(STDIN_FILENO, &g_orig);
    struct termios r = g_orig;
    r.c_lflag &= (tcflag_t)~(ECHO|ICANON|ISIG);
    r.c_iflag &= (tcflag_t)~(IXON|ICRNL|BRKINT|ISTRIP);
    r.c_cflag |= CS8;
    r.c_cc[VMIN] = 0; r.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &r);
#else
    HANDLE hi = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE ho = GetStdHandle(STD_OUTPUT_HANDLE);
    GetConsoleMode(hi, &g_orig_in);
    GetConsoleMode(ho, &g_orig_out);
    SetConsoleMode(hi, g_orig_in  & ~(ENABLE_LINE_INPUT|ENABLE_ECHO_INPUT|ENABLE_PROCESSED_INPUT));
    SetConsoleMode(ho, g_orig_out | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
}
static void raw_off(void) {
#ifndef _WIN32
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig);
#else
    SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE),  g_orig_in);
    SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), g_orig_out);
#endif
}

static void term_size(int *rows, int *cols) {
#ifndef _WIN32
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        { *rows = ws.ws_row; *cols = ws.ws_col; return; }
#else
    CONSOLE_SCREEN_BUFFER_INFO c;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &c)) {
        *cols = c.srWindow.Right  - c.srWindow.Left + 1;
        *rows = c.srWindow.Bottom - c.srWindow.Top  + 1;
        return;
    }
#endif
    *rows = 24; *cols = 80;
}

/* ── Unicode width helpers ── */
static int cp_width(unsigned int cp) {
    if (cp == 0 || (cp >= 0x07 && cp <= 0x0F)) return 0;
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return 0;
    if ((cp>=0x0300&&cp<=0x036F)||(cp>=0x0483&&cp<=0x0489)||
        (cp>=0x0591&&cp<=0x05BD)||(cp>=0x064B&&cp<=0x065F)||
        (cp>=0x1DC0&&cp<=0x1DFF)||(cp>=0x20D0&&cp<=0x20FF)||
        (cp>=0xFE20&&cp<=0xFE2F)) return 0;
    if (cp == 0x00AD) return 1;
    if ((cp>=0x1100&&cp<=0x115F)||(cp==0x2329)||(cp==0x232A)||
        (cp>=0x2E80&&cp<=0x303E)||(cp>=0x3041&&cp<=0x33FF)||
        (cp>=0x3400&&cp<=0x4DBF)||(cp>=0x4E00&&cp<=0xA4CF)||
        (cp>=0xAC00&&cp<=0xD7FF)||(cp>=0xF900&&cp<=0xFAFF)||
        (cp>=0xFE30&&cp<=0xFE6F)||(cp>=0xFF01&&cp<=0xFF60)||
        (cp>=0xFFE0&&cp<=0xFFE6)||(cp>=0x1F300&&cp<=0x1F64F)||
        (cp>=0x20000&&cp<=0x2FFFD)||(cp>=0x30000&&cp<=0x3FFFD)) return 2;
    return 1;
}
static unsigned int utf8_next(const char **pp) {
    const unsigned char *p = (const unsigned char *)*pp;
    unsigned int cp; int bytes;
    if      (*p < 0x80) { cp=*p;        bytes=1; }
    else if (*p < 0xC0) { cp=0xFFFD;    bytes=1; }
    else if (*p < 0xE0) { cp=*p&0x1F;   bytes=2; }
    else if (*p < 0xF0) { cp=*p&0x0F;   bytes=3; }
    else                { cp=*p&0x07;   bytes=4; }
    for (int i=1;i<bytes;i++) {
        if((p[i]&0xC0)!=0x80){cp=0xFFFD;bytes=i;break;}
        cp=(cp<<6)|(p[i]&0x3F);
    }
    *pp += bytes;
    return cp;
}
static int vw(const char *s) {
    int w=0; const char *p=s;
    while (*p) {
        if (*p=='\033') {
            p++;
            if (*p=='[')      { p++; while(*p&&(*p<0x40||*p>0x7E))p++; if(*p)p++; }
            else if (*p==']') { p++; while(*p&&*p!='\007'&&!(*p=='\033'&&p[1]=='\\'))p++; if(*p=='\007')p++; else if(*p=='\033')p+=2; }
            else if (*p)      { p++; }
            continue;
        }
        w += cp_width(utf8_next(&p));
    }
    return w;
}
static void trunc_to(const char *src, char *dst, size_t dstsz, int maxw) {
    if (maxw<=0){dst[0]='\0';return;}
    if (vw(src)<=maxw){snprintf(dst,dstsz,"%s",src);return;}
    const char *p=src; size_t i=0; int w=0;
    while(*p){
        const char *prev=p;
        unsigned int cp=utf8_next(&p);
        int cpw=cp_width(cp);
        size_t seq=(size_t)(p-prev);
        if(w+cpw>maxw-1)break;
        if(i+seq+3+1>=dstsz)break;
        memcpy(dst+i,prev,seq); i+=seq; w+=cpw;
    }
    if(i+4<dstsz){dst[i++]=(char)0xE2;dst[i++]=(char)0x80;dst[i++]=(char)0xA6;}
    dst[i]='\0';
}
static void fmt_dur(int total_s, char *b, size_t bsz) {
    int s = total_s < 0 ? 0 : total_s;
    int m = s/60, sec = s%60;
    if(m>=60) snprintf(b,bsz,"%d:%02d:%02d",(m/60)%100,m%60,sec);
    else      snprintf(b,bsz,"%d:%02d",m%1000,sec);
}

/* ── row builder ── */
static char g_row[FB_ROW_SZ];
static int  g_rp;
static void rb_reset(void){g_rp=0;g_row[0]='\0';}
static void rb_s(const char *s){
    if(!s)return;
    int rem=FB_ROW_SZ-g_rp-1; if(rem<=0)return;
    int n=snprintf(g_row+g_rp,(size_t)(rem+1),"%s",s);
    if(n>0&&n<=rem)g_rp+=n;
}
static void rb_pad(int n){while(n-->0&&g_rp<FB_ROW_SZ-1)g_row[g_rp++]=' ';g_row[g_rp]='\0';}
static void rb_rep(const char *s,int n){while(n-->0)rb_s(s);}
static const char *rb_done(void){return g_row;}

/* ── frame buffer ── */
static void fb_ensure(AppState *s){
    if(s->fb&&s->fb_rows==s->rows&&s->fb_cols==s->cols)return;
    free(s->fb);
    s->fb=calloc((size_t)s->rows,sizeof(FBRow));
    s->fb_rows=s->rows; s->fb_cols=s->cols;
    for(int i=0;i<s->rows;i++)s->fb[i].dirty=1;
}
static void fb_put(AppState *s,int row,const char *line){
    if(!s->fb||row<0||row>=s->fb_rows)return;
    FBRow *r=&s->fb[row];
    snprintf(r->cur,sizeof(r->cur),"%s",line);
    r->dirty=strcmp(r->cur,r->prev)!=0;
}
static void fb_flush(AppState *s){
    fputs(A_HIDE,stdout);
    for(int i=0;i<s->fb_rows;i++){
        FBRow *r=&s->fb[i]; if(!r->dirty)continue;
        printf("\033[%d;1H",i+1);
        fputs(r->cur,stdout); fputs(A_EOL,stdout);
        memcpy(r->prev,r->cur,sizeof(r->cur)); r->dirty=0;
    }
    fputs(A_SHOW, stdout); fflush(stdout);
}

/* ════════════════════════════════════════════════════════════
   RENDER
   Layout:
     row 0       ╭── squidget ──╮
     row 1       │ [mode] / query [spinner] │
     row 2       ├── # title  artist  dur ──┤
     rows 3..H-3 │ track rows              │
     row H-2     ╰─────────────────────────╯
     row H-1     keybinds + status
   ════════════════════════════════════════════════════════════ */

/* Column widths inside the border (W-2 usable columns).
   Columns: sp num sp ind title sp artist sp dur
   num=3  ind=2  dur=5  gaps=4   → fixed=14
   title ~55% of remainder, artist ~45%             */
#define COL_NUM 3
#define COL_IND 2   /* "▶ " or "  " */
#define COL_DUR 5
/* Note: inter-column spacing is embedded in list_col_widths() as +1 gaps */

static void list_col_widths(int W, int *tw, int *aw) {
    int inner  = W - 2;                        /* inside borders */
    int fixed  = 1 + COL_NUM + 1 + COL_IND + 1 + COL_DUR + 1; /* sp # sp ind sp dur sp */
    int rem    = inner - fixed;
    if (rem < 8) rem = 8;
    *tw = (rem * 55) / 100;
    *aw = rem - *tw;
}

void tui_render(AppState *s) {
    sqt_mutex_lock(&s->lock);
    if (!s->dirty) { sqt_mutex_unlock(&s->lock); return; }
    s->dirty = 0;

    tui_resize(s);
    fb_ensure(s);

    int W = s->cols;
    int H = s->rows;
    int inner = W - 2;                   /* columns inside │…│  */
    int list_top = 3;
    int list_bot = H - 2;
    int nlist    = list_bot - list_top;
    if (nlist < 1) nlist = 1;

    int tw, aw;
    list_col_widths(W, &tw, &aw);

    /* ── row 0: top border ╭── squidget ──╮ ── */
    {
        const char *lbl = " squidget ";
        int lblw = (int)strlen(lbl);
        int ld = (W - 2 - lblw) / 2;
        int rd = W - 2 - lblw - ld;
        if (ld < 0) ld = 0;
        if (rd < 0) rd = 0;
        rb_reset();
        rb_s(C_BORDER BTL);
        rb_rep(BH, ld);
        rb_s(A_BOLD C_LOGO); rb_s(lbl); rb_s(A_RST C_BORDER);
        rb_rep(BH, rd);
        rb_s(BTR A_RST);
        fb_put(s, 0, rb_done());
    }

    /* ── row 1: search bar ── */
    {
        rb_reset();
        rb_s(C_BORDER BV A_RST " ");
        int used = 2;

        /* mode badge */
        const char *badge =
            s->mode == MODE_SEARCH  ? " SEARCH "  :
            s->mode == MODE_RESULTS ? " RESULTS " :
            s->mode == MODE_SETUP   ? " SETUP "   :
                                      " QUALITY ";
        int bw = vw(badge);
        rb_s(C_MODEBG C_MODE A_BOLD); rb_s(badge); rb_s(A_RST " ");
        used += bw + 1;

        /* "/" prompt */
        rb_s(C_PROMPT A_BOLD "/" A_RST " ");
        used += 2;

        /* spinner */
        if (s->bg_running) {
            rb_s(C_SPIN); rb_s(SPIN[s->spin_frame % NSPIN]); rb_s(A_RST " ");
            used += 2;
        }

        /* query */
        int qspace = W - used - 2;
        if (qspace < 0) qspace = 0;
        char qbuf[512];
        trunc_to(s->query, qbuf, sizeof(qbuf), qspace);
        rb_s(C_QUERY A_BOLD); rb_s(qbuf); rb_s(A_RST);
        used += vw(qbuf);

        /* typing cursor */
        if ((s->mode == MODE_SEARCH || s->mode == MODE_RESULTS) && used < W - 2) {
            rb_s(C_PROMPT "│" A_RST); used++;
        }

        rb_pad(W - 1 - used);
        rb_s(C_BORDER BV A_RST);
        fb_put(s, 1, rb_done());
    }

    /* ── row 2: column header ├── # title artist dur ──┤ ── */
    {
        rb_reset();
        rb_s(C_BORDER BLT A_RST);
        /* build header string matching actual column positions */
        char hdr[512];
        snprintf(hdr, sizeof(hdr), " %*s  %-*s  %-*s  %s",
                 COL_NUM, "#", tw, "title", aw, "artist", "dur");
        char hdr_t[512];
        trunc_to(hdr, hdr_t, sizeof(hdr_t), inner);
        rb_s(C_HDR A_DIM); rb_s(hdr_t); rb_pad(inner - vw(hdr_t)); rb_s(A_RST);
        rb_s(C_BORDER BRT A_RST);
        fb_put(s, 2, rb_done());
    }

    /* ── rows 3..H-3: track list ── */
    for (int r = 0; r < nlist; r++) {
        int idx    = s->scroll + r;
        int in_rng = (idx < s->track_count);
        int is_sel = in_rng && (idx == s->cursor);

        rb_reset();
        rb_s(C_BORDER BV A_RST);

        if (s->mode == MODE_SETUP) {
            /* ── first-run setup overlay ── */
            int body_start = 1;   /* row offset inside list area where items start */

            if (r == 0) {
                /* title */
                const char *hd = "  \xe2\x99\xaa  Where should squidget save music?";
                rb_s(C_LOGO A_BOLD); rb_s(hd); rb_pad(inner - vw(hd)); rb_s(A_RST);

            } else if (r == body_start) {
                /* column sub-header */
                const char *sh = "  use \xe2\x86\x91\xe2\x86\x93 or number keys, press enter to confirm";
                rb_s(A_DIM); rb_s(sh); rb_pad(inner - vw(sh)); rb_s(A_RST);

            } else {
                int pi = r - body_start - 1;   /* preset index */

                if (pi >= 0 && pi < s->setup_count) {
                    int sel = (pi == s->setup_cursor);
                    int is_browse = (s->setup_presets[pi][0] == '\0');

                    char line[256];
                    if (sel) {
                        snprintf(line, sizeof(line), "  " IND " %d. %s",
                                 pi + 1, s->setup_labels[pi]);
                    } else {
                        snprintf(line, sizeof(line), "     %d. %s",
                                 pi + 1, s->setup_labels[pi]);
                    }

                    if (sel && is_browse) {
                        rb_s(C_SEL_BG C_SEL_FG A_BOLD);
                    } else if (sel) {
                        rb_s(C_QSEL A_BOLD);
                    } else if (is_browse) {
                        rb_s(C_KEYNAME);   /* browse is always highlighted slightly */
                    } else {
                        rb_s(C_TITLE);
                    }

                    rb_s(line); rb_pad(inner - vw(line)); rb_s(A_RST);

                } else {
                    rb_pad(inner);
                }
            }

        } else if (in_rng && s->mode != MODE_QUALITY) {
            if (is_sel) rb_s(C_SEL_BG);

            char nstr[8];
            { int _n = (idx+1) % 1000; snprintf(nstr, sizeof(nstr), "%3d", _n); }
            rb_s(" ");
            rb_s(is_sel ? C_SEL_NUM A_BOLD : C_NUM);
            rb_s(nstr);
            rb_s(A_RST);
            if (is_sel) rb_s(C_SEL_BG);
            rb_s(" ");

            /* indicator */
            if (is_sel) { rb_s(C_CURSOR A_BOLD IND " " A_RST C_SEL_BG); }
            else         { rb_s("  "); }

            /* title — buffer large enough for a full SQT_TITLE_SZ UTF-8 string */
            char titl[SQT_TITLE_SZ];
            trunc_to(s->tracks[idx].title, titl, sizeof(titl), tw);
            rb_s(is_sel ? C_SEL_FG A_BOLD : C_TITLE);
            rb_s(titl); rb_pad(tw - vw(titl) + 1);

            /* artist */
            char artst[SQT_TITLE_SZ];
            trunc_to(s->tracks[idx].artist, artst, sizeof(artst), aw);
            rb_s(is_sel ? C_SEL_FG : C_ARTIST);
            rb_s(artst); rb_pad(aw - vw(artst) + 1);

            /* duration */
            char dur[12];
            fmt_dur(s->tracks[idx].duration, dur, sizeof(dur));
            rb_s(is_sel ? C_SEL_FG : C_DUR);
            rb_s(dur);

            int row_used = 1 + COL_NUM + 1 + COL_IND + tw + 1 + aw + 1 + vw(dur);
            rb_pad(inner - row_used);
            rb_s(A_RST);

        } else if (s->mode == MODE_QUALITY && in_rng) {
            /* ── quality picker overlay (renders in place of track rows) ──
               First row shows the track name, then quality options.        */
            if (r == 0) {
                /* selected track context */
                char titl[SQT_TITLE_SZ];
                trunc_to(s->tracks[s->cursor].title, titl, sizeof(titl), inner - 4);
                rb_s(A_DIM "  "); rb_s(titl); rb_pad(inner - 2 - vw(titl)); rb_s(A_RST);
            } else if (r == 1) {
                rb_s(A_DIM "  choose quality:" A_RST);
                rb_pad(inner - 17);  /* "  choose quality:" = 17 columns */
            } else if (r >= 2 && r < 2 + QUALITY_COUNT) {
                int qi  = r - 2;
                int sel = (qi == s->qual_cursor);
                char ql[128];
                if (sel)
                    snprintf(ql, sizeof(ql), "  " IND " %d. %s", qi+1, QUALITY_LABELS[qi]);
                else
                    snprintf(ql, sizeof(ql), "     %d. %s", qi+1, QUALITY_LABELS[qi]);
                rb_s(sel ? C_QSEL A_BOLD : C_QDIM);
                rb_s(ql); rb_pad(inner - vw(ql));
                rb_s(A_RST);
            } else if (r == 2 + QUALITY_COUNT + 1) {
                const char *hint = "  enter or 1-5 to confirm   esc to cancel";
                rb_s(A_DIM); rb_s(hint); rb_pad(inner - vw(hint)); rb_s(A_RST);
            } else {
                rb_pad(inner);
            }
        } else {
            rb_pad(inner);
        }

        rb_s(C_BORDER BV A_RST);
        fb_put(s, list_top + r, rb_done());
    }

    /* ── row H-2: bottom border ╰──────────────╯ ── */
    {
        rb_reset();
        rb_s(C_BORDER BBL);
        rb_rep(BH, inner);
        rb_s(BBR A_RST);
        fb_put(s, H-2, rb_done());
    }

    /* ── row H-1: keybinds + right-aligned status ── */
    {
        rb_reset();
        rb_s(" ");
        int kw = 1;

        typedef struct { const char *k, *v; } KB;
        KB kb[8]; int nkb = 0;
#define K(key,val) kb[nkb].k=(key);kb[nkb].v=(val);nkb++
        if (s->mode == MODE_SEARCH || s->mode == MODE_RESULTS) {
            K("↑↓","nav"); K("enter","select"); K("/","search"); K("^C","quit");
        } else if (s->mode == MODE_QUALITY) {
            K("↑↓","move"); K("1-5","pick"); K("enter","dl"); K("esc","cancel");
        } else if (s->mode == MODE_SETUP) {
            K("↑↓","move"); K("1-4","shortcut"); K("enter","confirm");
        }
#undef K

        for (int i = 0; i < nkb; i++) {
            rb_s(C_KEYNAME A_BOLD); rb_s(kb[i].k); rb_s(A_RST);
            rb_s(C_KEY ":"); rb_s(A_RST);
            rb_s(C_KEYVAL);  rb_s(kb[i].v); rb_s(A_RST);
            kw += vw(kb[i].k) + 1 + vw(kb[i].v);
            if (i < nkb - 1) { rb_s("  "); kw += 2; }
        }

        if (*s->status) {
            const char *sc = strncmp(s->status,"error",5)==0 ? C_ERR : C_STATUS;
            int avail = W - 1 - kw;
            if (avail >= 4) {
                char st[256];
                trunc_to(s->status, st, sizeof(st), avail - 1);
                int stw = vw(st);
                int gap = avail - stw;
                if (gap > 0) rb_pad(gap);
                rb_s(sc); rb_s(A_BOLD); rb_s(st); rb_s(A_RST);
            }
        }

        fb_put(s, H-1, rb_done());
    }

    sqt_mutex_unlock(&s->lock);
    fb_flush(s);
}

void tui_resize(AppState *s) {
    int r, c; term_size(&r, &c);
    if (r != s->rows || c != s->cols) {
        s->rows = r; s->cols = c;
        if (s->fb) for (int i = 0; i < s->fb_rows; i++) s->fb[i].prev[0] = '\1';
        fputs(A_CLR, stdout); fflush(stdout);
    }
}

void tui_init(AppState *s) {
    term_size(&s->rows, &s->cols);
    raw_on();
    /* Bug 15: enter alternate screen so TUI doesn't pollute scroll-back */
    fputs(A_ALT_ON A_CLR A_HIDE, stdout); fflush(stdout);
    s->dirty = 1;
}
void tui_cleanup(AppState *s) {
    (void)s;
    /* Bug 15: leave alternate screen — restores previous terminal content */
    fputs(A_SHOW A_RST A_ALT_OFF "\n", stdout); fflush(stdout);
    raw_off();
}

int tui_read_key(AppState *s) {
    (void)s;
#ifndef _WIN32
    unsigned char buf[8] = {0};
    int n = (int)read(STDIN_FILENO, buf, sizeof(buf));
    if (n <= 0) return 0;
    if (n == 1) {
        if (buf[0]==127||buf[0]==8)        return KEY_BACKSPACE;
        if (buf[0]=='\r'||buf[0]=='\n')    return KEY_ENTER;
        if (buf[0]==27)                    return KEY_ESC;
        if (buf[0]==3)                     return KEY_CTRL_C;
        return buf[0];
    }
    if (n>=3&&buf[0]==27&&buf[1]=='[') {
        switch(buf[2]){
            case 'A': return KEY_UP;   case 'B': return KEY_DOWN;
            case 'C': return KEY_RIGHT;case 'D': return KEY_LEFT;
        }
        if (buf[2]>='0'&&buf[2]<='9'&&n>=4&&buf[3]=='~') {
            switch(buf[2]){
                case '1':case '7': return KEY_HOME;
                case '4':case '8': return KEY_END;
                case '5': return KEY_PGUP; case '6': return KEY_PGDN;
            }
        }
    }
    if (n>=2&&buf[0]==27&&buf[1]=='O') {
        if (n>=3) switch(buf[2]){
            case 'A': return KEY_UP;   case 'B': return KEY_DOWN;
            case 'C': return KEY_RIGHT;case 'D': return KEY_LEFT;
            case 'H': return KEY_HOME; case 'F': return KEY_END;
        }
    }
    return buf[0];
#else
    if (!_kbhit()) return 0;
    int c = _getch();
    if (c==0||c==0xE0) {
        c=_getch();
        switch(c){
            case 72: return KEY_UP;   case 80: return KEY_DOWN;
            case 75: return KEY_LEFT; case 77: return KEY_RIGHT;
            case 71: return KEY_HOME; case 79: return KEY_END;
            case 73: return KEY_PGUP; case 81: return KEY_PGDN;
        }
        return 0;
    }
    if(c=='\r') return KEY_ENTER;
    if(c==8)    return KEY_BACKSPACE;
    if(c==27)   return KEY_ESC;
    if(c==3)    return KEY_CTRL_C;
    return c;
#endif
}
