#include "squidget.h"
#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <stdarg.h>
#include <ctype.h>
#ifndef _WIN32
#include <strings.h>
#endif
#ifdef __APPLE__
#include <CommonCrypto/CommonKeyDerivation.h>
#endif

#if defined(_MSC_VER)
#define SQT_THREAD_LOCAL __declspec(thread)
#else
#define SQT_THREAD_LOCAL _Thread_local
#endif

#define SQT_BROWSER_UA "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/146.0.0.0 Safari/537.36"

// quality names
const char *const QUALITY_LABELS[QUALITY_COUNT] = {
    "Hi-Res Lossless",
    "Lossless",
    "High (320kbps)",
    "Low (96kbps)",
    "Dolby Atmos"
};

// http stuff: winhttp on windows, curl on linux/mac

typedef struct { char *buf; size_t len, cap; } Buf;
static char g_api_last_error[SQT_STATUS_SZ];

#define QOBUZ_ID_CACHE_MAX      256
#define QOBUZ_STREAM_CACHE_MAX  128
#define QOBUZ_STREAM_TTL_MS     (15ULL * 60ULL * 1000ULL)

typedef struct {
    char key[640];
    char id[64];
} QobuzIdCacheEntry;

typedef struct {
    char key[160];
    char url[SQT_URL_SZ];
    uint64_t stored_ms;
} QobuzStreamCacheEntry;

static QobuzIdCacheEntry     g_qobuz_id_cache[QOBUZ_ID_CACHE_MAX];
static QobuzStreamCacheEntry g_qobuz_stream_cache[QOBUZ_STREAM_CACHE_MAX];
static int                   g_qobuz_id_cache_next = 0;
static int                   g_qobuz_stream_cache_next = 0;
static sqt_mutex_t           g_qobuz_cache_lock;
static sqt_mutex_t           g_qobuz_resolver_gate;
static int                   g_qobuz_cache_ready = 0;
static uint64_t              g_qobuz_resolver_last_ms = 0;
static uint64_t              g_qobuz_resolver_cooldown_until_ms = 0;
static int                   g_qobuz_resolver_penalty_ms = 0;

const char *api_last_error(void) {
    return g_api_last_error;
}

static int sqt_debug_enabled(void) {
    const char *v = getenv("SQUIDGET_DEBUG");
    return v && v[0] && strcmp(v, "0") != 0;
}

static void api_debug(const char *fmt, ...) {
    if (!sqt_debug_enabled()) return;
    char path[1024];
    const char *custom = getenv("SQUIDGET_DEBUG_LOG");
    const char *home = getenv("HOME");
    if (custom && custom[0]) snprintf(path, sizeof path, "%s", custom);
    else if (home && home[0]) snprintf(path, sizeof path, "%s/.config/squidget/debug.log", home);
    else snprintf(path, sizeof path, "squidget-debug.log");
    FILE *f = fopen(path, "a");
    if (!f) return;
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (tm) {
        char ts[32];
        strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", tm);
        fprintf(f, "[%s] ", ts);
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static void api_set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_api_last_error, sizeof g_api_last_error, fmt, ap);
    va_end(ap);
    api_debug("error: %s", g_api_last_error);
}

static void api_clear_error(void) {
    g_api_last_error[0] = '\0';
}

static void api_cache_init(void) {
    if (!g_qobuz_cache_ready) {
        sqt_mutex_init(&g_qobuz_cache_lock);
        sqt_mutex_init(&g_qobuz_resolver_gate);
        g_qobuz_resolver_last_ms = 0;
        g_qobuz_resolver_cooldown_until_ms = 0;
        g_qobuz_resolver_penalty_ms = 0;
        g_qobuz_cache_ready = 1;
    }
}

static void api_cache_cleanup(void) {
    if (g_qobuz_cache_ready) {
        sqt_mutex_destroy(&g_qobuz_resolver_gate);
        sqt_mutex_destroy(&g_qobuz_cache_lock);
        g_qobuz_cache_ready = 0;
    }
}

static int sqt_env_int_clamped(const char *name, int defv, int minv, int maxv) {
    const char *v = getenv(name);
    if (!v || !v[0]) return defv;
    char *end = NULL;
    long n = strtol(v, &end, 10);
    if (end == v) return defv;
    if (n < minv) n = minv;
    if (n > maxv) n = maxv;
    return (int)n;
}

static int qobuz_resolver_serial_enabled(void) {
    const char *v = getenv("SQUIDGET_SERIAL_RESOLVER");
    return !(v && v[0] && strcmp(v, "0") == 0);
}

static void qobuz_resolver_gate_begin(const char *why) {
    if (!g_qobuz_cache_ready || !qobuz_resolver_serial_enabled()) return;
    sqt_mutex_lock(&g_qobuz_resolver_gate);

    int base_gap_ms = sqt_env_int_clamped("SQUIDGET_RESOLVER_GAP_MS", 250, 0, 5000);
    int gap_ms = base_gap_ms + g_qobuz_resolver_penalty_ms;
    if (gap_ms > 10000) gap_ms = 10000;

    uint64_t now = sqt_time_ms();
    uint64_t next_ok = 0;
    if (gap_ms > 0 && g_qobuz_resolver_last_ms)
        next_ok = g_qobuz_resolver_last_ms + (uint64_t)gap_ms;
    if (g_qobuz_resolver_cooldown_until_ms > next_ok)
        next_ok = g_qobuz_resolver_cooldown_until_ms;

    if (next_ok > now) {
        unsigned wait_ms = (unsigned)(next_ok - now);
        api_debug("resolver gate: waiting %ums before %s", wait_ms, why ? why : "request");
        sqt_sleep_ms(wait_ms);
    }
    g_qobuz_resolver_last_ms = sqt_time_ms();
}

static void qobuz_resolver_gate_end(void) {
    if (!g_qobuz_cache_ready || !qobuz_resolver_serial_enabled()) return;
    sqt_mutex_unlock(&g_qobuz_resolver_gate);
}

int api_qobuz_cooldown_remaining_ms(void) {
    if (!g_qobuz_cache_ready) return 0;
    uint64_t now = sqt_time_ms();
    sqt_mutex_lock(&g_qobuz_resolver_gate);
    int rem = g_qobuz_resolver_cooldown_until_ms > now
        ? (int)(g_qobuz_resolver_cooldown_until_ms - now) : 0;
    sqt_mutex_unlock(&g_qobuz_resolver_gate);
    return rem;
}

int api_qobuz_retry_delay_ms(int attempt) {
    int base = sqt_env_int_clamped("SQUIDGET_RETRY_DELAY_MS", 1500, 0, 30000);
    int maxv = sqt_env_int_clamped("SQUIDGET_RETRY_DELAY_MAX_MS", 15000, 1000, 120000);
    if (attempt < 0) attempt = 0;
    int delay = base * (attempt + 1);
    if (g_qobuz_resolver_penalty_ms > 0) delay += g_qobuz_resolver_penalty_ms;
    int cooldown = api_qobuz_cooldown_remaining_ms();
    if (cooldown > delay) delay = cooldown;
    if (delay > maxv) delay = maxv;
    return delay;
}

