#define _POSIX_C_SOURCE 200809L
#include "squidget.h"
#include "tag.h"
#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef _WIN32
#  include <io.h>       /* _access */
#  include <windows.h>  /* CreateDirectoryA */
#  define access(p,m) _access(p,m)
#else
#  include <unistd.h>   /* access  */
#  include <sys/stat.h> /* mkdir   */
#endif

/* ── Cover art download ─────────────────────────────────────────────────────
 *  cover_path is the UUID-based CDN path "ab/cd/ef/…" stored in Track.cover
 *  dest is a writable temp file path.  Returns 1 on success, 0 on failure.  */
static int fetch_cover(const char *cover_path, const char *dest) {
    if (!cover_path || !cover_path[0]) return 0;
    char url[768];
    snprintf(url, sizeof url, "%s/%s/1280x1280.jpg", SQT_TIDAL_IMG, cover_path);
    return (http_get_file(url, dest) > 0) ? 1 : 0;
}

// clean up filename or dir name (remove illegal chars)
// non-static so main.c can sanitise album_name before mkdir
void sqt_sanitise(const char *in, char *out, size_t outsz) {
    size_t j = 0;
    for (const char *p = in; *p && j + 1 < outsz; p++) {
        char c = *p;
        if (c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"'  || c == '<' || c == '>' || c == '|')
            c = '_';
        out[j++] = c;
    }
    out[j] = '\0';
}

// base64 decode
static int b64_val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
static char *b64_decode(const char *in, size_t *out_len) {
    size_t ilen = strlen(in);
    char *out = malloc(ilen / 4 * 3 + 4);
    if (!out) return NULL;
    size_t j = 0;
    size_t i = 0;
    while (i < ilen) {
        /* skip whitespace before each char (handles non-standard encoders) */
        while (i < ilen && (in[i]==' '||in[i]=='\r'||in[i]=='\n'||in[i]=='\t')) i++;
        if (i >= ilen) break;

        int c0 = b64_val((unsigned char)in[i++]);
        
        while (i < ilen && (in[i]==' '||in[i]=='\r'||in[i]=='\n'||in[i]=='\t')) i++;
        int c1 = (i < ilen && in[i] != '=') ? b64_val((unsigned char)in[i++]) : (i++, -1);
        
        while (i < ilen && (in[i]==' '||in[i]=='\r'||in[i]=='\n'||in[i]=='\t')) i++;
        int c2 = (i < ilen && in[i] != '=') ? b64_val((unsigned char)in[i++]) : (i++, -1);
        
        while (i < ilen && (in[i]==' '||in[i]=='\r'||in[i]=='\n'||in[i]=='\t')) i++;
        int c3 = (i < ilen && in[i] != '=') ? b64_val((unsigned char)in[i++]) : (i++, -1);

        if (c0 < 0 || c1 < 0) break;
        out[j++] = (char)((c0 << 2) | (c1 >> 4));
        /* Bug 7 fix: only write c2/c3 bytes when those fields are valid */
        if (c2 >= 0) out[j++] = (char)(((c1 & 0xF) << 4) | (c2 >> 2));
        if (c2 >= 0 && c3 >= 0) out[j++] = (char)(((c2 & 0x3) << 6) | c3);
    }
    out[j] = '\0';
    if (out_len) *out_len = j;
    return out;
}

// figure out file type from magic bytes
static void detect_ext(const char *path, char *ext, size_t extsz) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(ext, extsz, "flac"); return; }
    unsigned char h[12] = {0};
    size_t _nr = fread(h, 1, sizeof(h), f); (void)_nr;
    fclose(f);
    if (h[0]=='f'&&h[1]=='L'&&h[2]=='a'&&h[3]=='C')         { snprintf(ext,extsz,"flac"); return; }
    if (h[4]=='f'&&h[5]=='t'&&h[6]=='y'&&h[7]=='p')          { snprintf(ext,extsz,"m4a");  return; }
    if ((h[0]=='I'&&h[1]=='D'&&h[2]=='3')||(h[0]==0xFF&&(h[1]&0xE0)==0xE0))
                                                               { snprintf(ext,extsz,"mp3");  return; }
    if (h[0]=='O'&&h[1]=='g'&&h[2]=='g'&&h[3]=='S')          { snprintf(ext,extsz,"ogg");  return; }
    snprintf(ext, extsz, "flac");
}

