#include "squidget.h"
#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ══════════════════════════════════════════════════════════════════════
   HTTP back-end: WinHTTP on Windows, libcurl on Linux / macOS / BSD.
   ══════════════════════════════════════════════════════════════════════ */

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
    wchar_t wu[2048];
    if (!MultiByteToWideChar(CP_UTF8,0,url,-1,wu,(int)(sizeof(wu)/sizeof(*wu)))) return 0;
    URL_COMPONENTS uc; memset(&uc,0,sizeof uc); uc.dwStructSize=sizeof uc;
    wchar_t whost[512]={0},wpath[2048]={0};
    uc.lpszHostName=whost; uc.dwHostNameLength=512;
    uc.lpszUrlPath=wpath;  uc.dwUrlPathLength=2048;
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
    if (WinHttpSendRequest(hr,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0)
        && WinHttpReceiveResponse(hr,NULL)) {
        DWORD s=0,sl=sizeof s;
        WinHttpQueryHeaders(hr,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,NULL,&s,&sl,NULL);
        status=(int)s;
        DWORD avail=0,nr=0;
        while (WinHttpQueryDataAvailable(hr,&avail)&&avail>0) {
            char chunk[65536];
            DWORD want=avail<(DWORD)sizeof chunk?avail:(DWORD)sizeof chunk;
            if (!WinHttpReadData(hr,chunk,want,&nr)||nr==0) break;
            if (fp)   fwrite(chunk,1,nr,fp);
            else if (body&&buf_ensure(body,nr)){memcpy(body->buf+body->len,chunk,nr);body->len+=nr;body->buf[body->len]='\0';}
        }
    }
    WinHttpCloseHandle(hr);WinHttpCloseHandle(hc);WinHttpCloseHandle(hs);
    return status;
}

char *http_get(const char *url) {
    Buf b={malloc(4096),0,4096}; if(!b.buf) return NULL;
    char *res=NULL;
    for (int i=0;i<3;i++) {
        b.len=0; b.buf[0]='\0';
        int s=wh_do(url,&b,NULL,20000);
        if(s>=200&&s<400){res=b.buf;break;}
        if(i<2) sqt_sleep_ms(500*(i+1));
    }
    if(!res) free(b.buf);
    return res;
}

long http_get_file(const char *url, const char *path) {
    FILE *f=fopen(path,"wb"); if(!f) return -1;
    int s=wh_do(url,NULL,f,120000);
    long sz=(long)ftell(f); fclose(f);
    if(s<200||s>=400){remove(path);return -1;}
    return sz;
}

/* ── POSIX: libcurl ── */
#else
#include <curl/curl.h>

static size_t write_cb(void *data,size_t sz,size_t nmemb,void *ud){
    Buf *b=ud; size_t n=sz*nmemb;
    if(!buf_ensure(b,n)) return 0;
    memcpy(b->buf+b->len,data,n); b->len+=n; b->buf[b->len]='\0';
    return n;
}

static void urlencode(const char *in, char *out, size_t outsz) {
    CURL *c=curl_easy_init();
    if(!c){snprintf(out,outsz,"%s",in);return;}
    char *enc=curl_easy_escape(c,in,0);
    snprintf(out,outsz,"%s",enc?enc:in);
    if(enc) curl_free(enc);
    curl_easy_cleanup(c);
}

char *http_get(const char *url) {
    CURL *c=curl_easy_init(); if(!c) return NULL;
    Buf b={malloc(4096),0,4096};
    if(!b.buf){curl_easy_cleanup(c);return NULL;}
    curl_easy_setopt(c,CURLOPT_USERAGENT,     "squidget/2.0");
    curl_easy_setopt(c,CURLOPT_FOLLOWLOCATION,1L);
    curl_easy_setopt(c,CURLOPT_TIMEOUT,       20L);
    curl_easy_setopt(c,CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c,CURLOPT_WRITEDATA,     &b);
    curl_easy_setopt(c,CURLOPT_URL,           url);
    char *res=NULL;
    for (int i=0;i<3;i++) {
        b.len=0;
        if(curl_easy_perform(c)==CURLE_OK){
            long code=0; curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&code);
            if(code>=200&&code<400){res=b.buf;break;}
        }
        if(i<2) sqt_sleep_ms(500*(i+1));
    }
    curl_easy_cleanup(c);
    if(!res) free(b.buf);
    return res;
}