static int content_type_is_non_media(const char *ctype) {
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

void api_qobuz_note_download_response(long http_code, const char *content_type,
                                      long retry_after_sec, int media_ok) {
    if (!g_qobuz_cache_ready) return;
    uint64_t now = sqt_time_ms();
    sqt_mutex_lock(&g_qobuz_resolver_gate);

    if (media_ok && http_code >= 200 && http_code < 400 && !content_type_is_non_media(content_type)) {
        if (g_qobuz_resolver_penalty_ms > 0) {
            g_qobuz_resolver_penalty_ms -= 250;
            if (g_qobuz_resolver_penalty_ms < 0) g_qobuz_resolver_penalty_ms = 0;
        }
        sqt_mutex_unlock(&g_qobuz_resolver_gate);
        return;
    }

    int bump = 1500;
    if (retry_after_sec > 0) {
        uint64_t until = now + (uint64_t)retry_after_sec * 1000ULL;
        if (until > g_qobuz_resolver_cooldown_until_ms)
            g_qobuz_resolver_cooldown_until_ms = until;
        bump = (int)(retry_after_sec * 1000L);
    } else if (http_code == 429) {
        uint64_t until = now + 12000ULL;
        if (until > g_qobuz_resolver_cooldown_until_ms)
            g_qobuz_resolver_cooldown_until_ms = until;
        bump = 4000;
    } else if (http_code == 403) {
        uint64_t until = now + 20000ULL;
        if (until > g_qobuz_resolver_cooldown_until_ms)
            g_qobuz_resolver_cooldown_until_ms = until;
        bump = 6000;
    } else if (http_code >= 500 || content_type_is_non_media(content_type)) {
        bump = 2500;
    }

    if (bump < 1000) bump = 1000;
    g_qobuz_resolver_penalty_ms += bump;
    if (g_qobuz_resolver_penalty_ms > 10000) g_qobuz_resolver_penalty_ms = 10000;
    api_debug("resolver feedback: http=%ld ctype='%s' retry_after=%ld media_ok=%d penalty=%d cooldown_rem=%d",
              http_code, content_type ? content_type : "", retry_after_sec, media_ok,
              g_qobuz_resolver_penalty_ms,
              g_qobuz_resolver_cooldown_until_ms > now ? (int)(g_qobuz_resolver_cooldown_until_ms - now) : 0);
    sqt_mutex_unlock(&g_qobuz_resolver_gate);
}

static int qobuz_cache_get_id(const char *key, char *out, size_t outsz) {
    if (!key || !key[0] || !out || outsz == 0 || !g_qobuz_cache_ready) return 0;
    int ok = 0;
    sqt_mutex_lock(&g_qobuz_cache_lock);
    for (int i = 0; i < QOBUZ_ID_CACHE_MAX; i++) {
        QobuzIdCacheEntry *e = &g_qobuz_id_cache[i];
        if (e->key[0] && strcmp(e->key, key) == 0) {
            snprintf(out, outsz, "%s", e->id);
            ok = out[0] != '\0';
            break;
        }
    }
    sqt_mutex_unlock(&g_qobuz_cache_lock);
    return ok;
}

static void qobuz_cache_put_id(const char *key, const char *id) {
    if (!key || !key[0] || !id || !id[0] || !g_qobuz_cache_ready) return;
    sqt_mutex_lock(&g_qobuz_cache_lock);
    for (int i = 0; i < QOBUZ_ID_CACHE_MAX; i++) {
        QobuzIdCacheEntry *e = &g_qobuz_id_cache[i];
        if (e->key[0] && strcmp(e->key, key) == 0) {
            snprintf(e->id, sizeof e->id, "%s", id);
            sqt_mutex_unlock(&g_qobuz_cache_lock);
            return;
        }
    }
    int idx = g_qobuz_id_cache_next++ % QOBUZ_ID_CACHE_MAX;
    snprintf(g_qobuz_id_cache[idx].key, sizeof g_qobuz_id_cache[idx].key, "%s", key);
    snprintf(g_qobuz_id_cache[idx].id, sizeof g_qobuz_id_cache[idx].id, "%s", id);
    sqt_mutex_unlock(&g_qobuz_cache_lock);
}

static int qobuz_cache_get_stream(const char *key, char *out, size_t outsz) {
    if (!key || !key[0] || !out || outsz == 0 || !g_qobuz_cache_ready) return 0;
    int ok = 0;
    uint64_t now = sqt_time_ms();
    sqt_mutex_lock(&g_qobuz_cache_lock);
    for (int i = 0; i < QOBUZ_STREAM_CACHE_MAX; i++) {
        QobuzStreamCacheEntry *e = &g_qobuz_stream_cache[i];
        if (e->key[0] && strcmp(e->key, key) == 0) {
            if (now - e->stored_ms < QOBUZ_STREAM_TTL_MS) {
                snprintf(out, outsz, "%s", e->url);
                ok = out[0] != '\0';
            } else {
                e->key[0] = '\0';
                e->url[0] = '\0';
                e->stored_ms = 0;
            }
            break;
        }
    }
    sqt_mutex_unlock(&g_qobuz_cache_lock);
    return ok;
}

static void qobuz_cache_put_stream(const char *key, const char *url) {
    if (!key || !key[0] || !url || !url[0] || !g_qobuz_cache_ready) return;
    sqt_mutex_lock(&g_qobuz_cache_lock);
    for (int i = 0; i < QOBUZ_STREAM_CACHE_MAX; i++) {
        QobuzStreamCacheEntry *e = &g_qobuz_stream_cache[i];
        if (e->key[0] && strcmp(e->key, key) == 0) {
            snprintf(e->url, sizeof e->url, "%s", url);
            e->stored_ms = sqt_time_ms();
            sqt_mutex_unlock(&g_qobuz_cache_lock);
            return;
        }
    }
    int idx = g_qobuz_stream_cache_next++ % QOBUZ_STREAM_CACHE_MAX;
    snprintf(g_qobuz_stream_cache[idx].key, sizeof g_qobuz_stream_cache[idx].key, "%s", key);
    snprintf(g_qobuz_stream_cache[idx].url, sizeof g_qobuz_stream_cache[idx].url, "%s", url);
    g_qobuz_stream_cache[idx].stored_ms = sqt_time_ms();
    sqt_mutex_unlock(&g_qobuz_cache_lock);
}

void api_qobuz_invalidate_stream_url(const char *url) {
    if (!url || !url[0] || !g_qobuz_cache_ready) return;
    sqt_mutex_lock(&g_qobuz_cache_lock);
    int removed = 0;
    for (int i = 0; i < QOBUZ_STREAM_CACHE_MAX; i++) {
        QobuzStreamCacheEntry *e = &g_qobuz_stream_cache[i];
        if (e->url[0] && strcmp(e->url, url) == 0) {
            e->key[0] = '\0';
            e->url[0] = '\0';
            e->stored_ms = 0;
            removed++;
        }
    }
    sqt_mutex_unlock(&g_qobuz_cache_lock);
    if (removed) api_debug("stream cache: invalidated %d cached URL(s)", removed);
}

static int buf_ensure(Buf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return 1;
    size_t nc = (b->len + extra + 1) * 2;
    char *nb = realloc(b->buf, nc);
    if (!nb) return 0;
    b->buf = nb; b->cap = nc; return 1;
}

static char *http_post_custom(const char *url, const char *body,
                              const char *content_type,
                              const char *const *headers, int nheaders);


// url encode for query params
static void urlencode(const char *in, char *out, size_t outsz) {
    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        if ((*p>='A'&&*p<='Z')||(*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')
            ||*p=='-'||*p=='_'||*p=='.'||*p=='~') {
            if (j + 2 > outsz) break;          /* need char + null */
            out[j++] = (char)*p;
        } else {
            if (j + 4 > outsz) break;          /* need %XX + null  */
            out[j++] = '%'; out[j++] = hex[*p>>4]; out[j++] = hex[*p&0xF];
        }
    }
    out[j] = '\0';
}

static int lucida_url_p(const char *url) {
    return url && strstr(url, "lucida.to");
}

/* ── WINDOWS: WinHTTP ── */
#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>

static int wh_do(const char *url, Buf *body, FILE *fp, DWORD toms) {
    // large url buffer for stream URLs
    wchar_t wu[8192];
    if (!MultiByteToWideChar(CP_UTF8,0,url,-1,wu,(int)(sizeof(wu)/sizeof(*wu)))) return 0;
    URL_COMPONENTS uc; memset(&uc,0,sizeof uc); uc.dwStructSize=sizeof uc;
    wchar_t whost[512]={0},wpath[8192]={0};
    uc.lpszHostName=whost; uc.dwHostNameLength=512;
    uc.lpszUrlPath=wpath;  uc.dwUrlPathLength=8192;
    if (!WinHttpCrackUrl(wu,0,0,&uc)) return 0;
    if (!wpath[0]){wpath[0]=L'/';wpath[1]=0;}
    HINTERNET hs=WinHttpOpen(L"Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/146.0.0.0 Safari/537.36",WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if (!hs) return 0;
    WinHttpSetOption(hs,WINHTTP_OPTION_CONNECT_TIMEOUT,&toms,sizeof toms);
    WinHttpSetOption(hs,WINHTTP_OPTION_RECEIVE_TIMEOUT,&toms,sizeof toms);
    WinHttpSetOption(hs,WINHTTP_OPTION_SEND_TIMEOUT,   &toms,sizeof toms);
    HINTERNET hc=WinHttpConnect(hs,whost,uc.nPort,0);
    if (!hc){WinHttpCloseHandle(hs);return 0;}
    DWORD fl=(uc.nScheme==INTERNET_SCHEME_HTTPS)?WINHTTP_FLAG_SECURE:0;
    HINTERNET hr=WinHttpOpenRequest(hc,L"GET",wpath,NULL,
        WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,fl);
    if (!hr){WinHttpCloseHandle(hc);WinHttpCloseHandle(hs);return 0;}
    DWORD redir=WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hr,WINHTTP_OPTION_REDIRECT_POLICY,&redir,sizeof redir);
    int status=0;
    int write_err=0;
    if (WinHttpSendRequest(hr,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0)
        && WinHttpReceiveResponse(hr,NULL)) {
        DWORD s=0,sl=sizeof s;
        WinHttpQueryHeaders(hr,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,NULL,&s,&sl,NULL);
        status=(int)s;
        DWORD avail=0,nr=0;
        while (WinHttpQueryDataAvailable(hr,&avail)&&avail>0) {
            char chunk[16384];  /* replaced 65536 with 16KB stack-safe buffer */
            DWORD want=avail<(DWORD)sizeof chunk?avail:(DWORD)sizeof chunk;
            if (!WinHttpReadData(hr,chunk,want,&nr)||nr==0) break;
            if (fp) {
                if (fwrite(chunk,1,nr,fp) != nr) { write_err=1; break; }
            } else if (body&&buf_ensure(body,nr)){memcpy(body->buf+body->len,chunk,nr);body->len+=nr;body->buf[body->len]='\0';}
        }
    }
    WinHttpCloseHandle(hr);WinHttpCloseHandle(hc);WinHttpCloseHandle(hs);
    return write_err ? 0 : status;
}

/* WinHTTP POST — separate from wh_do to keep GET path unchanged */
static int wh_post(const char *url, const char *json_body, Buf *out_buf) {
    wchar_t wu[4096];
    if (!MultiByteToWideChar(CP_UTF8,0,url,-1,wu,(int)(sizeof(wu)/sizeof(*wu)))) return 0;
    URL_COMPONENTS uc; memset(&uc,0,sizeof uc); uc.dwStructSize=sizeof uc;
    wchar_t whost[512]={0},wpath[4096]={0};
    uc.lpszHostName=whost; uc.dwHostNameLength=512;
    uc.lpszUrlPath=wpath;  uc.dwUrlPathLength=4096;
    if (!WinHttpCrackUrl(wu,0,0,&uc)) return 0;
    if (!wpath[0]){wpath[0]=L'/';wpath[1]=0;}
    DWORD toms=15000;
    HINTERNET hs=WinHttpOpen(L"Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/146.0.0.0 Safari/537.36",WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if (!hs) return 0;
    WinHttpSetOption(hs,WINHTTP_OPTION_CONNECT_TIMEOUT,&toms,sizeof toms);
    WinHttpSetOption(hs,WINHTTP_OPTION_RECEIVE_TIMEOUT,&toms,sizeof toms);
    HINTERNET hc=WinHttpConnect(hs,whost,uc.nPort,0);
    if (!hc){WinHttpCloseHandle(hs);return 0;}
    DWORD fl=(uc.nScheme==INTERNET_SCHEME_HTTPS)?WINHTTP_FLAG_SECURE:0;
    HINTERNET hr=WinHttpOpenRequest(hc,L"POST",wpath,NULL,
        WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,fl);
    if (!hr){WinHttpCloseHandle(hc);WinHttpCloseHandle(hs);return 0;}
    DWORD redir=WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hr,WINHTTP_OPTION_REDIRECT_POLICY,&redir,sizeof redir);
    DWORD blen = json_body ? (DWORD)strlen(json_body) : 0;
    int status=0;
    if (WinHttpSendRequest(hr,
            L"Content-Type: application/json\r\n", (DWORD)-1L,
            (void*)json_body, blen, blen, 0)
        && WinHttpReceiveResponse(hr,NULL)) {
        DWORD s=0,sl=sizeof s;
        WinHttpQueryHeaders(hr,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,NULL,&s,&sl,NULL);
        status=(int)s;
        DWORD avail=0,nr=0;
        while (WinHttpQueryDataAvailable(hr,&avail)&&avail>0) {
            char chunk[16384];
            DWORD want=avail<(DWORD)sizeof chunk?avail:(DWORD)sizeof chunk;
            if (!WinHttpReadData(hr,chunk,want,&nr)||nr==0) break;
            if (out_buf&&buf_ensure(out_buf,nr)){
                memcpy(out_buf->buf+out_buf->len,chunk,nr);
                out_buf->len+=nr;
                out_buf->buf[out_buf->len]='\0';
            }
        }
    }
    WinHttpCloseHandle(hr);WinHttpCloseHandle(hc);WinHttpCloseHandle(hs);
    return status;
}

char *http_get(const char *url) {
    Buf b={malloc(4096),0,4096}; if(!b.buf) return NULL;
    char *res=NULL;
    // retry 3x with backoff — skip retry on 4xx client errors
    for (int i=0;i<3;i++) {
        b.len=0; b.buf[0]='\0';
        int s=wh_do(url,&b,NULL,15000);
        if(s>=200&&s<400){res=b.buf;break;}
        if(s>=400&&s<500) break; /* client error — retrying won't help */
        if(i<2) sqt_sleep_ms(500*(i+1));
    }
    if(!res) free(b.buf);
    return res;
}

long http_get_file(const char *url, const char *path,
                    void (*progress_cb)(size_t received, size_t total, void *ud), void *ud) {
    wchar_t wu[4096];
    if (!MultiByteToWideChar(CP_UTF8,0,url,-1,wu,(int)(sizeof(wu)/sizeof(*wu)))) return -1;
    URL_COMPONENTS uc; memset(&uc,0,sizeof uc); uc.dwStructSize=sizeof uc;
    wchar_t whost[512]={0},wpath[4096]={0};
    uc.lpszHostName=whost; uc.dwHostNameLength=512;
    uc.lpszUrlPath=wpath;  uc.dwUrlPathLength=4096;
    if (!WinHttpCrackUrl(wu,0,0,&uc)) return -1;
    if (!wpath[0]){wpath[0]=L'/';wpath[1]=0;}
    DWORD toms=60000;
    HINTERNET hs=WinHttpOpen(L"Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/146.0.0.0 Safari/537.36",WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if (!hs) return -1;
    WinHttpSetOption(hs,WINHTTP_OPTION_CONNECT_TIMEOUT,&toms,sizeof toms);
    WinHttpSetOption(hs,WINHTTP_OPTION_RECEIVE_TIMEOUT,&toms,sizeof toms);
    HINTERNET hc=WinHttpConnect(hs,whost,uc.nPort,0);
    if (!hc){WinHttpCloseHandle(hs);return -1;}
    DWORD fl=(uc.nScheme==INTERNET_SCHEME_HTTPS)?WINHTTP_FLAG_SECURE:0;
    HINTERNET hr=WinHttpOpenRequest(hc,L"GET",wpath,NULL,
        WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,fl);
    if (!hr){WinHttpCloseHandle(hc);WinHttpCloseHandle(hs);return -1;}
    DWORD redir=WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hr,WINHTTP_OPTION_REDIRECT_POLICY,&redir,sizeof redir);

    long total_sz = -1;
    if (WinHttpSendRequest(hr,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0)
        && WinHttpReceiveResponse(hr,NULL)) {
        DWORD clen=0,clenl=sizeof clen;
        if (WinHttpQueryHeaders(hr, WINHTTP_QUERY_CONTENT_LENGTH|WINHTTP_QUERY_FLAG_NUMBER,
                                NULL, &clen, &clenl, NULL)) {
            total_sz = (long)clen;
        }

        FILE *f = fopen(path, "wb");
        if (f) {
            DWORD avail=0,nr=0;
            size_t received = 0;
            while (WinHttpQueryDataAvailable(hr,&avail)&&avail>0) {
                char chunk[16384];
                DWORD want=avail<(DWORD)sizeof chunk?avail:(DWORD)sizeof chunk;
                if (!WinHttpReadData(hr,chunk,want,&nr)||nr==0) break;
                fwrite(chunk, 1, nr, f);
                received += nr;
                if (progress_cb) progress_cb(received, (size_t)total_sz, ud);
            }
            fclose(f);
        }
    }
    WinHttpCloseHandle(hr);WinHttpCloseHandle(hc);WinHttpCloseHandle(hs);
    return total_sz;
}

long http_get_file_ex(const char *url, const char *path,
                       void (*progress_cb)(size_t received, size_t total, void *ud), void *ud,
                       HttpFileInfo *info) {
    if (info) memset(info, 0, sizeof *info);
    long rc = http_get_file(url, path, progress_cb, ud);
    if (info) info->http_code = rc > 0 ? 200 : 0;
    return rc;
}

char *http_post(const char *url, const char *json_body) {
    Buf b={malloc(4096),0,4096}; if(!b.buf) return NULL;
    int s=wh_post(url,json_body,&b);
    if((s>=200&&s<400) || (s>=400 && b.len > 0)) return b.buf;
    free(b.buf); return NULL;
}

static char *http_post_custom(const char *url, const char *body,
                              const char *content_type,
                              const char *const *headers, int nheaders) {
    (void)content_type; (void)headers; (void)nheaders;
    return http_post(url, body);
}

// posix: libcurl (preferred) or curl cli fallback
#else

/* ── http_init / http_cleanup ─────────────────────────────────────────────── */

#ifdef SQT_USE_CURL
/* ── libcurl implementation ────────────────────────────────────────────────
 * POSIX builds intentionally require libcurl. No shell curl fallback is kept,
 * which avoids runtime command dependencies and shell escaping issues. */
#include <curl/curl.h>

void http_init(void) {
    curl_global_init(CURL_GLOBAL_ALL);
    api_cache_init();
}
void http_cleanup(void) {
    api_cache_cleanup();
    curl_global_cleanup();
}


typedef struct {
    void (*cb)(size_t, size_t, void *);
    void *ud;
} CurlProg;

static int curl_progress_cb(void *ud, curl_off_t dltotal, curl_off_t dlnow, 
                            curl_off_t ultotal, curl_off_t ulnow) {
    (void)ultotal; (void)ulnow;
    CurlProg *p = ud;
    if (p && p->cb) p->cb((size_t)dlnow, (size_t)dltotal, p->ud);
    return 0;
}

/* write callbacks */
static size_t curl_write_buf(char *ptr, size_t sz, size_t n, void *ud) {
    Buf *b = ud;
    size_t total = sz * n;
    if (!buf_ensure(b, total)) return 0;
    memcpy(b->buf + b->len, ptr, total);
    b->len += total;
    b->buf[b->len] = '\0';
    return total;
}
static size_t curl_write_file(char *ptr, size_t sz, size_t n, void *ud) {
    return fwrite(ptr, sz, n, (FILE *)ud);
}

/* shared easy-handle setup */
static CURL *curl_new(long timeout_s) {
    CURL *c = curl_easy_init();
    if (!c) return NULL;
    curl_easy_setopt(c, CURLOPT_USERAGENT,      SQT_BROWSER_UA);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,        timeout_s);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, timeout_s < 5L ? timeout_s : 5L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL,       1L);
    curl_easy_setopt(c, CURLOPT_IPRESOLVE,      CURL_IPRESOLVE_V4);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE,  1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS,      5L);
