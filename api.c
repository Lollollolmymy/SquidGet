#ifdef SQT_DEBUG
#  define SQT_LOG(...) fprintf(stderr, "[sqt] " __VA_ARGS__), fprintf(stderr, "\n")
#endif
#include "squidget.h"
#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

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

static int buf_ensure(Buf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return 1;
    size_t nc = (b->len + extra + 1) * 2;
    char *nb = realloc(b->buf, nc);
    if (!nb) return 0;
    b->buf = nb; b->cap = nc; return 1;
}

/* ── WINDOWS: WinHTTP ── */
#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>

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
    HINTERNET hs=WinHttpOpen(L"squidget/2.0",WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
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
    HINTERNET hs=WinHttpOpen(L"squidget/2.0",WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
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
    HINTERNET hs=WinHttpOpen(L"squidget/2.0",WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
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

char *http_post(const char *url, const char *json_body) {
    Buf b={malloc(4096),0,4096}; if(!b.buf) return NULL;
    int s=wh_post(url,json_body,&b);
    if(s>=200&&s<400) return b.buf;
    free(b.buf); return NULL;
}

// posix: libcurl (preferred) or curl cli fallback
#else

/* ── http_init / http_cleanup ─────────────────────────────────────────────── */

#ifdef SQT_USE_CURL
/* ── libcurl implementation (#2) ─────────────────────────────────────────────
 * Connection reuse via CURLSH (shared connection + DNS + SSL session cache).
 * Locking callbacks make the shared handle safe for the album thread pool. */
#include <curl/curl.h>

static CURLSH         *g_curl_share   = NULL;
static sqt_mutex_t     g_curl_sh_lock;

static void curl_sh_lock_cb(CURL *h, curl_lock_data d, curl_lock_access a, void *u) {
    (void)h; (void)d; (void)a; (void)u;
    sqt_mutex_lock(&g_curl_sh_lock);
}
static void curl_sh_unlock_cb(CURL *h, curl_lock_data d, void *u) {
    (void)h; (void)d; (void)u;
    sqt_mutex_unlock(&g_curl_sh_lock);
}

void http_init(void) {
    curl_global_init(CURL_GLOBAL_ALL);
    sqt_mutex_init(&g_curl_sh_lock);
    g_curl_share = curl_share_init();
    curl_share_setopt(g_curl_share, CURLSHOPT_SHARE,      CURL_LOCK_DATA_CONNECT);
    curl_share_setopt(g_curl_share, CURLSHOPT_SHARE,      CURL_LOCK_DATA_DNS);
    curl_share_setopt(g_curl_share, CURLSHOPT_SHARE,      CURL_LOCK_DATA_SSL_SESSION);
    curl_share_setopt(g_curl_share, CURLSHOPT_LOCKFUNC,   curl_sh_lock_cb);
    curl_share_setopt(g_curl_share, CURLSHOPT_UNLOCKFUNC, curl_sh_unlock_cb);
}
void http_cleanup(void) {
    if (g_curl_share) { curl_share_cleanup(g_curl_share); g_curl_share = NULL; }
    sqt_mutex_destroy(&g_curl_sh_lock);
    curl_global_cleanup();
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
    curl_easy_setopt(c, CURLOPT_SHARE,          g_curl_share);
    curl_easy_setopt(c, CURLOPT_USERAGENT,      "squidget/2.0");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,        timeout_s);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE,  1L);
    return c;
}

char *http_get(const char *url) {
    Buf b = {malloc(4096), 0, 4096};
    if (!b.buf) return NULL;
    char *res = NULL;
    for (int i = 0; i < 3; i++) {
        b.len = 0; b.buf[0] = '\0';
        CURL *c = curl_new(15L);
        if (!c) break;
        curl_easy_setopt(c, CURLOPT_URL,           url);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_buf);
        curl_easy_setopt(c, CURLOPT_WRITEDATA,     &b);
        CURLcode rc = curl_easy_perform(c);
        long http_code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
        curl_easy_cleanup(c);
        if (rc == CURLE_OK && http_code >= 200 && http_code < 400) { res = b.buf; break; }
        if (rc == CURLE_OK && http_code >= 400 && http_code < 500) break; /* client error */
        if (i < 2) sqt_sleep_ms(500 * (unsigned)(i + 1));
    }
    if (!res) free(b.buf);
    return res;
}

