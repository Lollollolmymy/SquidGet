/* tag.c — squidget native FLAC + M4A/MP4 tagger
 *
 * TCC/Windows ABI note: TCC on x64 Windows cannot reliably return structs
 * larger than 8 bytes by value (the Bb struct is 24 bytes).  Every builder
 * function therefore takes a  Bb *out  parameter and writes into it
 * directly — nothing is ever returned by value.
 */

#define _POSIX_C_SOURCE 200809L
#include "squidget.h"
#include "tag.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
   /* SQT_SEP already defined in squidget.h */
#else
#  include <unistd.h>
   /* SQT_SEP already defined in squidget.h */
#endif

/* ══════════════════════════ dynamic byte buffer ══════════════════════════ */

typedef struct { uint8_t *d; size_t len, cap; } Bb;

static int bb_need(Bb *b, size_t n) {
    if (b->len + n <= b->cap) return 1;
    size_t nc = (b->len + n) * 2 + 256;
    uint8_t *p = realloc(b->d, nc);
    if (!p) return 0;
    b->d = p; b->cap = nc; return 1;
}

static void bb_free(Bb *b) { free(b->d); b->d = NULL; b->len = b->cap = 0; }

static int bb_put(Bb *b, const void *src, size_t n) {
    if (!bb_need(b, n)) return 0;
    memcpy(b->d + b->len, src, n); b->len += n; return 1;
}

static int bb_u8(Bb *b, uint8_t v) { return bb_put(b, &v, 1); }

static int bb_be16(Bb *b, uint16_t v) {
    uint8_t p[2]; p[0]=(uint8_t)(v>>8); p[1]=(uint8_t)v;
    return bb_put(b,p,2);
}

static int bb_be32(Bb *b, uint32_t v) {
    uint8_t p[4]; p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16);
    p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v;
    return bb_put(b,p,4);
}

static int bb_le32(Bb *b, uint32_t v) {
    uint8_t p[4]; p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8);
    p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
    return bb_put(b,p,4);
}

static void bb_patch_be32(Bb *b, size_t pos, uint32_t v) {
    if (pos + 4 > b->len) return;
    b->d[pos]=(uint8_t)(v>>24); b->d[pos+1]=(uint8_t)(v>>16);
    b->d[pos+2]=(uint8_t)(v>>8); b->d[pos+3]=(uint8_t)v;
}

/* ══════════════════════════ raw integer readers / writers ════════════════ */

static uint32_t rd32be(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}

static uint64_t rd64be(const uint8_t *p) {
    return ((uint64_t)rd32be(p)<<32)|rd32be(p+4);
}

static void wr32be(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v;
}

static void wr64be(uint8_t *p, uint64_t v) {
    wr32be(p,(uint32_t)(v>>32)); wr32be(p+4,(uint32_t)v);
}

/* ══════════════════════════ ID3v2 helpers ═══════════════════════════════ */

static void encode_syncsafe(uint8_t out[4], uint32_t val) {
    out[3] = (uint8_t)(val & 0x7F); val >>= 7;
    out[2] = (uint8_t)(val & 0x7F); val >>= 7;
    out[1] = (uint8_t)(val & 0x7F); val >>= 7;
    out[0] = (uint8_t)(val & 0x7F);
}

static size_t id3v2_total_size(const uint8_t *data, size_t fsz) {
    if (fsz < 10 || data[0]!='I' || data[1]!='D' || data[2]!='3') return 0;
    uint32_t sz = ((uint32_t)(data[6] & 0x7F) << 21)
                | ((uint32_t)(data[7] & 0x7F) << 14)
                | ((uint32_t)(data[8] & 0x7F) <<  7)
                |  (uint32_t)(data[9] & 0x7F);
    return 10 + (size_t)sz;
}

static void build_id3v2_apic(Bb *b, const uint8_t *img, size_t img_sz) {
    const char *mime = (img_sz >= 4 &&
                        img[0]==0x89 && img[1]=='P' &&
                        img[2]=='N'  && img[3]=='G')
                       ? "image/png" : "image/jpeg";
    size_t mime_len = strlen(mime);
    uint32_t apic_content = (uint32_t)(1 + mime_len + 1 + 1 + 1 + img_sz);
    uint32_t tag_payload = 10 + apic_content;

    bb_put(b, "ID3", 3);
    bb_u8(b, 0x03);
    bb_u8(b, 0x00);
    bb_u8(b, 0x00);
    uint8_t ss[4];
    encode_syncsafe(ss, tag_payload);
    bb_put(b, ss, 4);
    bb_put(b, "APIC", 4);
    bb_be32(b, apic_content);
    bb_be16(b, 0x0000);
    bb_u8(b, 0x00);
    bb_put(b, mime, mime_len);
    bb_u8(b, 0x00);
    bb_u8(b, 0x03);
    bb_u8(b, 0x00);
    bb_put(b, img, img_sz);
}