#ifdef CURLOPT_BUFFERSIZE
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE,     256L * 1024L);
#endif
    return c;
}

char *http_get(const char *url) {
    Buf b = {malloc(4096), 0, 4096};
    if (!b.buf) return NULL;
    char *res = NULL;
    int tries = 3;
    for (int i = 0; i < tries; i++) {
        b.len = 0; b.buf[0] = '\0';
        CURL *c = curl_new(15L);
        if (!c) break;
        curl_easy_setopt(c, CURLOPT_URL,           url);
        curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_buf);
        curl_easy_setopt(c, CURLOPT_WRITEDATA,     &b);
        CURLcode rc = curl_easy_perform(c);
        long http_code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
        curl_easy_cleanup(c);
        if (rc == CURLE_OK && http_code >= 200 && http_code < 400) { res = b.buf; break; }
        if (rc == CURLE_OK && http_code >= 400 && http_code < 500) break; /* client error */
        if (i + 1 < tries) sqt_sleep_ms(500 * (unsigned)(i + 1));
    }
    if (!res) free(b.buf);
    return res;
}

typedef struct {
    HttpFileInfo *info;
} CurlHeaderCtx;

static size_t curl_header_cb(char *buffer, size_t size, size_t nitems, void *userdata) {
    size_t total = size * nitems;
    CurlHeaderCtx *ctx = userdata;
    if (!ctx || !ctx->info || !buffer || total == 0) return total;

    if (total > 12 && strncasecmp(buffer, "Retry-After:", 12) == 0) {
        char tmp[64];
        size_t len = total - 12;
        if (len >= sizeof tmp) len = sizeof tmp - 1;
        memcpy(tmp, buffer + 12, len);
        tmp[len] = '\0';
        char *p = tmp;
        while (*p == ' ' || *p == '\t') p++;
        char *end = NULL;
        long sec = strtol(p, &end, 10);
        if (end != p && sec >= 0) ctx->info->retry_after_sec = sec;
    }
    return total;
}

long http_get_file_ex(const char *url, const char *path,
                       void (*progress_cb)(size_t received, size_t total, void *ud), void *ud,
                       HttpFileInfo *info) {
    if (info) memset(info, 0, sizeof *info);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    CURL *c = curl_new(300L);
    if (!c) { fclose(f); remove(path); return -1; }
    CurlProg cp = {progress_cb, ud};
    CurlHeaderCtx hctx = {info};
    curl_easy_setopt(c, CURLOPT_URL,           url);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_file);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,     f);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS,    0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, curl_progress_cb);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA,     &cp);
    if (info) {
        curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, curl_header_cb);
        curl_easy_setopt(c, CURLOPT_HEADERDATA, &hctx);
    }
    CURLcode rc = curl_easy_perform(c);
    long http_code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
    curl_off_t total_sz = 0;
    curl_easy_getinfo(c, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &total_sz);
    char *ctype = NULL;
    char *effective = NULL;
    curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &ctype);
    curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &effective);
    if (info) {
        info->http_code = http_code;
        if (ctype) snprintf(info->content_type, sizeof info->content_type, "%s", ctype);
        if (effective) snprintf(info->effective_url, sizeof info->effective_url, "%s", effective);
    }
    curl_easy_cleanup(c);
    long sz = (long)ftell(f);
    fclose(f);
    if (rc != CURLE_OK || http_code < 200 || http_code >= 400) {
        api_set_error("download failed: curl=%d http=%ld content_type=%s",
                      (int)rc, http_code, info ? info->content_type : "");
        remove(path);
        return -1;
    }
    api_debug("download ok: http=%ld type='%s' bytes=%ld", http_code, info ? info->content_type : "", sz);
    return (long)total_sz > 0 ? (long)total_sz : sz;
}

long http_get_file(const char *url, const char *path,
                   void (*progress_cb)(size_t received, size_t total, void *ud), void *ud) {
    return http_get_file_ex(url, path, progress_cb, ud, NULL);
}

char *http_post(const char *url, const char *json_body) {
    Buf b = {malloc(4096), 0, 4096};
    if (!b.buf) return NULL;
    CURL *c = curl_new(15L);
    if (!c) { free(b.buf); return NULL; }
    struct curl_slist *hdrs = curl_slist_append(NULL, "Content-Type: application/json");
    curl_easy_setopt(c, CURLOPT_URL,            url);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_POSTFIELDS,     json_body ? json_body : "");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER,     hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,  curl_write_buf);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,      &b);
    CURLcode rc = curl_easy_perform(c);
    long http_code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(c);
    curl_slist_free_all(hdrs);
    if (rc == CURLE_OK && ((http_code >= 200 && http_code < 400) ||
                           (http_code >= 400 && b.len > 0))) return b.buf;
    free(b.buf); return NULL;
}

static char *http_post_custom(const char *url, const char *body,
                              const char *content_type,
                              const char *const *headers, int nheaders) {
    Buf b = {malloc(4096), 0, 4096};
    if (!b.buf) return NULL;
    CURL *c = curl_new(30L);
    if (!c) { free(b.buf); return NULL; }

    struct curl_slist *hdrs = NULL;
    char ctype[256];
    if (content_type && content_type[0]) {
        snprintf(ctype, sizeof ctype, "Content-Type: %s", content_type);
        hdrs = curl_slist_append(hdrs, ctype);
    }
    for (int i = 0; i < nheaders; i++) {
        if (headers && headers[i] && headers[i][0])
            hdrs = curl_slist_append(hdrs, headers[i]);
    }

    curl_easy_setopt(c, CURLOPT_URL,            url);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_POSTFIELDS,     body ? body : "");
    if (hdrs) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,  curl_write_buf);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,      &b);
    CURLcode rc = curl_easy_perform(c);
    long http_code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(c);
    if (hdrs) curl_slist_free_all(hdrs);
    if (rc == CURLE_OK && ((http_code >= 200 && http_code < 400) ||
                           (http_code >= 400 && b.len > 0))) return b.buf;
    free(b.buf); return NULL;
}

#else  /* !SQT_USE_CURL */
#error "SquidGet POSIX builds require libcurl. Build with -DSQT_USE_CURL and link libcurl."
#endif /* SQT_USE_CURL */
#endif /* _WIN32 */

char *api_get_lyrics(const char *isrc) {
    if (!isrc || !*isrc) return NULL;
    char url[512];
    snprintf(url, sizeof url, "https://lrclib.net/api/get?isrc=%s", isrc);
    char *resp = http_get(url);
    if (!resp) return NULL;
    JNode *root = json_parse(resp); free(resp);
    if (!root) return NULL;
    
    const char *synced = jstr(jobj_get(root, "syncedLyrics"));
    char *res = NULL;
    if (synced && *synced) {
        res = strdup(synced);
    } else {
        const char *plain = jstr(jobj_get(root, "plainLyrics"));
        if (plain && *plain) res = strdup(plain);
    }
    json_free(root);
    return res;
}

// api stuff — Qobuz native

#define QBZ_API    "https://www.qobuz.com/api.json/0.2"

/* Credentials are XOR-obfuscated (key = 0xA5 ^ index) to avoid plaintext in binary.
   Generate replacements: for each char c at index i, store c ^ (0xA5 ^ i). */
static const unsigned char QBZ_OBF_ID[] = {
    0x92,0x95,0x95,0x97,0x91,0x99,0x9B,0x92,0x94
};
static const unsigned char QBZ_OBF_SEC[] = {
    0x90,0x9C,0x9E,0xC4,0xC4,0x98,0x9B,0xC7,0x99,0x99,0x9C,0x96,
    0xCD,0xC9,0xCE,0xCB,0x84,0x85,0xD1,0x83,0x81,0x89,0xD7,0x80,
    0x84,0xD9,0x8B,0xDF,0x8B,0x8B,0xD9,0x8B
};
static void sqt_deobf(const unsigned char *in, size_t len, char *out) {
    for (size_t i = 0; i < len; i++)
        out[i] = (char)(in[i] ^ (0xA5u ^ (unsigned char)i));
    out[len] = '\0';
}
/* ── cached credentials (#4) ─────────────────────────────────────────────────
 * Deobfuscation is a tight loop but runs at every call site (5×). Since the
 * values never change at runtime, compute them once and cache. */
static char s_qbz_app_id[16] = {0};
static char s_qbz_secret[36] = {0};
static int  s_qbz_creds_ready = 0;

/* fills app_id (≥10 bytes) and secret (≥33 bytes) */
static void get_qbz_creds(char *app_id, char *secret) {
    if (!s_qbz_creds_ready) {
        sqt_deobf(QBZ_OBF_ID,  sizeof(QBZ_OBF_ID),  s_qbz_app_id);
        sqt_deobf(QBZ_OBF_SEC, sizeof(QBZ_OBF_SEC), s_qbz_secret);
        s_qbz_creds_ready = 1;
    }
    memcpy(app_id, s_qbz_app_id, sizeof(QBZ_OBF_ID) + 1);
    memcpy(secret, s_qbz_secret, sizeof(QBZ_OBF_SEC) + 1);
}

/* forward declaration — defined in the Qobuz backend section below */
static void md5_hex(const char *str, char out[33]);


#define ITUNES_API_BASE "https://itunes.apple.com"

static void fill_track_itunes(JNode *t, Track *out) {
    memset(out, 0, sizeof *out);
    double track_id = jnum(jobj_get(t, "trackId"));
    if (track_id > 0) snprintf(out->id, sizeof out->id, "%.0f", track_id);

    snprintf(out->title, sizeof out->title, "%s", jstr(jobj_get(t, "trackName")));
    if (!out->title[0]) snprintf(out->title, sizeof out->title, "%s", jstr(jobj_get(t, "collectionName")));
    if (!out->title[0]) snprintf(out->title, sizeof out->title, "%s", out->id);

    snprintf(out->artist, sizeof out->artist, "%s", jstr(jobj_get(t, "artistName")));
    snprintf(out->album, sizeof out->album, "%s", jstr(jobj_get(t, "collectionName")));
    snprintf(out->cover, sizeof out->cover, "%s", jstr(jobj_get(t, "artworkUrl100")));

    const char *date = jstr(jobj_get(t, "releaseDate"));
    if (date[0]) snprintf(out->year, sizeof out->year, "%.4s", date);

    double ms = jnum(jobj_get(t, "trackTimeMillis"));
    if (ms > 0) out->duration = (int)((ms + 500.0) / 1000.0);
    out->track_num = (int)jnum(jobj_get(t, "trackNumber"));
    out->disc_num = (int)jnum(jobj_get(t, "discNumber"));
    snprintf(out->quality, sizeof out->quality, "%s", "Metadata only");
}

static void fill_album_itunes(JNode *a, Album *out) {
    memset(out, 0, sizeof *out);
    double collection_id = jnum(jobj_get(a, "collectionId"));
    if (collection_id > 0) snprintf(out->id, sizeof out->id, "%.0f", collection_id);
    snprintf(out->title, sizeof out->title, "%s", jstr(jobj_get(a, "collectionName")));
    snprintf(out->artist, sizeof out->artist, "%s", jstr(jobj_get(a, "artistName")));
    out->num_tracks = (int)jnum(jobj_get(a, "trackCount"));
}

int api_search_tracks(const char *query, Track *out, int max) {
    api_clear_error();
    if (!query || !query[0] || !out || max <= 0) return 0;

    char url[1024], qesc[512];
    urlencode(query, qesc, sizeof qesc);
    snprintf(url, sizeof url,
             "%s/search?term=%s&media=music&entity=song&limit=%d",
             ITUNES_API_BASE, qesc, max > 200 ? 200 : max);

    api_debug("iTunes search tracks query=\"%s\"", query);
    char *resp = http_get(url);
    if (!resp) return 0;

    JNode *root = json_parse(resp);
    free(resp);
    if (!root) {
        api_set_error("Track search returned invalid JSON");
        return 0;
    }

    JNode *items = jobj_get(root, "results");
    if (!items) api_debug("iTunes search tracks: results key missing");
    else if (items->type != J_ARR) api_debug("iTunes search tracks: results has unexpected type %d", (int)items->type);

    int cnt = 0;
    if (items && items->type == J_ARR) {
        int n = items->arr.len < max ? items->arr.len : max;
        for (int i = 0; i < n; i++) {
            fill_track_itunes(items->arr.items[i], &out[cnt]);
            if (out[cnt].id[0]) cnt++;
        }
    }

    api_debug("iTunes search tracks: %d result(s)", cnt);
    json_free(root);
    return cnt;
}