long http_get_file(const char *url, const char *path,
                    void (*progress_cb)(size_t received, size_t total, void *ud), void *ud) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    CURL *c = curl_new(120L);
    if (!c) { fclose(f); remove(path); return -1; }
    CurlProg cp = {progress_cb, ud};
    curl_easy_setopt(c, CURLOPT_URL,           url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_file);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,     f);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS,    0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, curl_progress_cb);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA,     &cp);
    CURLcode rc = curl_easy_perform(c);
    long http_code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
    curl_off_t total_sz = 0;
    curl_easy_getinfo(c, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &total_sz);
    curl_easy_cleanup(c);
    long sz = (long)ftell(f);
    fclose(f);
    if (rc != CURLE_OK || http_code < 200 || http_code >= 400) { remove(path); return -1; }
    return (long)total_sz > 0 ? (long)total_sz : sz;
}

char *http_post(const char *url, const char *json_body) {
    Buf b = {malloc(4096), 0, 4096};
    if (!b.buf) return NULL;
    CURL *c = curl_new(15L);
    if (!c) { free(b.buf); return NULL; }
    struct curl_slist *hdrs = curl_slist_append(NULL, "Content-Type: application/json");
    curl_easy_setopt(c, CURLOPT_URL,            url);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS,     json_body ? json_body : "");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER,     hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,  curl_write_buf);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,      &b);
    CURLcode rc = curl_easy_perform(c);
    long http_code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(c);
    curl_slist_free_all(hdrs);
    if (rc == CURLE_OK && http_code >= 200 && http_code < 400) return b.buf;
    free(b.buf); return NULL;
}

#else  /* !SQT_USE_CURL — original popen/curl-CLI path */

void http_init(void)    { /* no-op */ }
void http_cleanup(void) { /* no-op */ }

static void urlencode(const char *in, char *out, size_t outsz) {
    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && j + 3 < outsz; p++) {
        if ((*p>='A'&&*p<='Z')||(*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')
            ||*p=='-'||*p=='_'||*p=='.'||*p=='~') {
            out[j++] = (char)*p;
        } else {
            out[j++] = '%';
            out[j++] = hex[*p >> 4];
            out[j++] = hex[*p & 0xF];
        }
    }
    out[j] = '\0';
}

// escape single quotes for shell
static void shell_escape(const char *in, char *out, size_t outsz) {
    size_t j = 0;
    for (const char *p = in; *p && j + 6 < outsz; p++) {
        if (*p == '\'') {
            /* '\'' */
            if (j + 4 >= outsz) break;
            out[j++] = '\'';
            out[j++] = '\\';
            out[j++] = '\'';
            out[j++] = '\'';
        } else {
            out[j++] = *p;
        }
    }
    out[j] = '\0';
}

// read all pipe output into a buffer
static char *popen_read(const char *cmd) {
    FILE *p = popen(cmd, "r");
    if (!p) return NULL;
    Buf b = {malloc(4096), 0, 4096};
    if (!b.buf) { pclose(p); return NULL; }
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, p)) > 0) {
        if (!buf_ensure(&b, n)) break;
        memcpy(b.buf + b.len, chunk, n);
        b.len += n;
        b.buf[b.len] = '\0';
    }
    pclose(p);
    if (b.len == 0) { free(b.buf); return NULL; }
    return b.buf;
}

char *http_get(const char *url) {
    char esc_url[4096];
    shell_escape(url, esc_url, sizeof esc_url);
    char cmd[8192];  /* esc_url up to 4095 + fixed prefix ~200 chars */
    char *res = NULL;
    for (int i = 0; i < 3; i++) {
        snprintf(cmd, sizeof cmd,
                 "curl -fsSL --max-time 15 -A 'squidget/2.0' '%s' 2>/dev/null",
                 esc_url);
        res = popen_read(cmd);
        if (res) break;
        if (i < 2) sqt_sleep_ms(500 * (i + 1));
    }
    return res;
}