/* ══════════════════════════ file helpers ════════════════════════════════ */

static uint8_t *file_read(const char *path, size_t *out_sz) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f,0,SEEK_END); long l = ftell(f); rewind(f);
    if (l <= 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)l + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)l, f) != (size_t)l) { free(buf); buf = NULL; }
    else { buf[l] = 0; *out_sz = (size_t)l; }
    fclose(f);
    return buf;
}

static int file_write_atomic(const char *path, const uint8_t *d, size_t n) {
    SQT_LOG("file_write_atomic: writing %zu bytes to %s", n, path);
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.sqttmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { SQT_LOG("file_write_atomic: cannot open temp file %s", tmp); return 0; }
    int ok = ((size_t)fwrite(d, 1, n, f) == n);
    fclose(f);
    if (!ok) { remove(tmp); return 0; }
#ifdef _WIN32
    remove(path);
    int ret = MoveFileA(tmp, path) != 0;
    if (!ret) SQT_LOG("file_write_atomic: MoveFileA failed le=%lu", GetLastError());
    return ret;
#else
    int ret = rename(tmp, path) == 0;
    if (!ret) SQT_LOG("file_write_atomic: rename failed");
    return ret;
#endif
}

/* ══════════════════════════ FLAC tagger (#3 streaming) ══════════════════════
 *
 * Old approach: file_read() loads the ENTIRE audio file into RAM.
 *   24-bit 96 kHz FLAC ≈ 300–500 MB peak resident.
 *
 * New approach: read only the small metadata blocks into RAM (STREAMINFO=34B,
 *   SEEKTABLE=few KB, etc.).  Audio bytes are streamed file→temp in 64 KB
 *   chunks so peak RSS is ~2 MB regardless of FLAC size (~500× improvement).
 */

#define FLAC_VORBIS_COMMENT 4
#define FLAC_PICTURE        6

/* Per-block storage for blocks we must preserve (STREAMINFO, SEEKTABLE, …). */
typedef struct { int type; uint8_t *data; uint32_t len; } SMBlk;

static void vc_field(Bb *b, const char *key, const char *val) {
    if (!val || !*val) return;
    uint32_t total = (uint32_t)(strlen(key) + 1 + strlen(val));
    bb_le32(b, total);
    bb_put(b, key, strlen(key));
    bb_u8(b, '=');
    bb_put(b, val, strlen(val));
}
static void vc_field_int(Bb *b, const char *key, int n) {
    char buf[24]; snprintf(buf,sizeof buf,"%d",n); vc_field(b, key, buf);
}
static void vc_field_fp(Bb *b, const char *key, float v) {
    char buf[32]; snprintf(buf,sizeof buf,"%.2f dB",v); vc_field(b, key, buf);
}

static void build_vorbis_comment(Bb *b, const Track *t) {
    const char *vendor = "squidget";
    bb_le32(b, (uint32_t)strlen(vendor));
    bb_put(b, vendor, strlen(vendor));
    size_t count_pos = b->len;
    bb_le32(b, 0);
    uint32_t n = 0;
#define VC(key, val)   do { vc_field    (b, key, val); n++; } while(0)
#define VCI(key, val)  do { vc_field_int(b, key, val); n++; } while(0)
#define VCFP(key, val) do { vc_field_fp (b, key, val); n++; } while(0)
    if (t->title[0])       VC("TITLE",      t->title);
    if (t->artist[0])    { VC("ARTIST",     t->artist);
                           VC("ALBUMARTIST",t->artist); }
    if (t->album[0])       VC("ALBUM",      t->album);
    if (t->year[0])        VC("DATE",       t->year);
    if (t->lyrics && t->lyrics[0]) VC("LYRICS", t->lyrics);
    if (t->track_num > 0)  VCI("TRACKNUMBER", t->track_num);
    if (t->disc_num  > 0)  VCI("DISCNUMBER",  t->disc_num);
    if (t->isrc[0])        VC("ISRC",       t->isrc);
    if (t->copyright[0])   VC("COPYRIGHT",  t->copyright);
    if (t->explicit_)      VC("COMMENT",    "Explicit");
    if (t->replay_gain != 0.f) VCFP("REPLAYGAIN_TRACK_GAIN", t->replay_gain);
#undef VC
#undef VCI
#undef VCFP
    if (count_pos + 4 <= b->len) {
        b->d[count_pos+0]=(uint8_t)n; b->d[count_pos+1]=(uint8_t)(n>>8);
        b->d[count_pos+2]=(uint8_t)(n>>16); b->d[count_pos+3]=(uint8_t)(n>>24);
    }
}