int api_search_albums(const char *query, Album *out, int max) {
    api_clear_error();
    if (!query || !query[0] || !out || max <= 0) return 0;

    char url[1024], qesc[512];
    urlencode(query, qesc, sizeof qesc);
    snprintf(url, sizeof url,
             "%s/search?term=%s&media=music&entity=album&limit=%d",
             ITUNES_API_BASE, qesc, max > 200 ? 200 : max);

    api_debug("iTunes search albums query=\"%s\"", query);
    char *resp = http_get(url);
    if (!resp) return 0;

    JNode *root = json_parse(resp);
    free(resp);
    if (!root) {
        api_set_error("Album search returned invalid JSON");
        return 0;
    }

    JNode *items = jobj_get(root, "results");
    if (!items) api_debug("iTunes search albums: results key missing");
    else if (items->type != J_ARR) api_debug("iTunes search albums: results has unexpected type %d", (int)items->type);

    int cnt = 0;
    if (items && items->type == J_ARR) {
        int n = items->arr.len < max ? items->arr.len : max;
        for (int i = 0; i < n; i++) {
            fill_album_itunes(items->arr.items[i], &out[cnt]);
            if (out[cnt].id[0]) cnt++;
        }
    }

    api_debug("iTunes search albums: %d result(s)", cnt);
    json_free(root);
    return cnt;
}


int api_get_album_tracks(const char *album_id, Track *out, int max) {
    if (!album_id || !album_id[0] || !out || max <= 0) return 0;

    char url[1024], id_esc[256];
    urlencode(album_id, id_esc, sizeof id_esc);
    snprintf(url, sizeof url,
             "%s/lookup?id=%s&entity=song&limit=%d",
             ITUNES_API_BASE, id_esc, max > 200 ? 200 : max);

    api_debug("iTunes get album tracks id=%s", album_id);
    char *resp = http_get(url);
    if (!resp) return 0;

    JNode *root = json_parse(resp);
    free(resp);
    if (!root) return 0;

    JNode *items = jobj_get(root, "results");
    int cnt = 0;
    if (items && items->type == J_ARR) {
        for (int i = 0; i < items->arr.len && cnt < max; i++) {
            JNode *kind = jobj_get(items->arr.items[i], "wrapperType");
            if (kind && kind->type == J_STR && strcmp(kind->s, "track") != 0) continue;
            fill_track_itunes(items->arr.items[i], &out[cnt]);
            if (out[cnt].id[0]) cnt++;
        }
    }

    json_free(root);
    return cnt;
}

static void html_unescape(char *s) {
    char *r = s, *w = s;
    while (*r) {
        if (strncmp(r, "&amp;", 5) == 0) { *w++ = '&'; r += 5; }
        else if (strncmp(r, "&quot;", 6) == 0) { *w++ = '"'; r += 6; }
        else if (strncmp(r, "&#39;", 5) == 0 || strncmp(r, "&apos;", 6) == 0) {
            *w++ = '\''; r += (r[1] == '#') ? 5 : 6;
        } else if (strncmp(r, "&lt;", 4) == 0) { *w++ = '<'; r += 4; }
        else if (strncmp(r, "&gt;", 4) == 0) { *w++ = '>'; r += 4; }
        else if (strncmp(r, "&#x", 3) == 0) {
            char *semi = strchr(r, ';');
            if (semi && semi - r < 10) {
                unsigned int cp = 0;
                for (char *p = r + 3; p < semi; p++) {
                    cp <<= 4;
                    if (*p >= '0' && *p <= '9') cp |= (unsigned int)(*p - '0');
                    else if (*p >= 'a' && *p <= 'f') cp |= (unsigned int)(*p - 'a' + 10);
                    else if (*p >= 'A' && *p <= 'F') cp |= (unsigned int)(*p - 'A' + 10);
                    else { cp = 0; break; }
                }
                if (cp > 0 && cp < 128) { *w++ = (char)cp; r = semi + 1; }
                else *w++ = *r++;
            } else *w++ = *r++;
        } else if (strncmp(r, "&#", 2) == 0) {
            char *semi = strchr(r, ';');
            if (semi && semi - r < 10) {
                unsigned int cp = 0;
                for (char *p = r + 2; p < semi; p++) {
                    if (*p < '0' || *p > '9') { cp = 0; break; }
                    cp = cp * 10u + (unsigned int)(*p - '0');
                }
                if (cp > 0 && cp < 128) { *w++ = (char)cp; r = semi + 1; }
                else *w++ = *r++;
            } else *w++ = *r++;
        }
        else *w++ = *r++;
    }
    *w = '\0';
}

static int copy_between(const char *src, const char *a, const char *b,
                        char *out, size_t outsz) {
    const char *p = strstr(src, a);
    if (!p) return 0;
    p += strlen(a);
    const char *e = strstr(p, b);
    if (!e) return 0;
    size_t n = (size_t)(e - p);
    if (n >= outsz) n = outsz - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    html_unescape(out);
    return out[0] != '\0';
}