long http_get_file(const char *url, const char *path,
                    void (*progress_cb)(size_t received, size_t total, void *ud), void *ud) {
    char esc_url[4096], esc_path[4096];
    shell_escape(url,  esc_url,  sizeof esc_url);
    shell_escape(path, esc_path, sizeof esc_path);
    char cmd[8192];
    
    /* On POSIX without libcurl, we'll use curl to write to stdout and we read it manually 
       to provide progress updates. */
    snprintf(cmd, sizeof cmd,
             "curl -sSL --max-time 120 -A 'squidget/2.0' '%s'",
             esc_url);
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    
    FILE *f = fopen(path, "wb");
    if (!f) { pclose(p); return -1; }
    
    char chunk[16384];
    size_t n, received = 0;
    while ((n = fread(chunk, 1, sizeof chunk, p)) > 0) {
        fwrite(chunk, 1, n, f);
        received += n;
        if (progress_cb) progress_cb(received, 0, ud); /* total unknown via pipe */
    }
    
    fclose(f);
    int curl_ret = pclose(p);
    if (curl_ret != 0) { remove(path); return -1; }
    return (long)received;
}

char *http_post(const char *url, const char *json_body) {
    char esc_url[4096], esc_body[8192];
    shell_escape(url,                        esc_url,  sizeof esc_url);
    shell_escape(json_body ? json_body : "", esc_body, sizeof esc_body);
    char cmd[16384];
    snprintf(cmd, sizeof cmd,
             "curl -sSL --max-time 15 -A 'squidget/2.0' "
             "-X POST -H 'Content-Type: application/json' "
             "-d '%s' '%s' 2>/dev/null",
             esc_body, esc_url);
    return popen_read(cmd);
}

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

/* helper: pick quality label from bit-depth field */
static const char *qobuz_quality_label(JNode *t) {
    int bd = (int)jnum(jobj_get(t, "maximum_bit_depth"));
    return (bd >= 24) ? QUAL_HIR : QUAL_HIGH;
}

/* fill a Track from a Qobuz track JSON object */
static void fill_track_qobuz(JNode *t, Track *out) {
    memset(out, 0, sizeof *out);

    JNode *id = jobj_get(t, "id");
    if (id && id->type == J_NUM) snprintf(out->id, sizeof out->id, "%.0f", id->n);
    else                          snprintf(out->id, sizeof out->id, "%s",   jstr(id));

    snprintf(out->title, sizeof out->title, "%s", jstr(jobj_get(t, "title")));

    /* artist: performer.name, fall back to artist.name */
    JNode *perf = jobj_get(t, "performer");
    if (perf && perf->type == J_OBJ)
        snprintf(out->artist, sizeof out->artist, "%s", jstr(jobj_get(perf, "name")));
    if (!out->artist[0]) {
        JNode *ao = jobj_get(t, "artist");
        if (ao && ao->type == J_OBJ)
            snprintf(out->artist, sizeof out->artist, "%s", jstr(jobj_get(ao, "name")));
    }

    /* album: title, cover URL (full https://...), year */
    JNode *alb = jobj_get(t, "album");
    if (alb && alb->type == J_OBJ) {
        snprintf(out->album, sizeof out->album, "%s", jstr(jobj_get(alb, "title")));
        JNode *img = jobj_get(alb, "image");
        if (img && img->type == J_OBJ)
            snprintf(out->cover, sizeof out->cover, "%s", jstr(jobj_get(img, "large")));
        const char *rd = jstr(jobj_get(alb, "release_date_original"));
        if (rd && rd[0]) snprintf(out->year, sizeof out->year, "%.4s", rd);
        /* artist fallback from album.artist when performer absent */
        if (!out->artist[0]) {
            JNode *aa = jobj_get(alb, "artist");
            if (aa && aa->type == J_OBJ)
                snprintf(out->artist, sizeof out->artist, "%s", jstr(jobj_get(aa, "name")));
        }
    }

    out->duration  = (int)jnum(jobj_get(t, "duration"));
    out->track_num = (int)jnum(jobj_get(t, "track_number"));
    out->disc_num  = (int)jnum(jobj_get(t, "disc_number"));

    /* explicit flag */
    JNode *pw = jobj_get(t, "parental_warning");
    out->explicit_ = (pw && pw->type == J_BOOL && pw->b) ? 1 : 0;

    snprintf(out->isrc,      sizeof out->isrc,      "%s", jstr(jobj_get(t, "isrc")));
    snprintf(out->copyright, sizeof out->copyright, "%s", jstr(jobj_get(t, "copyright")));
    snprintf(out->quality,   sizeof out->quality,   "%s", qobuz_quality_label(t));
}