// unescape HTML entities in URLs extracted from XML manifests
static void unescape_xml_entities(char *str) {
    if (!str) return;
    size_t i = 0, j = 0;
    char temp[8192];
    while (str[i] && j < sizeof(temp) - 1) {
        if (str[i] == '&' && i + 4 < strlen(str)) {
            if (strncmp(&str[i], "&amp;", 5) == 0) {
                temp[j++] = '&';
                i += 5;
                continue;
            }
            if (strncmp(&str[i], "&lt;", 4) == 0) {
                temp[j++] = '<';
                i += 4;
                continue;
            }
            if (strncmp(&str[i], "&gt;", 4) == 0) {
                temp[j++] = '>';
                i += 4;
                continue;
            }
            if (strncmp(&str[i], "&quot;", 6) == 0) {
                temp[j++] = '"';
                i += 6;
                continue;
            }
            if (strncmp(&str[i], "&apos;", 6) == 0) {
                temp[j++] = '\'';
                i += 6;
                continue;
            }
        }
        temp[j++] = str[i++];
    }
    temp[j] = '\0';
    snprintf(str, strlen(str) + 1, "%s", temp);  /* ensure no overflow */
}

// dash manifest parser (extracts segment urls)
#define MAX_SEGS 2048
typedef struct {
    char *urls[MAX_SEGS];
    int   count;
    int   is_single;
    char  ext[8];
} Manifest;

// free manifest resources
static void manifest_free(Manifest *m) {
    for (int i = 0; i < m->count; i++) free(m->urls[i]);
    m->count = 0;
}