long http_get_file(const char *url, const char *path) {
    FILE *f=fopen(path,"wb"); if(!f) return -1;
    CURL *c=curl_easy_init();
    if(!c){fclose(f);return -1;}
    curl_easy_setopt(c,CURLOPT_URL,           url);
    curl_easy_setopt(c,CURLOPT_WRITEDATA,     f);
    curl_easy_setopt(c,CURLOPT_TIMEOUT,       120L);
    curl_easy_setopt(c,CURLOPT_FOLLOWLOCATION,1L);
    CURLcode rc=curl_easy_perform(c);
    long http_code=0;
    curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&http_code);
    long sz=(long)ftell(f);
    curl_easy_cleanup(c); fclose(f);
    if(rc!=CURLE_OK||http_code<200||http_code>=400){remove(path);return -1;}
    return sz;
}
#endif /* _WIN32 */

/* ══════════════════════════════════════════════════════════════════════
   API layer
   ══════════════════════════════════════════════════════════════════════ */

static char *api_call(const char *path, const char *params) {
    char url[2048];
    if (params && *params)
        snprintf(url, sizeof url, "%s%s?%s", SQT_BASE, path, params);
    else
        snprintf(url, sizeof url, "%s%s", SQT_BASE, path);
    return http_get(url);
}

static void fill_track(JNode *t, Track *out) {
    memset(out, 0, sizeof *out);
    JNode *id = jobj_get(t, "id");
    if (id && id->type == J_NUM) snprintf(out->id, sizeof out->id, "%.0f", id->n);
    else                          snprintf(out->id, sizeof out->id, "%s",   jstr(id));
    snprintf(out->title, sizeof out->title, "%s", jstr(jobj_get(t,"title")));

    JNode *artists = jobj_get(t, "artists");
    if (artists && artists->type == J_ARR && artists->arr.len > 0) {
        JNode *a0 = artists->arr.items[0];
        snprintf(out->artist, sizeof out->artist, "%s", jstr(jobj_get(a0,"name")));
    } else {
        JNode *ao = jobj_get(t, "artist");
        if (ao && ao->type == J_OBJ)
            snprintf(out->artist, sizeof out->artist, "%s", jstr(jobj_get(ao,"name")));
        else
            snprintf(out->artist, sizeof out->artist, "%s", jstr(jobj_get(t,"artist")));
    }

    JNode *album = jobj_get(t, "album");
    if (album && album->type == J_OBJ)
        snprintf(out->album, sizeof out->album, "%s", jstr(jobj_get(album,"title")));

    out->duration = (int)jnum(jobj_get(t, "duration"));
    snprintf(out->quality, sizeof out->quality, "%s", jstr(jobj_get(t,"audioQuality")));
}

int api_search_tracks(const char *query, Track *out, int max) {
    char enc[512], params[600];
    urlencode(query, enc, sizeof enc);
    snprintf(params, sizeof params, "s=%s&limit=%d", enc, max);
    char *resp = api_call("/search/", params);
    if (!resp) return 0;
    JNode *root = json_parse(resp); free(resp);
    if (!root) return 0;
    JNode *data  = jobj_get(root, "data");
    JNode *items = data ? jobj_get(data, "items") : NULL;
    if (!items && data && data->type == J_ARR) items = data;
    int cnt = 0;
    if (items && items->type == J_ARR) {
        int n = items->arr.len < max ? items->arr.len : max;
        for (int i = 0; i < n; i++) fill_track(items->arr.items[i], &out[cnt++]);
    }
    json_free(root);
    return cnt;
}