/* fill an Album from a Qobuz album JSON object */
static void fill_album_qobuz(JNode *a, Album *out) {
    memset(out, 0, sizeof *out);
    snprintf(out->id,    sizeof out->id,    "%s", jstr(jobj_get(a, "id")));
    snprintf(out->title, sizeof out->title, "%s", jstr(jobj_get(a, "title")));
    JNode *artist = jobj_get(a, "artist");
    if (artist && artist->type == J_OBJ)
        snprintf(out->artist, sizeof out->artist, "%s", jstr(jobj_get(artist, "name")));
    else
        snprintf(out->artist, sizeof out->artist, "?");
    JNode *nt = jobj_get(a, "tracks_count");
    out->num_tracks = nt ? (int)jnum(nt) : 0;
}

int api_search_tracks(const char *query, Track *out, int max) {
    char app_id[16], secret[36];
    get_qbz_creds(app_id, secret);
    char enc[1600], ts_str[32], sig[33], payload[2048], url[2048];
    urlencode(query, enc, sizeof enc);
    snprintf(ts_str, sizeof ts_str, "%ld", (long)time(NULL));
    /* sig payload (Qobuz signing convention): method + sorted(param_name+val) + ts + secret
     * params sorted alpha: limit, query */
    char lim[16]; snprintf(lim, sizeof lim, "%d", max);
    snprintf(payload, sizeof payload, "tracksearchlimit%squery%s%s%s",
             lim, query, ts_str, secret);
    md5_hex(payload, sig);
    snprintf(url, sizeof url,
             "%s/track/search?query=%s&limit=%d&app_id=%s&request_ts=%s&request_sig=%s",
             QBZ_API, enc, max, app_id, ts_str, sig);

    char *resp = http_get(url);
    if (!resp) { SQT_LOG("qobuz search tracks: request failed"); return 0; }
    JNode *root = json_parse(resp); free(resp);
    if (!root) { SQT_LOG("qobuz search tracks: parse failed"); return 0; }

    JNode *tracks = jobj_get(root, "tracks");
    JNode *items  = tracks ? jobj_get(tracks, "items") : NULL;
    int cnt = 0;
    if (items && items->type == J_ARR) {
        int n = items->arr.len < max ? items->arr.len : max;
        for (int i = 0; i < n; i++) fill_track_qobuz(items->arr.items[i], &out[cnt++]);
    }
    SQT_LOG("api_search_tracks: returning %d tracks", cnt);
    json_free(root);
    return cnt;
}

int api_search_albums(const char *query, Album *out, int max) {
    char app_id[16], secret[36];
    get_qbz_creds(app_id, secret);
    char enc[1600], ts_str[32], sig[33], payload[2048], url[2048];
    urlencode(query, enc, sizeof enc);
    snprintf(ts_str, sizeof ts_str, "%ld", (long)time(NULL));
    /* params sorted alpha: limit, query */
    char lim[16]; snprintf(lim, sizeof lim, "%d", max);
    snprintf(payload, sizeof payload, "albumsearchlimit%squery%s%s%s",
             lim, query, ts_str, secret);
    md5_hex(payload, sig);
    snprintf(url, sizeof url,
             "%s/album/search?query=%s&limit=%d&app_id=%s&request_ts=%s&request_sig=%s",
             QBZ_API, enc, max, app_id, ts_str, sig);

    char *resp = http_get(url);
    if (!resp) { SQT_LOG("qobuz search albums: request failed"); return 0; }
    JNode *root = json_parse(resp); free(resp);
    if (!root) { SQT_LOG("qobuz search albums: parse failed"); return 0; }

    JNode *albs  = jobj_get(root, "albums");
    JNode *items = albs ? jobj_get(albs, "items") : NULL;
    int cnt = 0;
    if (items && items->type == J_ARR) {
        int n = items->arr.len < max ? items->arr.len : max;
        for (int i = 0; i < n; i++) fill_album_qobuz(items->arr.items[i], &out[cnt++]);
    }
    SQT_LOG("api_search_albums: returning %d albums", cnt);
    json_free(root);
    return cnt;
}