static void parse_dash(const char *xml, Manifest *m) {
    const char *st = strstr(xml, "SegmentTemplate");
    if (st) {
        char media_tpl[1024] = {0};
        const char *mp = strstr(st, " media=\"");
        if (!mp) mp = strstr(st, "\tmedia=\"");
        if (!mp) mp = strstr(st, "\nmedia=\"");
        if (mp) {
            mp += 8;
            const char *me = strchr(mp, '"');
            if (me) { size_t l = (size_t)(me-mp); if (l<sizeof(media_tpl)) { memcpy(media_tpl,mp,l); media_tpl[l]='\0'; } }
        }

        char init_url[1024] = {0};
        const char *ip = strstr(st, "initialization=\"");
        if (ip) {
            ip += 16;
            const char *ie = strchr(ip, '"');
            if (ie) { size_t l=(size_t)(ie-ip); if(l<sizeof(init_url)){memcpy(init_url,ip,l);init_url[l]='\0';} }
        }

        int seg_num = 1;
        const char *snp = strstr(st, "startNumber=\"");
        if (snp) { snp += 13; seg_num = atoi(snp); }

        char rep_id[64] = {0};
        const char *rid = strstr(xml, "Representation ");
        if (rid) {
            const char *rip = strstr(rid, " id=\"");
            if (rip) { rip += 5; const char *rie=strchr(rip,'"'); if(rie){size_t l=(size_t)(rie-rip)<63?(size_t)(rie-rip):63; memcpy(rep_id,rip,l);rep_id[l]='\0';} }
        }

        /* fill template helper */
        #define FILL(tpl,num,rid_s,out,outsz) do { \
            char _t[1024]; snprintf(_t,sizeof(_t),"%s",(tpl)); \
            char _n[32]; snprintf(_n,sizeof(_n),"%d",(num)); \
            char *_np=strstr(_t,"$Number$"); \
            if(_np){char _r[1024];size_t _p=(size_t)(_np-_t);snprintf(_r,sizeof(_r),"%.*s%s%s",(int)_p,_t,_n,_np+8);snprintf(_t,sizeof(_t),"%s",_r);} \
            char *_rp=strstr(_t,"$RepresentationID$"); \
            if(_rp&&*(rid_s)){char _r[1024];size_t _p=(size_t)(_rp-_t);snprintf(_r,sizeof(_r),"%.*s%s%s",(int)_p,_t,(rid_s),_rp+18);snprintf(_t,sizeof(_t),"%s",_r);} \
            snprintf(out,outsz,"%s",_t); \
        } while(0)

        if (*init_url && m->count < MAX_SEGS) {
            char f[1024]; FILL(init_url,seg_num,rep_id,f,sizeof(f));
            unescape_xml_entities(f);
            char *seg = strdup(f);
            if (!seg) return;  /* strdup failed — stop parsing */
            m->urls[m->count++] = seg;
        }

        const char *stp = strstr(st, "SegmentTimeline");
        if (stp && *media_tpl) {
            const char *sp = stp;
            while ((sp = strstr(sp, "<S ")) && m->count < MAX_SEGS) {
                int rpts = 0;
                const char *rp = strstr(sp, " r=\"");
                const char *end = strstr(sp, "/>");
                if (rp && end && rp < end) rpts = atoi(rp + 4);
                for (int ri = 0; ri <= rpts && m->count < MAX_SEGS; ri++) {
                    char f[1024]; FILL(media_tpl,seg_num,rep_id,f,sizeof(f));
                    unescape_xml_entities(f);
                    char *seg = strdup(f);
                    if (!seg) return;  /* strdup failed — stop parsing */
                    m->urls[m->count++] = seg;
                    seg_num++;
                }
                sp += 3;
            }
        }
        #undef FILL
        if (m->count > 0) return;
    }

    /* BaseURL fallback */
    const char *p = xml;
    while ((p = strstr(p, "<BaseURL>")) && m->count < MAX_SEGS) {
        p += 9;
        const char *e = strstr(p, "</BaseURL>");
        if (!e) break;
        size_t l = (size_t)(e - p);
        char *seg = malloc(l + 1);
        if (!seg) break;
        memcpy(seg, p, l); seg[l] = '\0';
        unescape_xml_entities(seg);
        m->urls[m->count++] = seg;
        p = e + 10;
    }
}

// download file bytes
static int get_manifest(const char *tid, const char *quality, Manifest *m) {
    memset(m, 0, sizeof(*m));
    snprintf(m->ext, sizeof(m->ext), "flac");

    char url[512];
    snprintf(url, sizeof(url), "%s/track/?id=%s&quality=%s", SQT_BASE_DOWNLOAD, tid, quality);
    SQT_LOG("Fetching manifest from: %s", url);
    char *resp = http_get(url);
    if (!resp) { SQT_LOG("http_get returned NULL for: %s", url); return -1; }
    
    SQT_LOG("Manifest response (first 500 chars): %.500s", resp);
    SQT_LOG("Manifest response received, parsing JSON...");
    JNode *root = json_parse(resp); free(resp);
    if (!root) { SQT_LOG("json_parse failed"); return -1; }

    JNode *data = jobj_get(root, "data");
    if (!data) { SQT_LOG("no 'data' field in response"); json_free(root); return -1; }

    const char *b64 = jstr(jobj_get(data, "manifest"));
    if (!*b64) { SQT_LOG("no 'manifest' field in data"); json_free(root); return -1; }
    SQT_LOG("Base64 manifest (first 100 chars): %.100s", b64);

    size_t raw_len = 0;
    char *raw = b64_decode(b64, &raw_len);
    if (!raw) { SQT_LOG("b64_decode failed"); json_free(root); return -1; }
    SQT_LOG("Decoded manifest (first 300 chars): %.*s", (int)((raw_len > 300) ? 300 : raw_len), raw);

    const char *mime = jstr(jobj_get(data, "manifestMimeType"));

    if (strstr(mime, "bts") || (raw_len > 0 && raw[0] == '{')) {
        /* BTS: single pre-signed URL */
        SQT_LOG("Detected BTS format");
        JNode *bts = json_parse(raw);
        if (bts) {
            const char *u = NULL;
            JNode *urls = jobj_get(bts, "urls");
            if (urls && urls->type == J_ARR && urls->arr.len > 0)
                u = jstr(urls->arr.items[0]);
            if (!u || !*u) u = jstr(jobj_get(bts, "url"));
            
            SQT_LOG("BTS URL: %s", u ? u : "NULL");

            const char *codec = jstr(jobj_get(bts, "codec"));
            if (codec && (strstr(codec,"m4a")||strstr(codec,"M4A")||
                          strstr(codec,"aac")||strstr(codec,"AAC")||
                          strstr(codec,"mp4")||strstr(codec,"alac")||strstr(codec,"ALAC")))
                snprintf(m->ext, sizeof(m->ext), "m4a");
            else
                snprintf(m->ext, sizeof(m->ext), "flac");

            if (u && *u) m->urls[m->count++] = strdup(u);
            json_free(bts);
        }
        m->is_single = 1;
    } else {
        /* DASH MPD */
        parse_dash(raw, m);
        snprintf(m->ext, sizeof(m->ext), "m4a");
    }

    free(raw);
    json_free(root);
    return m->count > 0 ? 0 : -1;
}