static void jsonish_unescape(char *s) {
    char *r = s, *w = s;
    while (*r) {
        if (r[0] == '\\' && r[1]) {
            r++;
            switch (*r) {
                case 'n': *w++ = ' '; break;
                case 'r': *w++ = ' '; break;
                case 't': *w++ = ' '; break;
                case '"': *w++ = '"'; break;
                case '\\': *w++ = '\\'; break;
                case '/': *w++ = '/'; break;
                default: *w++ = *r; break;
            }
            r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

static int add_playlist_query(Playlist *pl, Track *tracks, const char *title, const char *artist, int max) {
    if (!title || !title[0] || pl->track_count >= max) return 0;
    for (int i = 0; i < pl->track_count; i++) {
        if (strcmp(tracks[i].title, title) == 0 &&
            (!artist || strcmp(tracks[i].artist, artist) == 0))
            return 0;
    }
    Track *t = &tracks[pl->track_count++];
    memset(t, 0, sizeof(*t));
    snprintf(t->title, sizeof(t->title), "%s", title);
    if (artist && artist[0]) snprintf(t->artist, sizeof(t->artist), "%s", artist);
    return 1;
}

static int extract_jsonld_playlist(const char *html, Playlist *pl, Track *tracks, int max) {
    const char *p = html;
    int added = 0;
    while ((p = strstr(p, "\"@type\"")) != NULL && pl->track_count < max) {
        const char *block_end = strstr(p, "</script>");
        if (!block_end) block_end = p + strlen(p);
        char name[SQT_TITLE_SZ] = {0};
        char artist[SQT_TITLE_SZ] = {0};
        if (copy_between(p, "\"name\":\"", "\"", name, sizeof(name)) && p < block_end) {
            jsonish_unescape(name);
            const char *artist_pos = strstr(p, "\"byArtist\"");
            if (artist_pos && artist_pos < block_end) {
                copy_between(artist_pos, "\"name\":\"", "\"", artist, sizeof(artist));
                jsonish_unescape(artist);
            }
            added += add_playlist_query(pl, tracks, name, artist, max);
        }
        p += 7;
    }
    return added;
}

static int extract_name_artist_pairs(const char *html, Playlist *pl, Track *tracks, int max) {
    const char *p = html;
    int added = 0;
    while ((p = strstr(p, "\"name\":\"")) != NULL && pl->track_count < max) {
        p += 8;
        const char *e = strchr(p, '"');
        if (!e) break;
        size_t n = (size_t)(e - p);
        if (n > 0 && n < SQT_TITLE_SZ) {
            char name[SQT_TITLE_SZ] = {0};
            memcpy(name, p, n);
            jsonish_unescape(name);
            if (strstr(name, "Spotify") == NULL && strstr(name, "Apple Music") == NULL)
                added += add_playlist_query(pl, tracks, name, "", max);
        }
        p = e + 1;
    }
    return added;
}


static int add_playlist_track_ex(Playlist *pl, Track *tracks, const char *title,
                                 const char *artist, int duration, int max) {
    int before = pl ? pl->track_count : 0;
    int added = add_playlist_query(pl, tracks, title, artist, max);
    if (added && duration > 0 && before >= 0 && before < max)
        tracks[before].duration = duration;
    return added;
}

static int parse_duration_line(const char *s) {
    if (!s || !*s) return 0;
    int nums[3] = {0, 0, 0}, nnums = 0;
    const char *p = s;
    while (*p && nnums < 3) {
        if (!isdigit((unsigned char)*p)) return 0;
        int v = 0, digits = 0;
        while (isdigit((unsigned char)*p)) {
            v = v * 10 + (*p - '0');
            p++; digits++;
            if (digits > 3) return 0;
        }
        nums[nnums++] = v;
        if (*p == ':') p++;
        else break;
    }
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '\0') return 0;
    if (nnums == 2 && nums[1] < 60) return nums[0] * 60 + nums[1];
    if (nnums == 3 && nums[1] < 60 && nums[2] < 60) return nums[0] * 3600 + nums[1] * 60 + nums[2];
    return 0;
}

static void trim_in_place(char *s) {
    if (!s) return;
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

static int line_is_noise(const char *s) {
    if (!s || !*s) return 1;
    if (parse_duration_line(s) > 0) return 1;
    int digits = 0, other = 0;
    for (const char *p = s; *p; p++) {
        if (isdigit((unsigned char)*p)) digits++;
        else if (!isspace((unsigned char)*p) && *p != '.') other++;
    }
    if (digits > 0 && other == 0) return 1;
    if (strcmp(s, "Preview") == 0 || strcmp(s, "Spotify") == 0) return 1;
    /* Spotify embed pages render the explicit-content badge as its own visible
       text line (usually just "E"). Treat that as UI chrome, not as a
       track title. Without this, explicit tracks parse as title="E" and the
       real title line is skipped. */
    if (strcmp(s, "E") == 0 || strcmp(s, "Explicit") == 0 || strcmp(s, "EXPLICIT") == 0) return 1;
    if (strstr(s, "Open Spotify") || strstr(s, "This browser") || strstr(s, "Get Spotify")) return 1;
    return 0;
}

static int spotify_playlist_id_from_url(const char *url, char *out, size_t outsz) {
    if (!url || !out || outsz == 0) return 0;
    const char *q = strstr(url, "?si=");
    (void)q;
    const char *p = strstr(url, "playlist/");
    if (p) p += 9;
    else {
        p = strstr(url, "spotify:playlist:");
        if (p) p += 17;
    }
    if (!p || !*p) return 0;
    size_t n = 0;
    while (p[n] && n + 1 < outsz) {
        unsigned char c = (unsigned char)p[n];
        if (!isalnum(c)) break;
        out[n] = (char)c;
        n++;
    }
    out[n] = '\0';
    return n > 0;
}

static void spotify_embed_url_from_playlist(const char *url, char *out, size_t outsz) {
    char id[128] = {0};
    if (spotify_playlist_id_from_url(url, id, sizeof id))
        snprintf(out, outsz, "https://open.spotify.com/embed/playlist/%s", id);
    else
        snprintf(out, outsz, "%s", url ? url : "");
}


static void normalize_unicode_dashes(char *s) {
    if (!s) return;
    char *r = s;
    char *w = s;
    while (*r) {
        unsigned char c0 = (unsigned char)r[0];
        unsigned char c1 = (unsigned char)r[1];
        unsigned char c2 = (unsigned char)r[2];
        if (c0 == 0xE2 && c1 == 0x80 && (c2 == 0x93 || c2 == 0x94)) {
            *w++ = '-';
            r += 3;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

static void collapse_spaces(char *s) {
    if (!s) return;
    char *r = s;
    char *w = s;
    int last_space = 0;
    while (*r) {
        unsigned char c = (unsigned char)*r++;
        if (isspace(c)) {
            if (!last_space) *w++ = ' ';
            last_space = 1;
        } else {
            *w++ = (char)c;
            last_space = 0;
        }
    }
    *w = '\0';
    trim_in_place(s);
}

static int spotify_title_is_generic(const char *title) {
    if (!title || !title[0]) return 1;
    char t[SQT_TITLE_SZ];
    snprintf(t, sizeof t, "%s", title);
    normalize_unicode_dashes(t);
    collapse_spaces(t);
    for (char *p = t; *p; p++) *p = (char)tolower((unsigned char)*p);
    if (strcmp(t, "spotify") == 0) return 1;
    if (strstr(t, "spotify") && strstr(t, "web player")) return 1;
    if (strstr(t, "spotify") && strstr(t, "music for everyone")) return 1;
    if (strstr(t, "spotify") && strstr(t, "preview")) return 1;
    return 0;
}

static void clean_spotify_playlist_title(char *title) {
    if (!title) return;
    html_unescape(title);
    jsonish_unescape(title);
    normalize_unicode_dashes(title);
    collapse_spaces(title);

    const char *suffixes[] = {
        " | Spotify Playlist",
        " | Spotify",
        " - Spotify Playlist",
        " - Spotify",
        " on Spotify",
        NULL
    };
    for (int i = 0; suffixes[i]; i++) {
        size_t n = strlen(title);
        size_t sn = strlen(suffixes[i]);
        if (n > sn && strcmp(title + n - sn, suffixes[i]) == 0) {
            title[n - sn] = '\0';
            collapse_spaces(title);
            break;
        }
    }
}

static int spotify_set_playlist_title_candidate(Playlist *out, const char *candidate) {
    if (!out || !candidate || !candidate[0]) return 0;
    char title[SQT_TITLE_SZ];
    snprintf(title, sizeof title, "%s", candidate);
    clean_spotify_playlist_title(title);
    if (!title[0] || spotify_title_is_generic(title)) return 0;
    snprintf(out->title, sizeof(out->title), "%s", title);
    return 1;
}

static int spotify_fetch_oembed_title(const char *url, char *out, size_t outsz) {
    if (!url || !out || outsz == 0) return 0;
    char enc[SQT_URL_SZ * 3];
    char api[SQT_URL_SZ * 4];
    urlencode(url, enc, sizeof enc);
    snprintf(api, sizeof api, "https://open.spotify.com/oembed?url=%s", enc);
    char *json = http_get(api);
    if (!json) return 0;
    char title[SQT_TITLE_SZ] = {0};
    int ok = copy_between(json, "\"title\":\"", "\"", title, sizeof title);
    free(json);
    if (!ok) return 0;
    clean_spotify_playlist_title(title);
    if (!title[0] || spotify_title_is_generic(title)) return 0;
    snprintf(out, outsz, "%s", title);
    return 1;
}

static void append_text_char(char **dst, size_t *len, size_t *cap, char c) {
    if (!dst || !*dst || !len || !cap) return;
    if (*len + 2 >= *cap) {
        size_t nc = *cap ? *cap * 2 : 4096;
        char *nb = realloc(*dst, nc);
        if (!nb) return;
        *dst = nb;
        *cap = nc;
    }
    (*dst)[(*len)++] = c;
    (*dst)[*len] = '\0';
}

static void append_newline(char **dst, size_t *len, size_t *cap) {
    if (*len == 0 || (*dst)[*len - 1] == '\n') return;
    append_text_char(dst, len, cap, '\n');
}

static char *html_to_visible_text_lines(const char *html) {
    if (!html) return NULL;
    size_t cap = 4096, len = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    out[0] = '\0';
    const char *p = html;
    int in_script = 0, in_style = 0;
    while (*p) {
        if (*p == '<') {
            const char *gt = strchr(p, '>');
            if (!gt) break;
            size_t taglen = (size_t)(gt - p + 1);
            char tag[64] = {0};
            size_t n = taglen < sizeof(tag) ? taglen : sizeof(tag) - 1;
            memcpy(tag, p, n);
            tag[n] = '\0';
            for (size_t i = 0; tag[i]; i++) tag[i] = (char)tolower((unsigned char)tag[i]);
            if (strncmp(tag, "<script", 7) == 0) in_script = 1;
            else if (strncmp(tag, "</script", 8) == 0) { in_script = 0; append_newline(&out, &len, &cap); }
            else if (strncmp(tag, "<style", 6) == 0) in_style = 1;
            else if (strncmp(tag, "</style", 7) == 0) { in_style = 0; append_newline(&out, &len, &cap); }
            else if (!in_script && !in_style) {
                if (strncmp(tag, "<br", 3) == 0 || strncmp(tag, "</div", 5) == 0 ||
                    strncmp(tag, "</li", 4) == 0 || strncmp(tag, "</p", 3) == 0 ||
                    strncmp(tag, "</span", 6) == 0 || strncmp(tag, "</a", 3) == 0 ||
                    strncmp(tag, "</h", 3) == 0 || strncmp(tag, "<li", 3) == 0)
                    append_newline(&out, &len, &cap);
            }
            p = gt + 1;
            continue;
        }
        if (!in_script && !in_style) {
            char c = *p;
            if (c == '\r' || c == '\n' || c == '\t') c = ' ';
            append_text_char(&out, &len, &cap, c);
        }
        p++;
    }
    html_unescape(out);
    return out;
}

static int extract_spotify_visible_lines(const char *html, Playlist *pl, Track *tracks, int max) {
    char *txt = html_to_visible_text_lines(html);
    if (!txt) return 0;

    char *lines[2048];
    int nlines = 0;
    char *save = NULL;
    for (char *line = strtok_r(txt, "\n", &save); line && nlines < (int)(sizeof(lines) / sizeof(lines[0])); line = strtok_r(NULL, "\n", &save)) {
        trim_in_place(line);
        if (!line[0]) continue;
        if (line_is_noise(line) && parse_duration_line(line) <= 0) continue;
        lines[nlines++] = line;
    }

    int added = 0;
    for (int i = 0; i < nlines && pl->track_count < max; i++) {
        int dur = parse_duration_line(lines[i]);
        if (dur <= 0) continue;
        int ai = i - 1;
        while (ai >= 0 && line_is_noise(lines[ai])) ai--;
        int ti = ai - 1;
        while (ti >= 0 && line_is_noise(lines[ti])) ti--;
        if (ti < 0 || ai < 0) continue;
        char title[SQT_TITLE_SZ];
        char artist[SQT_TITLE_SZ];
        snprintf(title, sizeof title, "%s", lines[ti]);
        snprintf(artist, sizeof artist, "%s", lines[ai]);
        if (strncmp(artist, "E ", 2) == 0 && artist[2]) memmove(artist, artist + 2, strlen(artist + 2) + 1);
        trim_in_place(title);
        trim_in_place(artist);
        if (title[0] && artist[0]) added += add_playlist_track_ex(pl, tracks, title, artist, dur, max);
    }
    free(txt);
    return added;
}

static int extract_spotify_track_rows(const char *html, Playlist *pl, Track *tracks, int max);

static int fetch_spotify_embed_playlist(const char *url, Playlist *out, Track *tracks, int max) {
    char embed[SQT_URL_SZ];
    spotify_embed_url_from_playlist(url, embed, sizeof embed);
    if (!embed[0] || strcmp(embed, url) == 0) return 0;
    api_debug("Spotify playlist fallback: fetching embed page %s", embed);
    char *html = http_get(embed);
    if (!html) return 0;
    char title[SQT_TITLE_SZ] = {0};
    int have_good_title = 0;
    if (copy_between(html, "<meta property=\"og:title\" content=\"", "\"", title, sizeof(title)) ||
        copy_between(html, "<title>", "</title>", title, sizeof(title))) {
        have_good_title = spotify_set_playlist_title_candidate(out, title);
    }
    if (!have_good_title || spotify_title_is_generic(out->title)) {
        char oe_title[SQT_TITLE_SZ] = {0};
        if (spotify_fetch_oembed_title(url, oe_title, sizeof oe_title))
            spotify_set_playlist_title_candidate(out, oe_title);
    }
    int n = extract_spotify_track_rows(html, out, tracks, max);
    free(html);
    return n;
}

static int extract_spotify_track_rows(const char *html, Playlist *pl, Track *tracks, int max) {
    const char *p = html;
    int added = 0;
    int skipped_explicit_badge = 0;
    while ((p = strstr(p, "data-testid=\"track-row\"")) != NULL && pl->track_count < max) {
        const char *row_start = p;
        const char *prev = row_start;
        for (int i = 0; i < 5000 && prev > html; i++, prev--) {
            if (strncmp(prev, "role=\"group\"", 12) == 0) {
                row_start = prev;
                break;
            }
        }

        const char *aria = strstr(row_start, "aria-label=\"");
        if (aria && aria < p) {
            aria += 12;
            const char *end = strchr(aria, '"');
            if (end && end > aria) {
                char title[SQT_TITLE_SZ];
                char artist[SQT_TITLE_SZ] = {0};
                size_t n = (size_t)(end - aria);
                if (n >= sizeof(title)) n = sizeof(title) - 1;
                memcpy(title, aria, n);
                title[n] = '\0';
                html_unescape(title);

                const char *artist_link = strstr(p, "data-testid=\"internal-artist-link\"");
                const char *next_row = strstr(p + 1, "data-testid=\"track-row\"");
                if (artist_link && (!next_row || artist_link < next_row)) {
                    const char *a_tag = strstr(artist_link, "<a ");
                    const char *gt = a_tag ? strchr(a_tag, '>') : NULL;
                    const char *ae = gt ? strstr(gt + 1, "</a>") : NULL;
                    if (gt && ae && (!next_row || ae < next_row)) {
                        size_t an = (size_t)(ae - (gt + 1));
                        if (an >= sizeof(artist)) an = sizeof(artist) - 1;
                        memcpy(artist, gt + 1, an);
                        artist[an] = '\0';
                        html_unescape(artist);
                    }
                }
                trim_in_place(title);
                trim_in_place(artist);
                /* Avoid accepting Spotify's explicit badge aria-label as a
                   song title. If this row parser only found the badge, skip it
                   and let the visible-line fallback recover title/artist/duration. */
                if (strcmp(title, "E") != 0 && strcmp(title, "Explicit") != 0 &&
                    strcmp(title, "EXPLICIT") != 0) {
                    added += add_playlist_query(pl, tracks, title, artist, max);
                } else {
                    skipped_explicit_badge++;
                }
            }
        }
        p += 24;
    }
    if (added == 0 || skipped_explicit_badge > 0)
        added += extract_spotify_visible_lines(html, pl, tracks, max);
    return added;
}

int api_fetch_playlist(const char *url, Playlist *out, Track *tracks, int max) {
    if (!url || !url[0] || !out || !tracks || max <= 0) return 0;
    memset(out, 0, sizeof(*out));
    memset(tracks, 0, (size_t)max * sizeof(Track));
    snprintf(out->url, sizeof(out->url), "%s", url);
    snprintf(out->title, sizeof(out->title), "%s", url);

    char *html = http_get(url);
    if (!html) return 0;

    char title[SQT_TITLE_SZ] = {0};
    int have_good_title = 0;
    if (copy_between(html, "<meta property=\"og:title\" content=\"", "\"", title, sizeof(title)) ||
        copy_between(html, "<title>", "</title>", title, sizeof(title))) {
        have_good_title = spotify_set_playlist_title_candidate(out, title);
    }

    int is_spotify = strstr(url, "open.spotify.com") != NULL || strstr(html, "Spotify") != NULL;
    if (is_spotify && (!have_good_title || spotify_title_is_generic(out->title))) {
        char oe_title[SQT_TITLE_SZ] = {0};
        if (spotify_fetch_oembed_title(url, oe_title, sizeof oe_title))
            spotify_set_playlist_title_candidate(out, oe_title);
    }

    int added = 0;
    if (is_spotify) added = extract_spotify_track_rows(html, out, tracks, max);
    if (added == 0) added = extract_jsonld_playlist(html, out, tracks, max);
    if (added == 0 && !is_spotify) added = extract_name_artist_pairs(html, out, tracks, max);
    free(html);

    if (added == 0 && is_spotify) {
        /* Spotify's normal web-player HTML often omits track rows for non-browser
           clients. The public embed page still exposes the preview track list, so
           use it as a no-auth fallback instead of showing a useless 0-track item. */
        added = fetch_spotify_embed_playlist(url, out, tracks, max);
    }
    api_debug("playlist fetch: url=%s spotify=%d tracks=%d title='%s'", url, is_spotify, added, out->title);
    return added;
}

/* Fetch full track metadata from the iTunes lookup endpoint.
   Merges into *out — fields already set are preserved if the API returns empty. */
int api_get_track_info(const char *track_id, Track *out) {
    if (!track_id || !track_id[0] || !out) return 0;
    char url[1024], id_esc[256];
    urlencode(track_id, id_esc, sizeof id_esc);
    snprintf(url, sizeof url, "%s/lookup?id=%s", ITUNES_API_BASE, id_esc);

    char *resp = http_get(url);
    if (!resp) return 0;
    JNode *root = json_parse(resp);
    free(resp);
    if (!root) return 0;

    Track fresh;
    memset(&fresh, 0, sizeof fresh);
    JNode *items = jobj_get(root, "results");
    if (items && items->type == J_ARR && items->arr.len > 0) {
        fill_track_itunes(items->arr.items[0], &fresh);
    }

#define MERGE_STR(f) do { if (fresh.f[0]) snprintf(out->f, sizeof out->f, "%s", fresh.f); } while(0)
#define MERGE_INT(f) do { if (fresh.f)    out->f = fresh.f; } while(0)
    MERGE_STR(title);  MERGE_STR(artist); MERGE_STR(album);
    MERGE_STR(cover);  MERGE_STR(year);   MERGE_STR(isrc);
    MERGE_STR(copyright); MERGE_STR(quality);
    MERGE_INT(track_num); MERGE_INT(disc_num);
    MERGE_INT(duration);  MERGE_INT(explicit_);
#undef MERGE_STR
#undef MERGE_INT

    json_free(root);
    return 1;
}


/* ── Qobuz backend ───────────────────────────────────────────────────────── */

#define SPOTIFLAC_UA "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/146.0.0.0 Safari/537.36"
#define MUSICDL_QOBUZ_URL "https://www.musicdl.me/api/qobuz/download"
#define GDSTUDIO_XYZ_URL  "https://music.gdstudio.xyz/api.php"
#define GDSTUDIO_ORG_URL  "https://music.gdstudio.org/api.php"
#define GDSTUDIO_VERSION  "2026.5.10"

/* ── tiny self-contained MD5 (public domain, RSA Data Security) ── */
typedef struct { unsigned int s[4], c[2]; unsigned char b[64]; } MD5_CTX_;
static void md5_init_(MD5_CTX_ *c){
    c->s[0]=0x67452301;c->s[1]=0xefcdab89;c->s[2]=0x98badcfe;c->s[3]=0x10325476;
    c->c[0]=c->c[1]=0;
}
#define MD5_F(x,y,z) ((x&y)|(~x&z))
#define MD5_G(x,y,z) ((x&z)|(y&~z))
#define MD5_H(x,y,z) (x^y^z)
#define MD5_I(x,y,z) (y^(x|~z))
#define MD5_ROL(x,n) (((x)<<(n))|((x)>>(32-(n))))
#define MD5_FF(a,b,c,d,x,s,ac) a=MD5_ROL(a+MD5_F(b,c,d)+x+ac,s)+b
#define MD5_GG(a,b,c,d,x,s,ac) a=MD5_ROL(a+MD5_G(b,c,d)+x+ac,s)+b
#define MD5_HH(a,b,c,d,x,s,ac) a=MD5_ROL(a+MD5_H(b,c,d)+x+ac,s)+b
#define MD5_II(a,b,c,d,x,s,ac) a=MD5_ROL(a+MD5_I(b,c,d)+x+ac,s)+b
static void md5_transform_(unsigned int s[4], const unsigned char blk[64]){
    unsigned int a=s[0],b=s[1],c=s[2],d=s[3],x[16];
    for(int i=0;i<16;i++) x[i]=((unsigned int)blk[i*4])|((unsigned int)blk[i*4+1]<<8)|((unsigned int)blk[i*4+2]<<16)|((unsigned int)blk[i*4+3]<<24);
    MD5_FF(a,b,c,d,x[ 0], 7,0xd76aa478);MD5_FF(d,a,b,c,x[ 1],12,0xe8c7b756);MD5_FF(c,d,a,b,x[ 2],17,0x242070db);MD5_FF(b,c,d,a,x[ 3],22,0xc1bdceee);
    MD5_FF(a,b,c,d,x[ 4], 7,0xf57c0faf);MD5_FF(d,a,b,c,x[ 5],12,0x4787c62a);MD5_FF(c,d,a,b,x[ 6],17,0xa8304613);MD5_FF(b,c,d,a,x[ 7],22,0xfd469501);
    MD5_FF(a,b,c,d,x[ 8], 7,0x698098d8);MD5_FF(d,a,b,c,x[ 9],12,0x8b44f7af);MD5_FF(c,d,a,b,x[10],17,0xffff5bb1);MD5_FF(b,c,d,a,x[11],22,0x895cd7be);
    MD5_FF(a,b,c,d,x[12], 7,0x6b901122);MD5_FF(d,a,b,c,x[13],12,0xfd987193);MD5_FF(c,d,a,b,x[14],17,0xa679438e);MD5_FF(b,c,d,a,x[15],22,0x49b40821);
    MD5_GG(a,b,c,d,x[ 1], 5,0xf61e2562);MD5_GG(d,a,b,c,x[ 6], 9,0xc040b340);MD5_GG(c,d,a,b,x[11],14,0x265e5a51);MD5_GG(b,c,d,a,x[ 0],20,0xe9b6c7aa);
    MD5_GG(a,b,c,d,x[ 5], 5,0xd62f105d);MD5_GG(d,a,b,c,x[10], 9,0x02441453);MD5_GG(c,d,a,b,x[15],14,0xd8a1e681);MD5_GG(b,c,d,a,x[ 4],20,0xe7d3fbc8);
    MD5_GG(a,b,c,d,x[ 9], 5,0x21e1cde6);MD5_GG(d,a,b,c,x[14], 9,0xc33707d6);MD5_GG(c,d,a,b,x[ 3],14,0xf4d50d87);MD5_GG(b,c,d,a,x[ 8],20,0x455a14ed);
    MD5_GG(a,b,c,d,x[13], 5,0xa9e3e905);MD5_GG(d,a,b,c,x[ 2], 9,0xfcefa3f8);MD5_GG(c,d,a,b,x[ 7],14,0x676f02d9);MD5_GG(b,c,d,a,x[12],20,0x8d2a4c8a);
    MD5_HH(a,b,c,d,x[ 5], 4,0xfffa3942);MD5_HH(d,a,b,c,x[ 8],11,0x8771f681);MD5_HH(c,d,a,b,x[11],16,0x6d9d6122);MD5_HH(b,c,d,a,x[14],23,0xfde5380c);
    MD5_HH(a,b,c,d,x[ 1], 4,0xa4beea44);MD5_HH(d,a,b,c,x[ 4],11,0x4bdecfa9);MD5_HH(c,d,a,b,x[ 7],16,0xf6bb4b60);MD5_HH(b,c,d,a,x[10],23,0xbebfbc70);
    MD5_HH(a,b,c,d,x[13], 4,0x289b7ec6);MD5_HH(d,a,b,c,x[ 0],11,0xeaa127fa);MD5_HH(c,d,a,b,x[ 3],16,0xd4ef3085);MD5_HH(b,c,d,a,x[ 6],23,0x04881d05);
    MD5_HH(a,b,c,d,x[ 9], 4,0xd9d4d039);MD5_HH(d,a,b,c,x[12],11,0xe6db99e5);MD5_HH(c,d,a,b,x[15],16,0x1fa27cf8);MD5_HH(b,c,d,a,x[ 2],23,0xc4ac5665);
    MD5_II(a,b,c,d,x[ 0], 6,0xf4292244);MD5_II(d,a,b,c,x[ 7],10,0x432aff97);MD5_II(c,d,a,b,x[14],15,0xab9423a7);MD5_II(b,c,d,a,x[ 5],21,0xfc93a039);
    MD5_II(a,b,c,d,x[12], 6,0x655b59c3);MD5_II(d,a,b,c,x[ 3],10,0x8f0ccc92);MD5_II(c,d,a,b,x[10],15,0xffeff47d);MD5_II(b,c,d,a,x[ 1],21,0x85845dd1);
    MD5_II(a,b,c,d,x[ 8], 6,0x6fa87e4f);MD5_II(d,a,b,c,x[15],10,0xfe2ce6e0);MD5_II(c,d,a,b,x[ 6],15,0xa3014314);MD5_II(b,c,d,a,x[13],21,0x4e0811a1);
    MD5_II(a,b,c,d,x[ 4], 6,0xf7537e82);MD5_II(d,a,b,c,x[11],10,0xbd3af235);MD5_II(c,d,a,b,x[ 2],15,0x2ad7d2bb);MD5_II(b,c,d,a,x[ 9],21,0xeb86d391);
    s[0]+=a;s[1]+=b;s[2]+=c;s[3]+=d;
}
static void md5_update_(MD5_CTX_ *c, const unsigned char *in, size_t len){
    unsigned int idx=(c->c[0]>>3)&0x3f;
    if((c->c[0]+=(unsigned int)(len<<3))<(unsigned int)(len<<3)) c->c[1]++;
    c->c[1]+=(unsigned int)(len>>29);
    unsigned int part=64-idx;
    size_t i;
    if(len>=part){memcpy(&c->b[idx],in,part);md5_transform_(c->s,c->b);for(i=part;i+63<len;i+=64)md5_transform_(c->s,in+i);idx=0;}else i=0;
    memcpy(&c->b[idx],in+i,len-i);
}
static void md5_final_(unsigned char digest[16], MD5_CTX_ *c){
    static const unsigned char pad[64]={0x80};
    unsigned char cnt[8];
    for(int i=0;i<4;i++){cnt[i]=(unsigned char)(c->c[0]>>(i*8));cnt[i+4]=(unsigned char)(c->c[1]>>(i*8));}
    unsigned int idx=(c->c[0]>>3)&0x3f;
    unsigned int plen=(idx<56)?(56-idx):(120-idx);
    md5_update_(c,pad,plen);md5_update_(c,cnt,8);
    for(int i=0;i<4;i++){digest[i*4]=(unsigned char)(c->s[i]);digest[i*4+1]=(unsigned char)(c->s[i]>>8);digest[i*4+2]=(unsigned char)(c->s[i]>>16);digest[i*4+3]=(unsigned char)(c->s[i]>>24);}
    memset(c,0,sizeof(*c));
}
#undef MD5_F
#undef MD5_G
#undef MD5_H
#undef MD5_I
#undef MD5_ROL
#undef MD5_FF
#undef MD5_GG
#undef MD5_HH
#undef MD5_II

/* md5_hex: compute lowercase hex MD5 of str, write 33-byte result into out */
static void md5_hex(const char *str, char out[33]) {
    MD5_CTX_ c; md5_init_(&c);
    md5_update_(&c, (const unsigned char *)str, strlen(str));
    unsigned char dig[16]; md5_final_(dig, &c);
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) { out[i*2] = h[dig[i]>>4]; out[i*2+1] = h[dig[i]&0xf]; }
    out[32] = '\0';
}

/*
 * api_qobuz_get_stream_url
 *
 * Resolves an ISRC to a Qobuz direct download URL via:
 *   1. Signed Qobuz track/search (ISRC → Qobuz track ID)
 *   2. POST to the configured download resolver
 *
 * Returns 1 and fills out_url on success; 0 on any failure.
 */
static const char *zarz_quality_for_label(const char *quality) {
    if (!quality || !quality[0]) return "hi-res-max";
    if (strcmp(quality, QUALITY_LABELS[0]) == 0 || strcmp(quality, QUAL_HIR) == 0)
        return "hi-res-max";
    if (strcmp(quality, QUALITY_LABELS[1]) == 0 || strcmp(quality, QUAL_LOS) == 0)
        return "lossless";
    if (strcmp(quality, QUALITY_LABELS[2]) == 0 || strcmp(quality, QUAL_HIGH) == 0)
        return "high";
    if (strcmp(quality, QUALITY_LABELS[3]) == 0 || strcmp(quality, QUAL_LOW) == 0)
        return "low";
    if (strcmp(quality, QUALITY_LABELS[4]) == 0 || strcmp(quality, QUAL_ATM) == 0)
        return "dolby-atmos";
    return quality;
}

static const char *const *quality_fallbacks(const char *requested, int *count) {
    static const char *hir[]  = {"hi-res-max", "lossless", "high", "low"};
    static const char *los[]  = {"lossless", "high", "low"};
    static const char *high[] = {"high", "lossless", "low"};
    static const char *low[]  = {"low", "high"};
    static const char *atm[]  = {"dolby-atmos", "hi-res-max", "lossless", "high", "low"};

    if (strcmp(requested, "lossless") == 0) { *count = 3; return los; }
    if (strcmp(requested, "high") == 0)     { *count = 3; return high; }
    if (strcmp(requested, "low") == 0)      { *count = 2; return low; }
    if (strcmp(requested, "dolby-atmos") == 0) { *count = 5; return atm; }
    *count = 4;
    return hir;
}

static void qobuz_stream_err(char *err, size_t errsz, const char *fmt, ...) {
    if (!err || errsz == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errsz, fmt, ap);
    va_end(ap);
}

static const char *qobuz_resolver_url(char *err, size_t errsz) {
    const char *env = getenv("SQUIDGET_QOBUZ_RESOLVER_URL");
    if (env && env[0]) return env;
    qobuz_stream_err(err, errsz, "no custom Qobuz resolver configured");
    return NULL;
}

static void qobuz_spotiflac_quality(const char *quality, char *out, size_t outsz) {
    const char *q = "27";
    if (quality && (strcmp(quality, QUALITY_LABELS[1]) == 0 || strcmp(quality, QUAL_LOS) == 0))
        q = "6";
    else if (quality && (strcmp(quality, QUALITY_LABELS[2]) == 0 || strcmp(quality, QUAL_HIGH) == 0))
        q = "5";
    else if (quality && (strcmp(quality, QUALITY_LABELS[3]) == 0 || strcmp(quality, QUAL_LOW) == 0))
        q = "5";
    snprintf(out, outsz, "%s", q);
}

static const char *const *spotiflac_quality_fallbacks(const char *requested, int *count) {
    static const char *hires[] = {"27", "7", "6"};
    static const char *std[]   = {"6"};
    static const char *mp3[]   = {"5"};
    if (strcmp(requested, "6") == 0) { *count = 1; return std; }
    if (strcmp(requested, "5") == 0) { *count = 1; return mp3; }
    *count = 3;
    return hires;
}

static int extract_qobuz_stream_url(const char *body, char *out, size_t outsz);

static int qobuz_url_looks_streamable(const char *raw) {
    return raw && (strncmp(raw, "https://", 8) == 0 || strncmp(raw, "http://", 7) == 0);
}

static int copy_qobuz_json_url(JNode *root, char *out, size_t outsz) {
    if (!root || root->type != J_OBJ) return 0;
    const char *keys[] = {"download_url", "url", "play_url", "stream_url", "link", "file"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        const char *v = jstr(jobj_get(root, keys[i]));
        if (qobuz_url_looks_streamable(v)) {
            snprintf(out, outsz, "%s", v);
            return 1;
        }
    }

    JNode *data = jobj_get(root, "data");
    if (data && data->type == J_OBJ && copy_qobuz_json_url(data, out, outsz))
        return 1;
    return 0;
}

static void sqt_deobf_xor(const unsigned char *obf, size_t n, char *out, size_t outsz) {
    if (!out || outsz == 0) return;
    size_t len = n;
    if (len + 1 > outsz) len = outsz - 1;
    for (size_t i = 0; i < len; i++)
        out[i] = (char)(obf[i] ^ (0xA5u ^ (unsigned char)i));
    out[len] = '\0';
}

static const char *qobuz_community_quality_label(const char *quality) {
    const char *q = zarz_quality_for_label(quality);
    if (strcmp(q, "hi-res-max") == 0 || strcmp(q, "dolby-atmos") == 0 ||
        strcmp(q, "27") == 0 || strcmp(q, "7") == 0)
        return "24";
    return "16";
}

static int qobuz_community_disabled(void) {
    const char *v = getenv("SQUIDGET_DISABLE_COMMUNITY_QOBUZ");
    return v && (strcmp(v, "1") == 0 || strcmp(v, "true") == 0 ||
                 strcmp(v, "TRUE") == 0 || strcmp(v, "yes") == 0 ||
                 strcmp(v, "YES") == 0);
}

static int qobuz_community_supports_quality(const char *quality) {
    (void)quality;
    /* Accept-any mode: always try the public resolver. It only has lossless-ish
       outputs, but the downloader now saves whatever file/container comes back. */
    return 1;
}

static int qobuz_community_stream_url_once(const char *qobuz_id, const char *quality_tag,
                                           char *out, size_t outsz,
                                           char *err, size_t errsz) {
    static const unsigned char url_obf[] = {
        0xcd,0xd0,0xd3,0xd6,0xd2,0x9a,0x8c,0x8d,0xdc,0xce,0xd5,0x83,0xcf,0xc7,0xd8,0xd9,
        0x9b,0xc7,0xc7,0xd9,0xc5,0xd2,0xca,0xd7,0x93,0xcd,0xc5,0xc4,0x97,0xd1,0xd4,0x95,
        0xe4,0xf4,0xee,0xa9,0xe5,0xec
    };
    static const unsigned char key_obf[] = {
        0xc0,0xdc,0xd7,0xca,0xce,0xd2,0xc6,0x8f,0xc2,0xce,0xdc,0xcd,0xdc,0xda,0xce,0x87,
        0xd6,0xdc,0xde,0xc0,0xd0,0xdc,0xc1,0xcb,0x90,0xc8,0xcd,0xdf,0xcf,0xdd,0xc8,0xce,
        0xfc,0xa9,0xe5,0xea,0xe8,0xee,0xe8,0xf1
    };

    char api_url[128], api_key[64];
    sqt_deobf_xor(url_obf, sizeof url_obf, api_url, sizeof api_url);
    sqt_deobf_xor(key_obf, sizeof key_obf, api_key, sizeof api_key);

    char body[256];
    snprintf(body, sizeof body,
             "{\"id\":\"%s\",\"quality\":\"%s\"}",
             qobuz_id, quality_tag);

    char key_hdr[96];
    snprintf(key_hdr, sizeof key_hdr, "x-api-key: %s", api_key);
    const char *headers[] = {
        "User-Agent: SquidGet/1.0",
        "Accept: application/json",
        key_hdr
    };

    char *resp = http_post_custom(api_url, body, "application/json",
                                  headers, (int)(sizeof headers / sizeof headers[0]));
    if (!resp) {
        qobuz_stream_err(err, errsz, "Fast resolver did not respond");
        return 0;
    }

    if (extract_qobuz_stream_url(resp, out, outsz)) {
        free(resp);
        return 1;
    }

    JNode *root = json_parse(resp);
    if (root) {
        const char *detail = jstr(jobj_get(root, "error"));
        if (detail[0])
            qobuz_stream_err(err, errsz, "Fast resolver failed: %s", detail);
        json_free(root);
    }
    if (err && errsz && !err[0])
        qobuz_stream_err(err, errsz, "Fast resolver response had no stream URL");
    free(resp);
    return 0;
}

static int qobuz_community_stream_url(const char *qobuz_id, const char *quality,
                                      char *out, size_t outsz,
                                      char *err, size_t errsz) {
    const char *preferred = qobuz_community_quality_label(quality);
    const char *alts[3];
    int n = 0;

    /* Try the requested public quality first, then the other public flavor.
       This removes the old false failure where selecting High/Low/Atmos skipped
       the public resolver even though a valid FLAC/M4A/other stream could be
       returned and saved as-is. */
    alts[n++] = preferred;
    if (strcmp(preferred, "24") != 0) alts[n++] = "24";
    if (strcmp(preferred, "16") != 0) alts[n++] = "16";

    char last_err[160] = {0};
    for (int pass = 0; pass < 3; pass++) {
        for (int i = 0; i < n; i++) {
            char tmp_err[160] = {0};
            if (qobuz_community_stream_url_once(qobuz_id, alts[i], out, outsz,
                                                tmp_err, sizeof tmp_err)) {
                if (err && errsz) err[0] = '\0';
                return 1;
            }
            if (tmp_err[0]) snprintf(last_err, sizeof last_err, "%s", tmp_err);
        }
        if (pass + 1 < 3) sqt_sleep_ms(350u * (unsigned)(pass + 1));
    }

    if (err && errsz) {
        if (last_err[0]) snprintf(err, errsz, "%s", last_err);
        else qobuz_stream_err(err, errsz, "Fast resolver did not return a stream URL");
    }
    return 0;
}

static int extract_qobuz_stream_url(const char *body, char *out, size_t outsz) {
    if (!body || !body[0]) return 0;

    const char *parse_body = body;
    char jsonp_buf[8192];
    const char *open = strchr(body, '(');
    const char *close = open ? strrchr(open, ')') : NULL;
    if (open && close && close > open + 1 &&
        (size_t)(close - open - 1) + 1 < sizeof jsonp_buf) {
        size_t inner_len = (size_t)(close - open - 1);
        memcpy(jsonp_buf, open + 1, inner_len);
        jsonp_buf[inner_len] = '\0';
        parse_body = jsonp_buf;
    }

    JNode *root = json_parse(parse_body);
    if (root) {
        int ok = copy_qobuz_json_url(root, out, outsz);
        json_free(root);
        if (ok) return 1;
    }

    const char *p = body;
    while ((p = strstr(p, "http")) != NULL) {
        if (!qobuz_url_looks_streamable(p)) { p += 4; continue; }
        const char *end = p;
        while (*end && *end != '"' && *end != '\'' && *end != '<' &&
               *end != '>' && *end != ')' && *end != ' ' &&
               *end != '\r' && *end != '\n' && *end != '\\') end++;
        size_t len = (size_t)(end - p);
        if (len > 10 && len + 1 < outsz) {
            memcpy(out, p, len);
            out[len] = '\0';
            return 1;
        }
        p += 4;
    }
    return 0;
}

static void musicdl_debug_key(char *out, size_t outsz) {
    static const unsigned char obf[] = {
        0xd7,0xdd,0xdd,0xcb,0xc8,0xc3,0xca,0xd1,0xca,0xc3,0xce,0xda,
        0xcc,0xcc,0xca,0xc4,0xd1,0xda,0xd8,0xc2,0xd9,0xd9,0xdd,0xd5,
        0xde,0xd3,0xd2,0xdb,0xca,0xdd,0xcd,0xdf,0xeb,0xe7,0xeb,0xe9,
        0xf2,0xe5
    };
    size_t n = sizeof obf;
    if (n + 1 > outsz) n = outsz ? outsz - 1 : 0;
    for (size_t i = 0; i < n; i++)
        out[i] = (char)(obf[i] ^ (0xA5u ^ (unsigned char)i));
    if (outsz) out[n] = '\0';
}

static int qobuz_musicdl_stream_url(const char *qobuz_id, const char *quality,
                                    char *out, size_t outsz,
                                    char *err, size_t errsz) {
    char key[64];
    musicdl_debug_key(key, sizeof key);

    char body[256];
    snprintf(body, sizeof body,
             "{\"url\":\"https://open.qobuz.com/track/%s\",\"quality\":\"%s\"}",
             qobuz_id, quality);

    char debug_hdr[128];
    snprintf(debug_hdr, sizeof debug_hdr, "X-Debug-Key: %s", key);
    const char *headers[] = {
        "User-Agent: " SPOTIFLAC_UA,
        "Accept: application/json, text/plain, */*",
        debug_hdr
    };

    char *resp = http_post_custom(MUSICDL_QOBUZ_URL, body, "application/json",
                                  headers, (int)(sizeof headers / sizeof headers[0]));
    if (!resp) {
        qobuz_stream_err(err, errsz, "MusicDL did not respond");
        return 0;
    }

    if (extract_qobuz_stream_url(resp, out, outsz)) {
        free(resp);
        return 1;
    }

    JNode *root = json_parse(resp);
    if (root) {
        const char *detail = jstr(jobj_get(root, "error"));
        if (!detail[0]) detail = jstr(jobj_get(root, "message"));
        const char *code = jstr(jobj_get(root, "code"));
        if (detail[0] && code[0])
            qobuz_stream_err(err, errsz, "MusicDL failed: %s (%s)", detail, code);
        else if (detail[0])
            qobuz_stream_err(err, errsz, "MusicDL failed: %s", detail);
        json_free(root);
    }
    if (err && errsz && !err[0])
        qobuz_stream_err(err, errsz, "MusicDL response did not include a stream URL");
    free(resp);
    return 0;
}

static const char *gdstudio_host_for_url(const char *api_url) {
    if (strstr(api_url, "music.gdstudio.org")) return "music.gdstudio.org";
    return "music.gdstudio.xyz";
}

static void gdstudio_padded_version(char *out, size_t outsz) {
    char tmp[32] = {0};
    size_t j = 0;
    const char *p = GDSTUDIO_VERSION;
    while (*p && j + 1 < sizeof tmp) {
        const char *start = p;
        while (*p && *p != '.') p++;
        size_t len = (size_t)(p - start);
        if (len == 1 && j + 2 < sizeof tmp) tmp[j++] = '0';
        for (size_t i = 0; i < len && j + 1 < sizeof tmp; i++)
            tmp[j++] = start[i];
        if (*p == '.') p++;
    }
    tmp[j] = '\0';
    snprintf(out, outsz, "%s", tmp);
}

static void gdstudio_ts9(const char *api_url, char *out, size_t outsz) {
    const char *host = gdstudio_host_for_url(api_url);
    char url[128];
    snprintf(url, sizeof url, "https://%s/time", host);
    char *resp = http_get(url);
    if (resp) {
        char *p = resp;
        while (*p && (*p < '0' || *p > '9')) p++;
        if (strlen(p) >= 9) {
            snprintf(out, outsz, "%.9s", p);
            free(resp);
            return;
        }
        free(resp);
    }
    snprintf(out, outsz, "%ld", (long)time(NULL));
}

static const char *gdstudio_bitrate(const char *quality) {
    if (strcmp(quality, "27") == 0 || strcmp(quality, "7") == 0) return "999";
    if (strcmp(quality, "6") == 0) return "740";
    return "320";
}

static int qobuz_gdstudio_stream_url(const char *api_url, const char *qobuz_id,
                                     const char *quality, char *out, size_t outsz,
                                     char *err, size_t errsz) {
    const char *host = gdstudio_host_for_url(api_url);
    char ver[32], ts9[32], sig_base[256], sig[33];
    gdstudio_padded_version(ver, sizeof ver);
    gdstudio_ts9(api_url, ts9, sizeof ts9);
    char enc_id[128];
    urlencode(qobuz_id, enc_id, sizeof enc_id);
    snprintf(sig_base, sizeof sig_base, "%s|%s|%.9s|%s", host, ver, ts9, enc_id);
    md5_hex(sig_base, sig);
    for (int i = 0; sig[i]; i++)
        if (sig[i] >= 'a' && sig[i] <= 'z') sig[i] -= 32;

    char body[256];
    snprintf(body, sizeof body, "types=url&id=%s&source=qobuz&br=%s&s=%.8s",
             qobuz_id, gdstudio_bitrate(quality), sig + 24);

    char origin[96], referer[96];
    snprintf(origin, sizeof origin, "Origin: https://%s", host);
    snprintf(referer, sizeof referer, "Referer: https://%s/", host);
    const char *headers[] = {
        "User-Agent: " SPOTIFLAC_UA,
        "Accept: application/json, text/plain, */*",
        origin,
        referer
    };

    char *resp = http_post_custom(api_url, body,
                                  "application/x-www-form-urlencoded; charset=UTF-8",
                                  headers, (int)(sizeof headers / sizeof headers[0]));
    if (!resp) {
        qobuz_stream_err(err, errsz, "GDStudio did not respond");
        return 0;
    }

    if (extract_qobuz_stream_url(resp, out, outsz)) {
        free(resp);
        return 1;
    }

    JNode *root = json_parse(resp);
    if (root) {
        const char *detail = jstr(jobj_get(root, "detail"));
        if (!detail[0]) detail = jstr(jobj_get(root, "error"));
        if (!detail[0]) detail = jstr(jobj_get(root, "message"));
        if (detail[0])
            qobuz_stream_err(err, errsz, "GDStudio failed: %s", detail);
        json_free(root);
    }
    if (err && errsz && !err[0])
        qobuz_stream_err(err, errsz, "GDStudio response did not include a stream URL");
    free(resp);
    return 0;
}

static int qobuz_resolve_via_simple_post(const char *resolver_url, const char *qobuz_id,
                                         const char *quality, char *out_url, size_t sz,
                                         char *err, size_t errsz) {
    const char *requested = zarz_quality_for_label(quality);
    int fallback_count = 0;
    const char *const *fallback = quality_fallbacks(requested, &fallback_count);

    for (int i = 0; i < fallback_count; i++) {
        char body[1024];
        snprintf(body, sizeof body,
                 "{\"quality\":\"%s\",\"upload_to_r2\":false,"
                 "\"url\":\"https://play.qobuz.com/track/%s\"}",
                 fallback[i], qobuz_id);

        char *resp = http_post(resolver_url, body);
        if (!resp) {
            qobuz_stream_err(err, errsz, "Resolver did not respond");
            continue;
        }

        if (extract_qobuz_stream_url(resp, out_url, sz)) {
            free(resp);
            return 1;
        }
        free(resp);
        qobuz_stream_err(err, errsz, "Resolver response did not include a stream URL");
    }
    return 0;
}

static int qobuz_try_direct_stream_resolvers(const char *qobuz_id, const char *quality,
                                              char *out_url, size_t sz,
                                              char *err, size_t errsz) {
    /* Direct Akamai/Qobuz CDN URLs — much faster than Lucida's proxy download. */
    if (!qobuz_community_disabled() &&
        qobuz_community_supports_quality(quality) &&
        qobuz_community_stream_url(qobuz_id, quality, out_url, sz, err, errsz))
        return 1;

    char sq[8];
    qobuz_spotiflac_quality(quality, sq, sizeof sq);
    int fb_count = 0;
    const char *const *fb = spotiflac_quality_fallbacks(sq, &fb_count);
    for (int i = 0; i < fb_count; i++) {
        if (qobuz_musicdl_stream_url(qobuz_id, fb[i], out_url, sz, err, errsz))
            return 1;
        if (qobuz_gdstudio_stream_url(GDSTUDIO_XYZ_URL, qobuz_id, fb[i],
                                    out_url, sz, err, errsz))
            return 1;
        if (qobuz_gdstudio_stream_url(GDSTUDIO_ORG_URL, qobuz_id, fb[i],
                                    out_url, sz, err, errsz))
            return 1;
    }

    if (err && errsz) err[0] = '\0';
    return 0;
}

static int qobuz_resolve_id_via_configured_resolver(const char *qobuz_id, const char *quality,
                                                     char *out_url, size_t sz,
                                                     char *err, size_t errsz,
                                                     int allow_slow_lucida) {
    (void)allow_slow_lucida;

    const char *cache_quality = qobuz_community_quality_label(quality);
    char cache_key[160];
    snprintf(cache_key, sizeof cache_key, "%s|%s", qobuz_id ? qobuz_id : "", cache_quality);
    if (qobuz_cache_get_stream(cache_key, out_url, sz)) {
        if (err && errsz) err[0] = '\0';
        return 1;
    }

    char last_err[160] = {0};
    qobuz_resolver_gate_begin("stream resolve");
    for (int attempt = 0; attempt < 2; attempt++) {
        char tmp_err[160] = {0};
        if (qobuz_try_direct_stream_resolvers(qobuz_id, quality, out_url, sz,
                                             tmp_err, sizeof tmp_err)) {
            qobuz_resolver_gate_end();
            qobuz_cache_put_stream(cache_key, out_url);
            if (err && errsz) err[0] = '\0';
            return 1;
        }
        if (tmp_err[0]) snprintf(last_err, sizeof last_err, "%s", tmp_err);
        if (attempt == 0) sqt_sleep_ms(700);
    }

    const char *resolver_url = qobuz_resolver_url(err, errsz);
    if (resolver_url) {
        if (lucida_url_p(resolver_url)) {
            qobuz_resolver_gate_end();
            qobuz_stream_err(err, errsz,
                             "Lucida resolver is disabled; use a direct Qobuz resolver URL");
            return 0;
        }
        if (qobuz_resolve_via_simple_post(resolver_url, qobuz_id, quality,
                                          out_url, sz, err, errsz)) {
            qobuz_resolver_gate_end();
            qobuz_cache_put_stream(cache_key, out_url);
            return 1;
        }
    }
    qobuz_resolver_gate_end();

    if (err && errsz) {
        if (last_err[0]) snprintf(err, errsz,
                                  "Direct Qobuz resolvers did not return a stream URL (%s)",
                                  last_err);
        else qobuz_stream_err(err, errsz, "Direct Qobuz resolvers did not return a stream URL");
    }
    return 0;
}

static int api_qobuz_get_stream_url_err_mode(const char *isrc, const char *quality,
                                             char *out_url, size_t sz,
                                             char *err, size_t errsz,
                                             int allow_slow_lucida) {
    if (err && errsz > 0) err[0] = '\0';
    if (!isrc || !isrc[0]) {
        qobuz_stream_err(err, errsz, "missing ISRC");
        return 0;
    }

    char cache_key[320];
    char qobuz_id[64] = {0};
    snprintf(cache_key, sizeof cache_key, "isrc:%s", isrc);

    if (!qobuz_cache_get_id(cache_key, qobuz_id, sizeof qobuz_id)) {
        char app_id[16], secret[36];
        get_qbz_creds(app_id, secret);
        (void)secret;

        char enc[256];
        urlencode(isrc, enc, sizeof enc);

        char search_url[1024];
        snprintf(search_url, sizeof search_url,
                 "%s/track/search?query=%s&limit=5&app_id=%s",
                 QBZ_API, enc, app_id);

        qobuz_resolver_gate_begin("Qobuz ISRC search");
        char *resp = http_get(search_url);
        qobuz_resolver_gate_end();
        if (!resp) {
            qobuz_stream_err(err, errsz, "Qobuz search request failed");
            return 0;
        }

        JNode *root = json_parse(resp);
        free(resp);
        if (!root) {
            qobuz_stream_err(err, errsz, "Qobuz search returned invalid JSON");
            return 0;
        }

        JNode *tracks = jobj_get(root, "tracks");
        JNode *items  = tracks ? jobj_get(tracks, "items") : NULL;
        if (!items || items->type != J_ARR || items->arr.len == 0) {
            qobuz_stream_err(err, errsz, "no Qobuz track match for ISRC");
            json_free(root);
            return 0;
        }

        JNode *first   = items->arr.items[0];
        JNode *id_node = jobj_get(first, "id");
        if (id_node && id_node->type == J_NUM)
            snprintf(qobuz_id, sizeof qobuz_id, "%.0f", id_node->n);
        else
            snprintf(qobuz_id, sizeof qobuz_id, "%s", jstr(id_node));
        json_free(root);

        if (!qobuz_id[0]) {
            qobuz_stream_err(err, errsz, "Qobuz track match had no ID");
            return 0;
        }
        qobuz_cache_put_id(cache_key, qobuz_id);
    }

    return qobuz_resolve_id_via_configured_resolver(qobuz_id, quality,
                                                    out_url, sz, err, errsz,
                                                    allow_slow_lucida);

}

int api_qobuz_get_stream_url_err(const char *isrc, const char *quality,
                                 char *out_url, size_t sz,
                                 char *err, size_t errsz) {
    return api_qobuz_get_stream_url_err_mode(isrc, quality, out_url, sz,
                                             err, errsz, 1);
}

int api_qobuz_get_stream_url(const char *isrc, const char *quality, char *out_url, size_t sz) {
    return api_qobuz_get_stream_url_err(isrc, quality, out_url, sz, NULL, 0);
}


static void match_normalize(const char *in, char *out, size_t outsz) {
    size_t j = 0;
    int last_space = 1;
    for (const unsigned char *p = (const unsigned char *)(in ? in : ""); *p && j + 1 < outsz; p++) {
        unsigned char c = *p;
        if (isalnum(c)) {
            out[j++] = (char)tolower(c);
            last_space = 0;
        } else if (!last_space && j + 1 < outsz) {
            out[j++] = ' ';
            last_space = 1;
        }
    }
    while (j > 0 && out[j - 1] == ' ') j--;
    out[j] = '\0';
}

static int token_overlap_score(const char *want, const char *got, int max_score) {
    char nw[512], ng[512];
    match_normalize(want, nw, sizeof nw);
    match_normalize(got, ng, sizeof ng);
    if (!nw[0] || !ng[0]) return 0;
    if (strcmp(nw, ng) == 0) return max_score;
    if (strstr(ng, nw) || strstr(nw, ng)) return (max_score * 3) / 4;

    int tokens = 0, hits = 0;
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", nw);
    for (char *tok = strtok(tmp, " "); tok; tok = strtok(NULL, " ")) {
        if (strlen(tok) < 2) continue;
        tokens++;
        if (strstr(ng, tok)) hits++;
    }
    if (tokens == 0) return 0;
    return (max_score * hits) / tokens;
}

static const char *qobuz_item_artist(JNode *item) {
    JNode *performer = jobj_get(item, "performer");
    JNode *name = performer ? jobj_get(performer, "name") : NULL;
    if (name && jstr(name)[0]) return jstr(name);
    JNode *artist = jobj_get(item, "artist");
    name = artist ? jobj_get(artist, "name") : NULL;
    if (name && jstr(name)[0]) return jstr(name);
    return "";
}

static int qobuz_match_score(JNode *item, const char *title, const char *artist, int duration, int *duration_diff_out) {
    int score = 0;
    const char *qt = jstr(jobj_get(item, "title"));
    const char *qa = qobuz_item_artist(item);

    score += token_overlap_score(title, qt, 50);
    if (artist && artist[0]) score += token_overlap_score(artist, qa, 25);

    int diff = -1;
    JNode *dur = jobj_get(item, "duration");
    if (duration > 0 && dur && dur->type == J_NUM && dur->n > 0) {
        int qd = (int)dur->n;
        diff = duration > qd ? duration - qd : qd - duration;
        if (diff <= 2) score += 25;
        else if (diff <= 5) score += 18;
        else if (diff <= 8) score += 10;
        else if (diff <= 15) score -= 10;
        else score -= 35;
    }
    if (duration_diff_out) *duration_diff_out = diff;
    return score;
}

static int strict_match_enabled(void) {
    const char *v = getenv("SQUIDGET_STRICT_MATCH");
    return v && v[0] && strcmp(v, "0") != 0;
}

/* Search Qobuz by title+artist and attempt to get stream URL
   Used as a fallback for iTunes tracks that don't have ISRC */
static int api_qobuz_get_stream_url_by_title_err_mode(const char *title, const char *artist,
                                                      int duration, const char *quality,
                                                      char *out_url, size_t sz,
                                                      char *err, size_t errsz,
                                                      int allow_slow_lucida) {
    if (err && errsz > 0) err[0] = '\0';
    if (!title || !title[0]) {
        if (err && errsz)
            snprintf(err, errsz, "missing track title");
        return 0;
    }

    char query[512];
    if (artist && artist[0])
        snprintf(query, sizeof query, "%s %s", title, artist);
    else
        snprintf(query, sizeof query, "%s", title);

    char cache_key[640];
    char qobuz_id[64] = {0};
    snprintf(cache_key, sizeof cache_key, "title:%s|artist:%s|dur:%d",
             title, artist ? artist : "", duration);

    if (!qobuz_cache_get_id(cache_key, qobuz_id, sizeof qobuz_id)) {
        char app_id[16], secret[36];
        get_qbz_creds(app_id, secret);
        (void)secret;

        char enc[256];
        urlencode(query, enc, sizeof enc);

        char search_url[1024];
        snprintf(search_url, sizeof search_url,
                 "%s/track/search?query=%s&limit=5&app_id=%s",
                 QBZ_API, enc, app_id);

        api_debug("Qobuz title search: query='%s' duration=%d", query, duration);
        qobuz_resolver_gate_begin("Qobuz title search");
        char *resp = http_get(search_url);
        qobuz_resolver_gate_end();
        if (!resp) {
            if (err && errsz)
                snprintf(err, errsz, "Qobuz search request failed");
            return 0;
        }

        JNode *root = json_parse(resp);
        free(resp);
        if (!root) {
            if (err && errsz)
                snprintf(err, errsz, "Qobuz search returned invalid JSON");
            return 0;
        }

        JNode *tracks = jobj_get(root, "tracks");
        JNode *items = tracks ? jobj_get(tracks, "items") : NULL;
        if (!items || items->type != J_ARR || items->arr.len == 0) {
            if (err && errsz)
                snprintf(err, errsz, "no Qobuz track match for '%s'", query);
            json_free(root);
            return 0;
        }

        int best_i = -1;
        int best_score = -1000;
        int best_diff = -1;
        for (int i = 0; i < items->arr.len; i++) {
            int diff = -1;
            int score = qobuz_match_score(items->arr.items[i], title, artist, duration, &diff);
            api_debug("Qobuz candidate %d score=%d diff=%d title='%s' artist='%s'",
                      i + 1, score, diff,
                      jstr(jobj_get(items->arr.items[i], "title")),
                      qobuz_item_artist(items->arr.items[i]));
            if (score > best_score) {
                best_score = score;
                best_i = i;
                best_diff = diff;
            }
        }

        if (best_i < 0 || (strict_match_enabled() && (best_score < 55 || best_diff > 8))) {
            if (err && errsz)
                snprintf(err, errsz, "no confident Qobuz track match for '%s'", query);
            json_free(root);
            return 0;
        }

        JNode *best = items->arr.items[best_i];
        JNode *id_node = jobj_get(best, "id");
        if (id_node && id_node->type == J_NUM)
            snprintf(qobuz_id, sizeof qobuz_id, "%.0f", id_node->n);
        else
            snprintf(qobuz_id, sizeof qobuz_id, "%s", jstr(id_node));

        json_free(root);

        if (!qobuz_id[0]) {
            if (err && errsz)
                snprintf(err, errsz, "Qobuz track match had no ID");
            return 0;
        }

        qobuz_cache_put_id(cache_key, qobuz_id);
        api_debug("Qobuz title search: selected track id=%s score=%d diff=%d",
                  qobuz_id, best_score, best_diff);
    } else {
        api_debug("Qobuz title search: cache hit id=%s", qobuz_id);
    }

    return qobuz_resolve_id_via_configured_resolver(qobuz_id, quality,
                                                    out_url, sz, err, errsz,
                                                    allow_slow_lucida);

}

int api_qobuz_get_stream_url_by_title_err(const char *title, const char *artist,
                                          int duration, const char *quality,
                                          char *out_url, size_t sz,
                                          char *err, size_t errsz) {
    return api_qobuz_get_stream_url_by_title_err_mode(title, artist, duration,
                                                      quality, out_url, sz,
                                                      err, errsz, 1);
}