int api_get_album_tracks(const char *album_id, Track *out, int max) {
    char app_id[16], secret[36];
    get_qbz_creds(app_id, secret);
    char enc_id[256], ts_str[32], sig[33], payload[512], url[2048];
    urlencode(album_id, enc_id, sizeof enc_id);
    snprintf(ts_str, sizeof ts_str, "%ld", (long)time(NULL));
    /* param: album_id only */
    snprintf(payload, sizeof payload, "albumgetalbum_id%s%s%s",
             album_id, ts_str, secret);
    md5_hex(payload, sig);
    snprintf(url, sizeof url,
             "%s/album/get?album_id=%s&app_id=%s&request_ts=%s&request_sig=%s",
             QBZ_API, enc_id, app_id, ts_str, sig);

    char *resp = http_get(url);
    if (!resp) { SQT_LOG("qobuz album tracks: request failed"); return 0; }
    JNode *root = json_parse(resp); free(resp);
    if (!root) { SQT_LOG("qobuz album tracks: parse failed"); return 0; }

    /* hoist album-level fields to fill in tracks that lack them */
    char album_name[256] = {0}, album_cover[512] = {0}, album_year[8] = {0};
    const char *alb_t = jstr(jobj_get(root, "title"));
    if (alb_t && alb_t[0]) snprintf(album_name, sizeof album_name, "%s", alb_t);
    JNode *img = jobj_get(root, "image");
    if (img && img->type == J_OBJ) {
        const char *lrg = jstr(jobj_get(img, "large"));
        if (lrg && lrg[0]) snprintf(album_cover, sizeof album_cover, "%s", lrg);
    }
    const char *rd = jstr(jobj_get(root, "release_date_original"));
    if (rd && rd[0]) snprintf(album_year, sizeof album_year, "%.4s", rd);

    JNode *tracks = jobj_get(root, "tracks");
    JNode *items  = tracks ? jobj_get(tracks, "items") : NULL;
    int cnt = 0;
    if (items && items->type == J_ARR) {
        int n = items->arr.len < max ? items->arr.len : max;
        for (int i = 0; i < n; i++) {
            fill_track_qobuz(items->arr.items[i], &out[cnt]);
            if (!out[cnt].album[0] && album_name[0])
                snprintf(out[cnt].album, sizeof out[cnt].album, "%s", album_name);
            if (!out[cnt].cover[0] && album_cover[0])
                snprintf(out[cnt].cover, sizeof out[cnt].cover, "%s", album_cover);
            if (!out[cnt].year[0]  && album_year[0])
                snprintf(out[cnt].year,  sizeof out[cnt].year,  "%s", album_year);
            cnt++;
        }
    }
    SQT_LOG("api_get_album_tracks: returning %d tracks", cnt);
    json_free(root);
    return cnt;
}

/* Fetch full track metadata from Qobuz /track/get.
   Merges into *out — fields already set are preserved if Qobuz returns empty. */