static void build_picture_block(Bb *b, const uint8_t *img, size_t img_sz) {
    const char *mime = "image/jpeg";
    if (img_sz >= 4 && img[0]==0x89 && img[1]=='P') mime = "image/png";
    bb_be32(b, 3);
    bb_be32(b, (uint32_t)strlen(mime)); bb_put(b, mime, strlen(mime));
    bb_be32(b, 0);
    bb_be32(b, 0); bb_be32(b, 0); bb_be32(b, 0); bb_be32(b, 0);
    bb_be32(b, (uint32_t)img_sz); bb_put(b, img, img_sz);
}

static void flac_emit_block(Bb *out, int type, int last,
                              const uint8_t *data, uint32_t len) {
    uint8_t hdr[4];
    hdr[0] = (uint8_t)(((last?1:0)<<7) | (type&0x7F));
    hdr[1] = (uint8_t)(len>>16); hdr[2] = (uint8_t)(len>>8); hdr[3] = (uint8_t)len;
    bb_put(out, hdr, 4);
    if (len) bb_put(out, data, len);
}

/* Stream bytes from src (from audio_offset) to dst in 64 KB chunks. */
static int stream_audio(FILE *dst, FILE *src, long audio_offset) {
    if (fseek(src, audio_offset, SEEK_SET) != 0) return 0;
    uint8_t chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, src)) > 0)
        if (fwrite(chunk, 1, n, dst) != n) return 0;
    return !ferror(src) && !ferror(dst);
}

