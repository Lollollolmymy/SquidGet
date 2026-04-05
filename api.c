#include "squidget.h"
#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// quality names
const char *const QUALITY_LABELS[QUALITY_COUNT] = {
    QUAL_HIR, QUAL_LOS, QUAL_HIGH, QUAL_LOW, QUAL_ATM
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
    // handle huge tidal urls w/ big buffer
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
            char chunk[65536];
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

char *http_get(const char *url) {
    Buf b={malloc(4096),0,4096}; if(!b.buf) return NULL;
    char *res=NULL;
    // retry 3x with backoff
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
    FILE *f=fopen(path,"wb");
    if(!f){fprintf(stderr,"[squidget] fopen failed: %s\n",path);return -1;}
    int s=wh_do(url,NULL,f,120000);
    long sz=(long)ftell(f); fclose(f);
    if(s==0){remove(path);fprintf(stderr,"[squidget] WinHTTP connection failed (URL too long or TLS error?)\n");return -1;}
    if(s<200||s>=400){remove(path);fprintf(stderr,"[squidget] HTTP %d for download URL\n",s);return -1;}
    return sz;
}

// posix: use curl cli
#else

static void urlencode(const char *in, char *out, size_t outsz) {
    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && j + 4 < outsz; p++) {
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
    char cmd[8192];  /* esc_url up to 4095 + fixed prefix ~57 chars */
    char *res = NULL;
    for (int i = 0; i < 3; i++) {
        snprintf(cmd, sizeof cmd,
                 "curl -sSL --max-time 20 -A 'squidget/2.0' '%s' 2>/dev/null",
                 esc_url);
        res = popen_read(cmd);
        if (res) break;
        if (i < 2) sqt_sleep_ms(500 * (i + 1));
    }
    return res;
}

long http_get_file(const char *url, const char *path) {
    char esc_url[4096], esc_path[2048];
    shell_escape(url,  esc_url,  sizeof esc_url);
    shell_escape(path, esc_path, sizeof esc_path);
    char cmd[8192];
    snprintf(cmd, sizeof cmd,
             "curl -sSL --max-time 120 -A 'squidget/2.0' -o '%s' '%s'",
             esc_path, esc_url);
    FILE *p = popen(cmd, "r");
    if (!p) { remove(path); return -1; }
    int curl_ret = pclose(p);
    if (curl_ret != 0) { remove(path); return -1; }  /* curl failed (404, timeout, etc.) */
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    return sz > 0 ? sz : -1;
}
#endif /* _WIN32 */

// api stuff

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
    snprintf(out->quality, sizeof out->quality, "%s", jstr(jobj_get(t, "audioQuality")));
}

int api_search_tracks(const char *query, Track *out, int max) {
    char enc[1600], params[1620];  /* enc: 512 chars * 3 bytes/char URL-encoded + headroom; params: "s=" + enc + "&limit=XX" */
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

// parse album info from json
static void fill_album(JNode *a, Album *out) {
    memset(out, 0, sizeof *out);
    JNode *id = jobj_get(a, "id");
    if (id && id->type == J_NUM) snprintf(out->id, sizeof out->id, "%.0f", id->n);
    else                          snprintf(out->id, sizeof out->id, "%s",   jstr(id));
    snprintf(out->title,  sizeof out->title,  "%s", jstr(jobj_get(a, "title")));

    JNode *artists = jobj_get(a, "artists");
    if (artists && artists->type == J_ARR && artists->arr.len > 0)
        snprintf(out->artist, sizeof out->artist, "%s",
                 jstr(jobj_get(artists->arr.items[0], "name")));
    else
        snprintf(out->artist, sizeof out->artist, "?");

    JNode *nt = jobj_get(a, "numberOfTracks");
    out->num_tracks = nt ? (int)jnum(nt) : 0;
}

// search albums
int api_search_albums(const char *query, Album *out, int max) {
    char enc[1600], params[1640];
    urlencode(query, enc, sizeof enc);
    snprintf(params, sizeof params, "al=%s&limit=%d", enc, max);
    char *resp = api_call("/search/", params);
    if (!resp) return 0;
    JNode *root = json_parse(resp); free(resp);
    if (!root) return 0;

    JNode *data  = jobj_get(root, "data");
    JNode *alb   = data ? jobj_get(data, "albums") : NULL;
    JNode *items = alb  ? jobj_get(alb,  "items")  : NULL;

    int cnt = 0;
    if (items && items->type == J_ARR) {
        int n = items->arr.len < max ? items->arr.len : max;
        for (int i = 0; i < n; i++)
            fill_album(items->arr.items[i], &out[cnt++]);
    }
    json_free(root);
    return cnt;
}

// get all tracks in an album
int api_get_album_tracks(const char *album_id, Track *out, int max) {
    char params[128];
    snprintf(params, sizeof params, "id=%s", album_id);
    char *resp = api_call("/album/", params);
    if (!resp) return 0;

    JNode *root = json_parse(resp); free(resp);
    if (!root) return 0;

    /* root = {data: {items: [{item:{...}}, ...] } } or [array_format] for backwards compat */
    JNode *items = NULL;
    
    /* Try new format: { data: { items: [...] } } */
    if (root->type == J_OBJ) {
        JNode *data = jobj_get(root, "data");
        if (data && data->type == J_OBJ)
            items = jobj_get(data, "items");
    }
    /* Fallback to old format: [album_meta, tracks_obj] */
    else if (root->type == J_ARR && root->arr.len >= 2) {
        JNode *tobj = root->arr.items[1];
        if (tobj && tobj->type == J_OBJ)
            items = jobj_get(tobj, "items");
    }

    int cnt = 0;
    if (items && items->type == J_ARR) {
        int n = items->arr.len < max ? items->arr.len : max;
        for (int i = 0; i < n; i++) {
            JNode *wrap  = items->arr.items[i];
            JNode *track = jobj_get(wrap, "item");
            if (!track) track = wrap;
            fill_track(track, &out[cnt++]);
        }
    }
    json_free(root);
    return cnt;
}
