#define _POSIX_C_SOURCE 200809L
#include "squidget.h"
#include "tag.h"
#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef _WIN32
#  include <io.h>
#  include <windows.h>
#  define access(p,m) _access(p,m)
#else
#  include <unistd.h>
#  include <sys/stat.h>
#endif

static int fetch_cover(const char *cover_path, const char *dest) {
    if (!cover_path || !cover_path[0]) return 0;
    char url[768];
    snprintf(url, sizeof url, "%s/%s/1280x1280.jpg", SQT_TIDAL_IMG, cover_path);
    return (http_get_file(url, dest) > 0) ? 1 : 0;
}

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
        if (c2 >= 0) out[j++] = (char)(((c1 & 0xF) << 4) | (c2 >> 2));
        if (c2 >= 0 && c3 >= 0) out[j++] = (char)(((c2 & 0x3) << 6) | c3);
    }
    out[j] = '\0';
    if (out_len) *out_len = j;
    return out;
}

typedef struct {
    char *urls[2048];
    int   count;
    char  ext[8];
} Manifest;

static void manifest_free(Manifest *m) {
    for (int i = 0; i < m->count; i++) free(m->urls[i]);
    m->count = 0;
}

static int get_manifest(const char *tid, const char *quality, Manifest *m, char *error_buf, size_t error_sz) {
    memset(m, 0, sizeof(*m));
    snprintf(m->ext, sizeof(m->ext), "m4a");

    char url[512];
    snprintf(url, sizeof(url), "%s/track/?id=%s&quality=%s", SQT_BASE_DOWNLOAD, tid, quality);
    
    char *resp = http_get(url);
    if (!resp) {
        if (error_buf) snprintf(error_buf, error_sz, "HTTP request failed");
        return -1;
    }
    
    JNode *root = json_parse(resp); 
    free(resp);
    if (!root) {
        if (error_buf) snprintf(error_buf, error_sz, "Failed to parse JSON");
        return -1;
    }

    JNode *detail = jobj_get(root, "detail");
    if (detail) {
        const char *err = jstr(detail);
        if (error_buf) snprintf(error_buf, error_sz, "API: %s", err);
        json_free(root);
        return -1;
    }

    JNode *data = jobj_get(root, "data");
    if (!data) {
        if (error_buf) snprintf(error_buf, error_sz, "No data field");
        json_free(root);
        return -1;
    }

    const char *b64 = jstr(jobj_get(data, "manifest"));
    if (!*b64) {
        if (error_buf) snprintf(error_buf, error_sz, "No manifest field");
        json_free(root);
        return -1;
    }

    size_t raw_len = 0;
    char *raw = b64_decode(b64, &raw_len);
    if (!raw) {
        if (error_buf) snprintf(error_buf, error_sz, "Base64 decode failed");
        json_free(root);
        return -1;
    }

    JNode *manifest_json = json_parse(raw);
    free(raw);
    
    if (!manifest_json) {
        if (error_buf) snprintf(error_buf, error_sz, "Failed to parse manifest JSON");
        json_free(root);
        return -1;
    }

    JNode *urls_node = jobj_get(manifest_json, "urls");
    if (urls_node && urls_node->type == J_ARR) {
        for (int i = 0; i < urls_node->arr.len && m->count < 2048; i++) {
            const char *seg_url = jstr(urls_node->arr.items[i]);
            if (seg_url && *seg_url) {
                m->urls[m->count++] = strdup(seg_url);
            }
        }
    } else {
        const char *single_url = jstr(jobj_get(manifest_json, "url"));
        if (single_url && *single_url) {
            m->urls[m->count++] = strdup(single_url);
        }
    }

    const char *codec = jstr(jobj_get(manifest_json, "codecs"));
    if (codec && (strstr(codec, "flac") || strstr(codec, "FLAC"))) {
        snprintf(m->ext, sizeof(m->ext), "flac");
    }

    json_free(manifest_json);
    json_free(root);
    
    if (m->count == 0 && error_buf) {
        snprintf(error_buf, error_sz, "No URLs in manifest");
    }
    
    return m->count > 0 ? 0 : -1;
}