static int flac_tag(const char *path, const Track *t, const char *cover_path) {
    SQT_LOG("flac_tag (streaming) START  path='%s'", path);

    FILE *in = fopen(path, "rb");
    if (!in) { SQT_LOG("flac_tag: cannot open input"); return -1; }

    /* detect optional ID3v2 prefix */
    uint8_t hdr10[10] = {0};
    if (fread(hdr10, 1, 10, in) != 10) { fclose(in); return -1; }
    long flac_start = 0;
    if (hdr10[0]=='I' && hdr10[1]=='D' && hdr10[2]=='3') {
        size_t id3sz = id3v2_total_size(hdr10, 10);
        if (id3sz < 10) { fclose(in); return -1; }
        flac_start = (long)id3sz;
        if (fseek(in, flac_start, SEEK_SET) != 0 ||
            fread(hdr10, 1, 4, in) != 4 ||
            memcmp(hdr10, "fLaC", 4) != 0) {
            SQT_LOG("flac_tag: fLaC magic not found after ID3");
            fclose(in); return -1;
        }
    } else {
        if (memcmp(hdr10, "fLaC", 4) != 0) {
            SQT_LOG("flac_tag: not a FLAC file"); fclose(in); return -1;
        }
        fseek(in, 4, SEEK_SET);
    }
    /* file cursor is now just past the "fLaC" marker */

    /* scan metadata blocks, keeping non-metadata ones in small buffers */
    SMBlk keep[64]; int nkeep = 0;
    int last_block = 0;
    while (!last_block) {
        uint8_t mhdr[4];
        if (fread(mhdr, 1, 4, in) != 4) break;
        last_block    = (mhdr[0] >> 7) & 1;
        int    btype  = mhdr[0] & 0x7F;
        uint32_t blen = ((uint32_t)mhdr[1]<<16)|((uint32_t)mhdr[2]<<8)|mhdr[3];
        if (btype != FLAC_VORBIS_COMMENT && btype != FLAC_PICTURE && nkeep < 64) {
            keep[nkeep].type = btype;
            keep[nkeep].len  = blen;
            keep[nkeep].data = blen ? malloc(blen) : NULL;
            if (blen && (!keep[nkeep].data ||
                         fread(keep[nkeep].data, 1, blen, in) != blen)) {
                free(keep[nkeep].data);
                for (int i = 0; i < nkeep; i++) free(keep[i].data);
                fclose(in); return -1;
            }
            nkeep++;
        } else {
            if (blen && fseek(in, (long)blen, SEEK_CUR) != 0) {
                for (int i = 0; i < nkeep; i++) free(keep[i].data);
                fclose(in); return -1;
            }
        }
    }
    long audio_offset = ftell(in);  /* first audio frame starts here */

    /* load cover art (small JPEG only, never the audio data) */
    uint8_t *cover = NULL; size_t cover_sz = 0;
    if (cover_path && cover_path[0]) cover = file_read(cover_path, &cover_sz);

    /* build new metadata in RAM (tags + optional cover ≤ ~2 MB) */
    Bb vc = {0}; build_vorbis_comment(&vc, t);
    Bb pic = {0};
    if (cover && cover_sz > 0) build_picture_block(&pic, cover, cover_sz);
    Bb id3bb = {0};
    if (cover && cover_sz > 0) build_id3v2_apic(&id3bb, cover, cover_sz);
    free(cover);

    Bb meta = {0};
    if (id3bb.len > 0) bb_put(&meta, id3bb.d, id3bb.len);
    bb_free(&id3bb);
    bb_put(&meta, "fLaC", 4);
    for (int i = 0; i < nkeep; i++) {
        int is_last = (i == nkeep-1) && (vc.len == 0) && (pic.len == 0);
        flac_emit_block(&meta, keep[i].type, is_last, keep[i].data, keep[i].len);
        free(keep[i].data);
    }
    if (vc.len > 0)
        flac_emit_block(&meta, FLAC_VORBIS_COMMENT, (pic.len == 0),
                         vc.d, (uint32_t)vc.len);
    if (pic.len > 0)
        flac_emit_block(&meta, FLAC_PICTURE, 1, pic.d, (uint32_t)pic.len);
    bb_free(&vc); bb_free(&pic);

    /* write temp file: small metadata header + streamed audio (64 KB chunks) */
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.sqttmp", path);
    FILE *out = fopen(tmp, "wb");
    if (!out) { bb_free(&meta); fclose(in); return -1; }

    int ok = (fwrite(meta.d, 1, meta.len, out) == meta.len);
    if (ok) ok = stream_audio(out, in, audio_offset);
    fclose(out); fclose(in); bb_free(&meta);
    if (!ok) { remove(tmp); return -1; }

    /* atomic rename */
#ifdef _WIN32
    remove(path);
    ok = MoveFileA(tmp, path) != 0;
#else
    ok = rename(tmp, path) == 0;
#endif
    return ok ? 0 : -1;
}

/* ══════════════════════════ M4A / MP4 tagger ════════════════════════════ */

static size_t box_next(const uint8_t *buf, size_t total, size_t cursor,
                        char type_out[4], size_t *coff, size_t *csz) {
    if (cursor + 8 > total) return 0;
    uint32_t sz32 = rd32be(buf + cursor);
    if (type_out) memcpy(type_out, buf + cursor + 4, 4);
    if (sz32 == 0) {
        if (coff) *coff = cursor + 8;
        if (csz)  *csz  = total - cursor - 8;
        return total;
    }
    if (sz32 == 1) {
        if (cursor + 16 > total) return 0;
        uint64_t sz64 = rd64be(buf + cursor + 8);
        if (sz64 < 16 || cursor + (size_t)sz64 > total) return 0;
        if (coff) *coff = cursor + 16;
        if (csz)  *csz  = (size_t)(sz64 - 16);
        return cursor + (size_t)sz64;
    }
    if (sz32 < 8 || cursor + sz32 > total) return 0;
    if (coff) *coff = cursor + 8;
    if (csz)  *csz  = sz32 - 8;
    return cursor + sz32;
}

static size_t bb_box_begin(Bb *b, const char *type) {
    size_t pos = b->len;
    bb_be32(b, 0);
    bb_put(b, type, 4);
    return pos;
}

static void bb_box_end(Bb *b, size_t pos) {
    bb_patch_be32(b, pos, (uint32_t)(b->len - pos));
}