// concat segments (raw bytes)
static int cat_segments(char *const *urls, int count, const char *out_path,
                         void (*cb)(const char *, void *), void *ud) {
    FILE *out = fopen(out_path, "wb");
    if (!out) return -1;

    // download each segment

    for (int i = 0; i < count; i++) {
        if (cb) {
            char msg[64];
            snprintf(msg, sizeof(msg), "downloading %d/%d…", i + 1, count);
            cb(msg, ud);
        }

        char tmp[1200];
        snprintf(tmp, sizeof(tmp), "%s.seg%04d", out_path, i);
        if (http_get_file(urls[i], tmp) < 0) {
            remove(tmp);
            fclose(out); remove(out_path); return -1;
        }

        FILE *in = fopen(tmp, "rb");
        if (!in) { fclose(out); remove(out_path); remove(tmp); return -1; }
        char buf[16384]; size_t n;  /* replaced 65536 with 16KB heap-safe buffer */
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
            if (fwrite(buf, 1, n, out) != n) {
                fclose(in); fclose(out);
                remove(out_path); remove(tmp);
                return -1;
            }
        }
        fclose(in);
        remove(tmp);
    }

    fclose(out);
    return 0;
}

// download a track
// preloaded_cover: path to a cover JPEG that's already on disk (album mode, shared
//   across all tracks so we don't hammer the CDN 39x).  NULL = fetch per-track as usual.
int download_track(const Track *t, const char *quality,
                   const char *out_dir,
                   const char *preloaded_cover,
                   void (*progress_cb)(const char *msg, void *ud),
                   void *ud) {

    if (progress_cb) progress_cb("fetching manifest…", ud);

    // try best quality first, then fall back to lower ones
    static const char *const fallback[] = {
        "HI_RES_LOSSLESS", "LOSSLESS", "HIGH", "LOW", "DOLBY_ATMOS", NULL
    };

    Manifest m;
    int got = 0;
    const char *used_quality = quality;

    /* try requested quality first */
    if (get_manifest(t->id, quality, &m) == 0) {
        got = 1;
    } else {
        /* fall back through lower qualities */
        for (int i = 0; fallback[i] && !got; i++) {
            if (strcmp(fallback[i], quality) == 0) continue; /* already tried */
            manifest_free(&m);
            if (get_manifest(t->id, fallback[i], &m) == 0) {
                got = 1;
                used_quality = fallback[i];   /* track actual quality used */
            }
        }
    }

    if (!got) {
        if (progress_cb) progress_cb("error: no quality available for this track", ud);
        return -1;
    }

    /* build output filename base (extension finalised after detection) */
    char artist_s[512], title_s[512];  /* replaced VLA SQT_TITLE_SZ */
    sqt_sanitise(t->artist, artist_s, sizeof(artist_s));
    sqt_sanitise(t->title,  title_s,  sizeof(title_s));

    int ok = 0;  /* 0 = success, -1 = failure */
    char final_out[1040]  /* base[1024] + " (999).ext" = 1035 max */;

    if (m.is_single) {
        /* Download to a temp path first, detect magic bytes, rename.
           This ensures the on-disk extension always matches the container. */
        char tmp_path[1024];
        snprintf(tmp_path, sizeof(tmp_path), "%s" SQT_SEP "%s - %s.sqtmp",
                 out_dir, artist_s, title_s);

        if (progress_cb) progress_cb("downloading…", ud);
        /* ensure output dir exists — may have been deleted since startup */
#ifdef _WIN32
        CreateDirectoryA(out_dir, NULL);
#else
        mkdir(out_dir, 0755);
#endif
        if (http_get_file(m.urls[0], tmp_path) < 0) {
            remove(tmp_path);
            manifest_free(&m);
            if (progress_cb) progress_cb("error: download failed", ud);
            return -1;
        }

        /* detect real container from magic bytes */
        detect_ext(tmp_path, m.ext, sizeof(m.ext));

        /* build final filename with correct extension */
        snprintf(final_out, sizeof(final_out), "%s" SQT_SEP "%s - %s.%s",
                 out_dir, artist_s, title_s, m.ext);

        /* avoid overwriting existing files — append counter suffix */
        if (access(final_out, F_OK) == 0) {
            char base[1032];   /* 512(out_dir)+1+256(artist)+3+256(title) = 1028 max + headroom */
            snprintf(base, sizeof(base), "%s" SQT_SEP "%s - %s",
                     out_dir, artist_s, title_s);
            for (int n = 2; n < 1000; n++) {
                snprintf(final_out, sizeof(final_out), "%s (%d).%s", base, n, m.ext);
                if (access(final_out, F_OK) != 0) break;
            }
        }

        if (rename(tmp_path, final_out) != 0) {
            remove(tmp_path);
            manifest_free(&m);
            if (progress_cb) progress_cb("error: could not rename temp file", ud);
            return -1;
        }
        /* Single file downloaded successfully */
        ok = 0;
    } else {
        /* DASH — concatenate segments */
        /* ensure output dir exists */
#ifdef _WIN32
        CreateDirectoryA(out_dir, NULL);
#else
        mkdir(out_dir, 0755);
#endif
        snprintf(final_out, sizeof(final_out), "%s" SQT_SEP "%s - %s.%s",
                 out_dir, artist_s, title_s, m.ext);

        /* avoid overwriting */
        if (access(final_out, F_OK) == 0) {
            char base[1032];   /* 512(out_dir)+1+256(artist)+3+256(title) = 1028 max + headroom */
            snprintf(base, sizeof(base), "%s" SQT_SEP "%s - %s",
                     out_dir, artist_s, title_s);
            for (int n = 2; n < 1000; n++) {
                snprintf(final_out, sizeof(final_out), "%s (%d).%s", base, n, m.ext);
                if (access(final_out, F_OK) != 0) break;
            }
        }

        ok = cat_segments(m.urls, m.count, final_out, progress_cb, ud);
    }

    manifest_free(&m);

    if (ok != 0) {
        SQT_LOG("download failed for '%s - %s'", t->artist, t->title);
        if (progress_cb) progress_cb("error: download failed", ud);
        return -1;
    }

    /* log downloaded file size */
    {
        FILE *_f = fopen(final_out, "rb");
        if (_f) {
            fseek(_f, 0, SEEK_END); long _sz = ftell(_f); fclose(_f);
            SQT_LOG("download ok  file='%s'  size=%ld bytes", final_out, _sz);
        } else {
            SQT_LOG("WARNING: cannot open final file to check size: %s", final_out);
        }
    }

    /* ── Fetch extended metadata from /info/ ── */
    if (progress_cb) progress_cb("fetching metadata...", ud);
    SQT_LOG("fetching extended metadata  track_id='%s'", t->id);
    Track rich = *t;
    int info_ok = api_get_track_info(t->id, &rich);
    SQT_LOG("api_get_track_info => %d", info_ok);
    SQT_LOG("  title     = '%s'", rich.title);
    SQT_LOG("  artist    = '%s'", rich.artist);
    SQT_LOG("  album     = '%s'", rich.album);
    SQT_LOG("  year      = '%s'", rich.year);
    SQT_LOG("  track_num = %d  disc_num = %d", rich.track_num, rich.disc_num);
    SQT_LOG("  isrc      = '%s'", rich.isrc);
    SQT_LOG("  copyright = '%s'", rich.copyright);
    SQT_LOG("  cover     = '%s'", rich.cover);
    SQT_LOG("  quality   = '%s'", rich.quality);
    SQT_LOG("  rg_gain   = %.4f  explicit = %d", rich.replay_gain, rich.explicit_);

    /* ── Download cover art ── */
    char cover_tmp[1024] = {0};
    int  cover_owned = 0;  /* 1 = we fetched it, we must delete it after tagging */
    if (preloaded_cover && preloaded_cover[0]) {
        /* album mode: caller already fetched the cover once — just reuse it */
        snprintf(cover_tmp, sizeof cover_tmp, "%s", preloaded_cover);
        SQT_LOG("using preloaded cover  path='%s'", cover_tmp);
    } else if (rich.cover[0]) {
        snprintf(cover_tmp, sizeof cover_tmp,
                 "%s" SQT_SEP ".sqt_cover_%s.jpg", out_dir, t->id);
        SQT_LOG("fetching cover  cover_path='%s'  dest='%s'", rich.cover, cover_tmp);
        if (!fetch_cover(rich.cover, cover_tmp)) {
            SQT_LOG("cover fetch FAILED — tagging without art");
            cover_tmp[0] = '\0';
        } else {
            FILE *_cf = fopen(cover_tmp, "rb");
            long _csz = 0;
            if (_cf) { fseek(_cf, 0, SEEK_END); _csz = ftell(_cf); fclose(_cf); }
            SQT_LOG("cover art ok  size=%ld bytes", _csz);
            cover_owned = 1;
        }
    } else {
        SQT_LOG("no cover URL in metadata — tagging without art");
    }

    /* ── Tag ── */
    if (progress_cb) progress_cb("tagging...", ud);
    SQT_LOG("calling sqt_tag  file='%s'  cover='%s'",
            final_out, cover_tmp[0] ? cover_tmp : "(none)");
    int tag_ret = sqt_tag(final_out, &rich, cover_tmp[0] ? cover_tmp : NULL);
    SQT_LOG("sqt_tag returned %d", tag_ret);
    if (tag_ret != 0)
        SQT_LOG("WARNING: tagging failed — file still saved without tags");
    else
        SQT_LOG("tagging completed successfully");

    if (cover_tmp[0] && cover_owned) remove(cover_tmp);

    /* report actual quality if we fell back */
    if (strcmp(used_quality, quality) != 0) {
        char done_msg[1024];  /* replaced VLA SQT_STATUS_SZ */
        int ret = snprintf(done_msg, sizeof(done_msg), "done! (fell back to %s)", used_quality);
        if (ret >= (int)sizeof(done_msg)) SQT_LOG("snprintf truncated done_msg");
        if (progress_cb) progress_cb(done_msg, ud);
    } else {
        if (progress_cb) progress_cb("done!", ud);
    }
    SQT_LOG("download_track finished  file='%s'", final_out);
    return 0;
}