static int download_single_url(const char *url, const char *out_path, 
                                void (*progress_cb)(const char *msg, void *ud), void *ud) {
    if (progress_cb) progress_cb("downloading…", ud);
    
    char tmp_path[1200];
    snprintf(tmp_path, sizeof(tmp_path), "%s.sqtmp", out_path);
    
    if (http_get_file(url, tmp_path) < 0) {
        remove(tmp_path);
        return -1;
    }
    
    if (rename(tmp_path, out_path) != 0) {
        remove(tmp_path);
        return -1;
    }
    
    return 0;
}

int download_track(const Track *t, const char *quality,
                   const char *out_dir,
                   const char *preloaded_cover,
                   void (*progress_cb)(const char *msg, void *ud),
                   void *ud) {

    if (progress_cb) progress_cb("fetching manifest…", ud);

    static const char *const quality_order[] = {
        "HI_RES_LOSSLESS", "LOSSLESS", "HIGH", "LOW", "DOLBY_ATMOS", NULL
    };

    Manifest m;
    int got = 0;
    const char *used_quality = quality;
    char last_error[256] = {0};

    for (int i = 0; quality_order[i] && !got; i++) {
        const char *try_q = quality_order[i];
        manifest_free(&m);
        
        if (get_manifest(t->id, try_q, &m, last_error, sizeof(last_error)) == 0) {
            got = 1;
            used_quality = try_q;
            break;
        }
    }

    if (!got) {
        char err_msg[512];
        snprintf(err_msg, sizeof(err_msg), "error: %s", last_error);
        if (progress_cb) progress_cb(err_msg, ud);
        return -1;
    }

    char artist_s[512], title_s[512];
    sqt_sanitise(t->artist, artist_s, sizeof(artist_s));
    sqt_sanitise(t->title,  title_s,  sizeof(title_s));

#ifdef _WIN32
    CreateDirectoryA(out_dir, NULL);
#else
    mkdir(out_dir, 0755);
#endif

    char final_out[1040];
    snprintf(final_out, sizeof(final_out), "%s" SQT_SEP "%s - %s.%s",
             out_dir, artist_s, title_s, m.ext);

    if (access(final_out, F_OK) == 0) {
        char base[1032];
        snprintf(base, sizeof(base), "%s" SQT_SEP "%s - %s",
                 out_dir, artist_s, title_s);
        for (int n = 2; n < 1000; n++) {
            snprintf(final_out, sizeof(final_out), "%s (%d).%s", base, n, m.ext);
            if (access(final_out, F_OK) != 0) break;
        }
    }

    if (m.count != 1) {
        if (progress_cb) progress_cb("error: expected single URL manifest", ud);
        manifest_free(&m);
        return -1;
    }

    if (download_single_url(m.urls[0], final_out, progress_cb, ud) != 0) {
        manifest_free(&m);
        if (progress_cb) progress_cb("error: download failed", ud);
        return -1;
    }

    manifest_free(&m);

    Track rich = *t;
    (void)api_get_track_info(t->id, &rich);

    char cover_tmp[1024] = {0};
    int cover_owned = 0;
    if (preloaded_cover && preloaded_cover[0]) {
        snprintf(cover_tmp, sizeof cover_tmp, "%s", preloaded_cover);
    } else if (rich.cover[0]) {
        snprintf(cover_tmp, sizeof cover_tmp, "%s" SQT_SEP ".sqt_cover_%s.jpg", out_dir, t->id);
        if (!fetch_cover(rich.cover, cover_tmp)) {
            cover_tmp[0] = '\0';
        } else {
            cover_owned = 1;
        }
    }

    if (progress_cb) progress_cb("tagging...", ud);
    int tag_ret = sqt_tag(final_out, &rich, cover_tmp[0] ? cover_tmp : NULL);
    (void)tag_ret;

    if (cover_tmp[0] && cover_owned) remove(cover_tmp);

    if (strcmp(used_quality, quality) != 0) {
        char done_msg[1024];
        snprintf(done_msg, sizeof(done_msg), "done! (used %s)", used_quality);
        if (progress_cb) progress_cb(done_msg, ud);
    } else {
        if (progress_cb) progress_cb("done!", ud);
    }
    
    return 0;
}