static void patch_offsets(uint8_t *buf, size_t off, size_t sz, int64_t delta) {
    static const char *containers[] = {
        "moov","trak","edts","mdia","minf","dinf","stbl","mvex",
        "moof","traf","mfra","skip","udta", NULL
    };
    size_t cur = off, end = off + sz;
    while (cur < end) {
        if (cur + 8 > end) break;
        char btype[4]; size_t co, cs;
        size_t next = box_next(buf, end, cur, btype, &co, &cs);
        if (!next) break;
        if (memcmp(btype, "stco", 4) == 0 && cs >= 8) {
            uint32_t n = rd32be(buf + co + 4);
            if (cs >= 8 + (size_t)n * 4) {
                for (uint32_t i = 0; i < n; i++) {
                    uint8_t *p = buf + co + 8 + i * 4;
                    wr32be(p, rd32be(p) + (uint32_t)(int32_t)delta);
                }
            }
        } else if (memcmp(btype, "co64", 4) == 0 && cs >= 8) {
            uint32_t n = rd32be(buf + co + 4);
            if (cs >= 8 + (size_t)n * 8) {
                for (uint32_t i = 0; i < n; i++) {
                    uint8_t *p = buf + co + 8 + i * 8;
                    wr64be(p, rd64be(p) + (uint64_t)(int64_t)delta);
                }
            }
        } else {
            for (int i = 0; containers[i]; i++) {
                if (memcmp(btype, containers[i], 4) == 0) {
                    patch_offsets(buf, co, cs, delta);
                    break;
                }
            }
            if (memcmp(btype, "meta", 4) == 0 && cs > 4)
                patch_offsets(buf, co + 4, cs - 4, delta);
        }
        cur = next;
    }
}

static void bb_text_atom(Bb *b, const char *atom_type, const char *text) {
    if (!text || !*text) return;
    size_t ap = bb_box_begin(b, atom_type);
    size_t dp = bb_box_begin(b, "data");
    bb_be32(b, 1); bb_be32(b, 0);
    bb_put(b, text, strlen(text));
    bb_box_end(b, dp);
    bb_box_end(b, ap);
}

static void bb_free_atom(Bb *b, const char *name, const char *value) {
    if (!value || !*value) return;
    size_t fp = bb_box_begin(b, "----");
    { size_t mp = bb_box_begin(b, "mean"); bb_be32(b, 0); bb_put(b, "com.apple.iTunes", 16); bb_box_end(b, mp); }
    { size_t np = bb_box_begin(b, "name"); bb_be32(b, 0); bb_put(b, name, strlen(name)); bb_box_end(b, np); }
    { size_t dp = bb_box_begin(b, "data"); bb_be32(b, 1); bb_be32(b, 0); bb_put(b, value, strlen(value)); bb_box_end(b, dp); }
    bb_box_end(b, fp);
}

static void bb_pair_atom(Bb *b, const char *atom_type, uint16_t num) {
    if (num == 0) return;
    size_t ap = bb_box_begin(b, atom_type);
    size_t dp = bb_box_begin(b, "data");
    bb_be32(b, 0); bb_be32(b, 0);
    bb_be16(b, 0); bb_be16(b, num); bb_be16(b, 0); bb_be16(b, 0);
    bb_box_end(b, dp);
    bb_box_end(b, ap);
}