int api_get_track_info(const char *track_id, Track *out) {
    char app_id[16], secret[36];
    get_qbz_creds(app_id, secret);
    char enc_id[256], ts_str[32], sig[33], payload[512], url[2048];
    urlencode(track_id, enc_id, sizeof enc_id);
    snprintf(ts_str, sizeof ts_str, "%ld", (long)time(NULL));
    snprintf(payload, sizeof payload, "trackgettrack_id%s%s%s",
             track_id, ts_str, secret);
    md5_hex(payload, sig);
    snprintf(url, sizeof url,
             "%s/track/get?track_id=%s&app_id=%s&request_ts=%s&request_sig=%s",
             QBZ_API, enc_id, app_id, ts_str, sig);

    SQT_LOG("Fetching track info for ID: %s", track_id);
    char *resp = http_get(url);
    if (!resp) { SQT_LOG("Failed to fetch track info"); return 0; }
    JNode *root = json_parse(resp); free(resp);
    if (!root) { SQT_LOG("Failed to parse track info JSON"); return 0; }

    Track fresh;
    fill_track_qobuz(root, &fresh);

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

#define ZARZ_URL   "https://api.zarz.moe/dl/qbz"

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
 *   2. POST to api.zarz.moe (Qobuz track ID → Akamai FLAC URL)
 *
 * Returns 1 and fills out_url on success; 0 on any failure.
 */
int api_qobuz_get_stream_url(const char *isrc, char *out_url, size_t sz) {
    if (!isrc || !isrc[0]) return 0;

    /* 1 ── sign a Qobuz track/search request for this ISRC
     *
     * Signature payload (Qobuz signing convention):
     *   "tracksearch" + "limit" + "1" + "query" + <isrc> + <ts> + <secret>
     * (params sorted alphabetically, app_id/ts/sig excluded)
     */
    char app_id[16], secret[36];
    get_qbz_creds(app_id, secret);

    char enc[256];
    urlencode(isrc, enc, sizeof enc);

    char ts_str[32];
    snprintf(ts_str, sizeof ts_str, "%ld", (long)time(NULL));

    char payload[512];
    snprintf(payload, sizeof payload,
             "tracksearchlimit1query%s%s%s",
             isrc,
             ts_str,
             secret);

    char sig[33];
    md5_hex(payload, sig);

    char search_url[1024];
    snprintf(search_url, sizeof search_url,
             "%s/track/search?query=%s&limit=1&app_id=%s&request_ts=%s&request_sig=%s",
             QBZ_API, enc, app_id, ts_str, sig);

    char *resp = http_get(search_url);
    if (!resp) { SQT_LOG("qobuz: search request failed"); return 0; }

    JNode *root = json_parse(resp); free(resp);
    if (!root) { SQT_LOG("qobuz: search parse failed"); return 0; }

    JNode *tracks = jobj_get(root, "tracks");
    JNode *items  = tracks ? jobj_get(tracks, "items") : NULL;
    if (!items || items->type != J_ARR || items->arr.len == 0) {
        SQT_LOG("qobuz: no results for ISRC %s", isrc);
        json_free(root);
        return 0;
    }

    JNode *first   = items->arr.items[0];
    JNode *id_node = jobj_get(first, "id");
    char qobuz_id[64] = {0};
    if (id_node && id_node->type == J_NUM)
        snprintf(qobuz_id, sizeof qobuz_id, "%.0f", id_node->n);
    else
        snprintf(qobuz_id, sizeof qobuz_id, "%s", jstr(id_node));
    json_free(root);

    if (!qobuz_id[0]) { SQT_LOG("qobuz: missing track id in search result"); return 0; }
    SQT_LOG("qobuz: ISRC %s → id %s", isrc, qobuz_id);

    /* 2 ── POST to zarz.moe for the direct stream URL */
    char body[256];
    snprintf(body, sizeof body,
             "{\"quality\":\"hi-res-max\",\"upload_to_r2\":false,"
             "\"url\":\"https://open.qobuz.com/track/%s\"}",
             qobuz_id);

    char *zresp = http_post(ZARZ_URL, body);
    if (!zresp) { SQT_LOG("qobuz: zarz request failed"); return 0; }

    JNode *zroot = json_parse(zresp); free(zresp);
    if (!zroot) { SQT_LOG("qobuz: zarz parse failed"); return 0; }

    JNode *ok_node = jobj_get(zroot, "success");
    if (!ok_node || ok_node->type != J_BOOL || !ok_node->b) {
        SQT_LOG("qobuz: zarz error: %s", jstr(jobj_get(zroot, "error")));
        json_free(zroot);
        return 0;
    }

    const char *dl = jstr(jobj_get(zroot, "download_url"));
    int got = (dl && dl[0]);
    if (got) snprintf(out_url, sz, "%s", dl);
    json_free(zroot);

    if (!got) { SQT_LOG("qobuz: no download_url in zarz response"); return 0; }
    SQT_LOG("qobuz: got stream URL for id %s", qobuz_id);
    return 1;
}