static void build_ilst(Bb *b, const Track *t, const uint8_t *cover, size_t cover_sz) {
    uint8_t nam_atom[] = {0xa9, 'n', 'a', 'm'};
    uint8_t art_atom[] = {0xa9, 'A', 'R', 'T'};
    uint8_t alb_atom[] = {0xa9, 'a', 'l', 'b'};
    uint8_t day_atom[] = {0xa9, 'd', 'a', 'y'};
    
    bb_text_atom(b, (const char*)nam_atom, t->title);
    bb_text_atom(b, (const char*)art_atom, t->artist);
    bb_text_atom(b, "aART", t->artist);
    bb_text_atom(b, (const char*)alb_atom, t->album);
    bb_text_atom(b, (const char*)day_atom, t->year);
    bb_text_atom(b, "cprt", t->copyright);
    if (t->lyrics && t->lyrics[0]) bb_text_atom(b, "\xa9lyr", t->lyrics);

    if (t->track_num > 0) bb_pair_atom(b, "trkn", (uint16_t)t->track_num);

    if (t->disc_num  > 0) bb_pair_atom(b, "disk", (uint16_t)t->disc_num);
    bb_free_atom(b, "ISRC", t->isrc);
    
    if (t->replay_gain != 0.f) {
        char rg[32]; snprintf(rg, sizeof rg, "%.2f dB", t->replay_gain);
        bb_free_atom(b, "REPLAYGAIN_TRACK_GAIN", rg);
    }
    
    if (t->explicit_) {
        size_t ap = bb_box_begin(b, "rtng");
        size_t dp = bb_box_begin(b, "data");
        bb_be32(b, 21); bb_be32(b, 0); bb_u8(b, 4);
        bb_box_end(b, dp);
        bb_box_end(b, ap);
    }
    
    if (cover && cover_sz > 0) {
        uint32_t img_flags = (cover_sz >= 4 && cover[0]==0x89 && cover[1]=='P') ? 14 : 13;
        size_t ap = bb_box_begin(b, "covr");
        size_t dp = bb_box_begin(b, "data");
        bb_be32(b, img_flags); bb_be32(b, 0);
        bb_put(b, cover, cover_sz);
        bb_box_end(b, dp);
        bb_box_end(b, ap);
    }
}

static void build_meta_content(Bb *out, const Bb *ilst_data) {
    bb_be32(out, 0);
    {
        size_t hp = bb_box_begin(out, "hdlr");
        bb_be32(out, 0); bb_be32(out, 0);
        bb_put(out, "mdir", 4);
        bb_be32(out, 0); bb_be32(out, 0); bb_be32(out, 0);
        bb_u8(out, 0);
        bb_box_end(out, hp);
    }
    {
        size_t ip = bb_box_begin(out, "ilst");
        bb_put(out, ilst_data->d, ilst_data->len);
        bb_box_end(out, ip);
    }
}

static int m4a_tag(const char *path, const Track *t, const char *cover_path) {
    SQT_LOG("m4a_tag (streaming) START  path='%s'", path);

    FILE *in = fopen(path, "rb");
    if (!in) { SQT_LOG("m4a_tag: cannot open input"); return -1; }

    fseek(in, 0, SEEK_END);
    size_t fsz = (size_t)ftell(in);
    rewind(in);

    size_t moov_start = 0, moov_len = 0, moov_coff = 0;
    size_t mdat_start = 0, mdat_len = 0;
    uint8_t *ftyp_data = NULL; size_t ftyp_len = 0;
    uint8_t *moov_data = NULL;

    size_t cur = 0;
    while (cur < fsz) {
        uint8_t h[8];
        if (fseek(in, (long)cur, SEEK_SET) != 0 || fread(h, 1, 8, in) != 8) break;
        uint32_t sz = rd32be(h);
        char ty[4]; memcpy(ty, h + 4, 4);

        if (memcmp(ty, "ftyp", 4) == 0) {
            ftyp_len = sz;
            ftyp_data = malloc(sz);
            fseek(in, (long)cur, SEEK_SET);
            if (fread(ftyp_data, 1, sz, in) != sz) { free(ftyp_data); ftyp_data = NULL; }
        } else if (memcmp(ty, "moov", 4) == 0) {
            moov_start = cur;
            moov_len = sz;
            moov_data = malloc(sz);
            fseek(in, (long)cur, SEEK_SET);
            if (fread(moov_data, 1, sz, in) != sz) { free(moov_data); moov_data = NULL; }
            /* find coff (start of boxes inside moov) */
            size_t co, cs;
            box_next(moov_data, moov_len, 0, NULL, &co, &cs);
            moov_coff = co;
        } else if (memcmp(ty, "mdat", 4) == 0) {
            mdat_start = cur;
            mdat_len = sz;
        }

        if (sz == 0) break;
        cur += sz;
    }

    if (!moov_data) {
        if (ftyp_data) free(ftyp_data);
        fclose(in);
        SQT_LOG("m4a_tag: moov not found");
        return -1;
    }

    uint8_t *cover = NULL; size_t cover_sz = 0;
    if (cover_path && cover_path[0]) cover = file_read(cover_path, &cover_sz);

    Bb ilst = {0};
    build_ilst(&ilst, t, cover, cover_sz);
    free(cover);

    if (ilst.len == 0 || ilst.len > 100000000UL) {
        bb_free(&ilst); free(ftyp_data); free(moov_data); fclose(in);
        return -1;
    }

    Bb meta_content = {0};
    build_meta_content(&meta_content, &ilst);
    bb_free(&ilst);

    Bb moov_body = {0};
    {
        size_t cur_m = moov_coff, end_m = moov_len;
        while (cur_m < end_m) {
            char ty[4]; size_t co, cs;
            size_t next = box_next(moov_data, end_m, cur_m, ty, &co, &cs);
            if (!next) break;
            if (memcmp(ty, "udta", 4) != 0) {
                bb_put(&moov_body, moov_data + cur_m, next - cur_m);
            }
            cur_m = next;
        }
    }

    {
        size_t udta_pos = bb_box_begin(&moov_body, "udta");
        size_t meta_pos = bb_box_begin(&moov_body, "meta");
        bb_put(&moov_body, meta_content.d, meta_content.len);
        bb_box_end(&moov_body, meta_pos);
        bb_box_end(&moov_body, udta_pos);
    }
    bb_free(&meta_content);

    Bb new_moov = {0};
    {
        size_t mp = bb_box_begin(&new_moov, "moov");
        bb_put(&new_moov, moov_body.d, moov_body.len);
        bb_box_end(&new_moov, mp);
    }
    bb_free(&moov_body);

    int moov_before_mdat = (moov_start < mdat_start);
    if (moov_before_mdat) {
        int64_t delta = (int64_t)new_moov.len - (int64_t)moov_len;
        if (delta != 0)
            patch_offsets(new_moov.d, 8, new_moov.len - 8, delta);
    }

    /* write to temp file */
    char tmp_path[1024];
    snprintf(tmp_path, sizeof tmp_path, "%s.sqttmp", path);
    FILE *out = fopen(tmp_path, "wb");
    if (!out) {
        free(ftyp_data); free(moov_data); bb_free(&new_moov); fclose(in);
        return -1;
    }

    int ok = 1;
    if (ftyp_data) ok = (fwrite(ftyp_data, 1, ftyp_len, out) == ftyp_len);
    
    if (ok) {
        if (moov_before_mdat) {
            /* 1. new moov */
            if (fwrite(new_moov.d, 1, new_moov.len, out) != new_moov.len) ok = 0;
            /* 2. any boxes between old moov and mdat? 
               (simplified: we assume ftyp -> moov -> mdat or ftyp -> mdat -> moov) */
            if (ok) ok = stream_audio(out, in, (long)(mdat_start));
        } else {
            /* mdat then moov */
            if (ok) ok = stream_audio(out, in, (long)(mdat_start));
            if (ok) ok = (fwrite(new_moov.d, 1, new_moov.len, out) == new_moov.len);
        }
    }

    fclose(out); fclose(in);
    free(ftyp_data); free(moov_data); bb_free(&new_moov);

    if (!ok) { remove(tmp_path); return -1; }

#ifdef _WIN32
    remove(path);
    ok = MoveFileA(tmp_path, path) != 0;
#else
    ok = rename(tmp_path, path) == 0;
#endif
    return ok ? 0 : -1;
}

/* ══════════════════════════ public entry point ══════════════════════════ */

int sqt_tag(const char *path, const Track *t, const char *cover_jpg) {
    SQT_LOG("sqt_tag: path='%s'  title='%s'  cover='%s'",
            path, t->title, cover_jpg ? cover_jpg : "(none)");
    FILE *f = fopen(path, "rb");
    if (!f) { SQT_LOG("sqt_tag: cannot open file"); return -1; }
    uint8_t magic[10] = {0};
    size_t nr = fread(magic, 1, sizeof magic, f);

    if (nr >= 10 && magic[0]=='I' && magic[1]=='D' && magic[2]=='3') {
        size_t id3_sz = id3v2_total_size(magic, nr);
        if (id3_sz >= 10) {
            fseek(f, (long)id3_sz, SEEK_SET);
            nr = fread(magic, 1, 4, f);
        }
    }
    fclose(f);

    if (nr < 4) { SQT_LOG("sqt_tag: file too small nr=%zu", nr); return -1; }
    
    if (memcmp(magic, "fLaC", 4) == 0) {
        return flac_tag(path, t, cover_jpg);
    }
    return m4a_tag(path, t, cover_jpg);
}