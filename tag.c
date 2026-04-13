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
#  define SQT_SEP "\\"
#else
#  include <unistd.h>
#  define SQT_SEP "/"
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
static int  bb_put(Bb *b, const void *src, size_t n) {
    if (!bb_need(b, n)) return 0;
    memcpy(b->d + b->len, src, n); b->len += n; return 1;
}
static int  bb_u8  (Bb *b, uint8_t  v) { return bb_put(b, &v, 1); }
static int  bb_be16(Bb *b, uint16_t v) {
    uint8_t p[2]; p[0]=(uint8_t)(v>>8); p[1]=(uint8_t)v;
    return bb_put(b,p,2);
}
static int  bb_be32(Bb *b, uint32_t v) {
    uint8_t p[4]; p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16);
    p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v;
    return bb_put(b,p,4);
}
static int  bb_le32(Bb *b, uint32_t v) {
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

/* Encode val as a 4-byte syncsafe integer (each byte uses only 7 bits). */
static void encode_syncsafe(uint8_t out[4], uint32_t val) {
    out[3] = (uint8_t)(val & 0x7F); val >>= 7;
    out[2] = (uint8_t)(val & 0x7F); val >>= 7;
    out[1] = (uint8_t)(val & 0x7F); val >>= 7;
    out[0] = (uint8_t)(val & 0x7F);
}

/* Returns total byte length of an ID3v2 block (10-byte header + payload),
 * or 0 if the data does not start with a valid ID3v2 header. */
static size_t id3v2_total_size(const uint8_t *data, size_t fsz) {
    if (fsz < 10 || data[0]!='I' || data[1]!='D' || data[2]!='3') return 0;
    uint32_t sz = ((uint32_t)(data[6] & 0x7F) << 21)
                | ((uint32_t)(data[7] & 0x7F) << 14)
                | ((uint32_t)(data[8] & 0x7F) <<  7)
                |  (uint32_t)(data[9] & 0x7F);
    return 10 + (size_t)sz;
}

/* Build a minimal ID3v2.3 tag containing a single APIC (cover art) frame.
 * Writes into *b (caller-allocated, zeroed).
 *
 * This is prepended before the "fLaC" marker so that macOS Core Audio /
 * AVFoundation — which does not reliably parse METADATA_BLOCK_PICTURE —
 * can still display album artwork, while the native PICTURE block remains
 * for spec-compliant players. */
static void build_id3v2_apic(Bb *b, const uint8_t *img, size_t img_sz) {
    const char *mime = (img_sz >= 4 &&
                        img[0]==0x89 && img[1]=='P' &&
                        img[2]=='N'  && img[3]=='G')
                       ? "image/png" : "image/jpeg";
    size_t mime_len = strlen(mime);

    /* APIC frame content:
     *   1   text encoding  (0x00 = Latin-1)
     *   N   MIME type string
     *   1   MIME null terminator
     *   1   picture type   (0x03 = front cover)
     *   1   description null terminator (empty string)
     *   N   image data
     */
    uint32_t apic_content = (uint32_t)(1 + mime_len + 1 + 1 + 1 + img_sz);

    /* ID3v2.3 tag payload = one 10-byte frame header + APIC content */
    uint32_t tag_payload = 10 + apic_content;

    /* ── ID3v2.3 tag header (10 bytes) ── */
    bb_put(b, "ID3", 3);
    bb_u8 (b, 0x03);           /* version 2.3        */
    bb_u8 (b, 0x00);           /* revision 0         */
    bb_u8 (b, 0x00);           /* flags (none)       */
    uint8_t ss[4];
    encode_syncsafe(ss, tag_payload);
    bb_put(b, ss, 4);          /* syncsafe tag size  */

    /* ── APIC frame header (10 bytes; ID3v2.3 frame size is plain big-endian) ── */
    bb_put (b, "APIC", 4);
    bb_be32(b, apic_content);  /* frame size (NOT syncsafe in v2.3) */
    bb_be16(b, 0x0000);        /* frame flags                        */

    /* ── APIC frame payload ── */
    bb_u8(b, 0x00);            /* text encoding: Latin-1             */
    bb_put(b, mime, mime_len);
    bb_u8(b, 0x00);            /* null-terminate MIME type           */
    bb_u8(b, 0x03);            /* picture type: front cover          */
    bb_u8(b, 0x00);            /* description: empty (null term)     */
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
    if (fread(buf, 1, (size_t)l, f) != (size_t)l) { free(buf); buf = NULL; fclose(f); return NULL; }
    buf[l] = 0;
    *out_sz = (size_t)l;
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
    if (!ok) SQT_LOG("file_write_atomic: fwrite failed");
    fclose(f);
    if (!ok) { remove(tmp); return 0; }
#ifdef _WIN32
    remove(path);
    int ret = MoveFileA(tmp, path) != 0;
    if (!ret) SQT_LOG("file_write_atomic: MoveFileA failed le=%lu", GetLastError());
    else      SQT_LOG("file_write_atomic: success");
    return ret;
#else
    int ret = rename(tmp, path) == 0;
    if (!ret) SQT_LOG("file_write_atomic: rename failed");
    else      SQT_LOG("file_write_atomic: success");
    return ret;
#endif
}

/* ══════════════════════════ FLAC tagger ═════════════════════════════════ */

#define FLAC_STREAMINFO     0
#define FLAC_PADDING        1
#define FLAC_VORBIS_COMMENT 4
#define FLAC_PICTURE        6

static void vc_field(Bb *b, const char *key, const char *val) {
    if (!val || !*val) return;
    uint32_t total = (uint32_t)(strlen(key) + 1 + strlen(val));
    bb_le32(b, total);
    bb_put(b, key, strlen(key));
    bb_u8(b, '=');
    bb_put(b, val, strlen(val));
}
static void vc_field_int(Bb *b, const char *key, int n) {
    char buf[24]; snprintf(buf,sizeof buf,"%d",n);
    vc_field(b, key, buf);
}
static void vc_field_fp(Bb *b, const char *key, float v) {
    char buf[32]; snprintf(buf,sizeof buf,"%.2f dB",v);
    vc_field(b, key, buf);
}

/* NOTE: takes Bb *b (caller-allocated, zeroed).  Never returns Bb by value. */
static void build_vorbis_comment(Bb *b, const Track *t) {
    int n = 0;
    if (t->title[0])         n++;
    if (t->artist[0])        n += 2;
    if (t->album[0])         n++;
    if (t->year[0])          n++;
    if (t->track_num > 0)    n++;
    if (t->disc_num  > 0)    n++;
    if (t->isrc[0])          n++;
    if (t->copyright[0])     n++;
    if (t->explicit_)        n++;
    if (t->replay_gain!=0.f) n++;

    const char *vendor = "squidget";
    bb_le32(b, (uint32_t)strlen(vendor));
    bb_put (b, vendor, strlen(vendor));
    bb_le32(b, (uint32_t)n);

    vc_field    (b, "TITLE",               t->title);
    vc_field    (b, "ARTIST",              t->artist);
    vc_field    (b, "ALBUMARTIST",         t->artist);
    vc_field    (b, "ALBUM",               t->album);
    vc_field    (b, "DATE",                t->year);
    if (t->track_num > 0) vc_field_int(b, "TRACKNUMBER", t->track_num);
    if (t->disc_num  > 0) vc_field_int(b, "DISCNUMBER",  t->disc_num);
    vc_field    (b, "ISRC",                t->isrc);
    vc_field    (b, "COPYRIGHT",           t->copyright);
    if (t->explicit_)        vc_field(b, "COMMENT", "Explicit");
    if (t->replay_gain!=0.f) vc_field_fp(b, "REPLAYGAIN_TRACK_GAIN", t->replay_gain);
}

/* NOTE: takes Bb *b (caller-allocated, zeroed).  Never returns Bb by value. */
static void build_picture_block(Bb *b, const uint8_t *img, size_t img_sz) {
    const char *mime = "image/jpeg";
    if (img_sz >= 4 && img[0]==0x89 && img[1]=='P') mime = "image/png";
    bb_be32(b, 3);
    bb_be32(b, (uint32_t)strlen(mime));
    bb_put (b, mime, strlen(mime));
    bb_be32(b, 0);
    bb_be32(b, 0); bb_be32(b, 0);
    bb_be32(b, 0); bb_be32(b, 0);
    bb_be32(b, (uint32_t)img_sz);
    bb_put (b, img, img_sz);
}

static void flac_emit_block(Bb *out, int type, int last,
                             const uint8_t *data, uint32_t len) {
    uint8_t hdr[4];
    hdr[0] = (uint8_t)(((last?1:0)<<7) | (type&0x7F));
    hdr[1] = (uint8_t)(len>>16);
    hdr[2] = (uint8_t)(len>>8);
    hdr[3] = (uint8_t)(len);
    bb_put(out, hdr, 4);
    if (len) bb_put(out, data, len);
}

static int flac_tag(const char *path, const Track *t, const char *cover_path) {
    SQT_LOG("flac_tag START  path='%s'", path);

    size_t fsz;
    uint8_t *file = file_read(path, &fsz);
    if (!file) { SQT_LOG("flac_tag: file_read returned NULL"); return -1; }
    SQT_LOG("flac_tag: file_read ok  fsz=%zu", fsz);

    /* Skip any existing ID3v2 prefix written by a previous tag pass. */
    size_t flac_start = id3v2_total_size(file, fsz);

    if (flac_start + 4 > fsz || memcmp(file + flac_start, "fLaC", 4) != 0) {
        SQT_LOG("flac_tag: invalid FLAC magic at offset %zu: %02x %02x %02x %02x",
                flac_start,
                flac_start     < fsz ? file[flac_start]   : 0,
                flac_start + 1 < fsz ? file[flac_start+1] : 0,
                flac_start + 2 < fsz ? file[flac_start+2] : 0,
                flac_start + 3 < fsz ? file[flac_start+3] : 0);
        free(file); return -1;
    }

    uint8_t *cover = NULL; size_t cover_sz = 0;
    if (cover_path && cover_path[0]) {
        cover = file_read(cover_path, &cover_sz);
        SQT_LOG("flac_tag: cover loaded  sz=%zu  ptr=%p", cover_sz, (void*)cover);
    }

    typedef struct { int type; uint32_t off, len; } MBlk;
    MBlk keep[64]; int nkeep = 0;
    size_t pos = flac_start + 4; int last_block = 0;
    while (pos + 4 <= fsz && !last_block) {
        uint8_t h0    = file[pos];
        last_block    = (h0 >> 7) & 1;
        int    btype  = h0 & 0x7F;
        uint32_t blen = ((uint32_t)file[pos+1]<<16)
                       |((uint32_t)file[pos+2]<<8)
                       |(uint32_t)file[pos+3];
        SQT_LOG("flac_tag: block type=%d  len=%u  last=%d", btype, blen, last_block);
        if (btype != FLAC_VORBIS_COMMENT &&
            btype != FLAC_PICTURE        &&
            btype != FLAC_PADDING        &&
            nkeep < 64) {
            keep[nkeep].type = btype;
            keep[nkeep].off  = (uint32_t)(pos + 4);
            keep[nkeep].len  = blen;
            nkeep++;
        }
        pos += 4 + blen;
        if (pos > fsz) break;
    }
    size_t audio_off = pos;
    SQT_LOG("flac_tag: nkeep=%d  audio_off=%zu  audio_bytes=%zu",
            nkeep, audio_off, fsz - audio_off);

    /* build comment block via pointer — NO struct return */
    Bb vc = {0};
    build_vorbis_comment(&vc, t);
    SQT_LOG("flac_tag: vorbis comment built  len=%zu  ptr=%p", vc.len, (void*)vc.d);

    Bb pic = {0};
    if (cover && cover_sz > 0) {
        build_picture_block(&pic, cover, cover_sz);
        SQT_LOG("flac_tag: picture block built  len=%zu  ptr=%p", pic.len, (void*)pic.d);
    }

    /* Build the ID3v2.3 APIC block while cover is still in memory.
     * It will be prepended before "fLaC" so macOS AVFoundation can find
     * the artwork; the PICTURE metadata block below serves spec-compliant
     * players. */
    Bb id3 = {0};
    if (cover && cover_sz > 0)
        build_id3v2_apic(&id3, cover, cover_sz);

    free(cover);

    Bb out = {0};
    if (id3.len > 0) bb_put(&out, id3.d, id3.len);  /* macOS compat prefix */
    bb_free(&id3);
    bb_put(&out, "fLaC", 4);
    for (int i = 0; i < nkeep; i++) {
        int is_last = (i == nkeep-1) && (vc.len == 0) && (pic.len == 0);
        flac_emit_block(&out, keep[i].type, is_last,
                         file + keep[i].off, keep[i].len);
    }
    if (vc.len > 0)
        flac_emit_block(&out, FLAC_VORBIS_COMMENT, (pic.len == 0),
                         vc.d, (uint32_t)vc.len);
    if (pic.len > 0)
        flac_emit_block(&out, FLAC_PICTURE, 1, pic.d, (uint32_t)pic.len);
    if (audio_off < fsz)
        bb_put(&out, file + audio_off, fsz - audio_off);

    SQT_LOG("flac_tag: total output=%zu bytes", out.len);
    free(file);
    bb_free(&vc); bb_free(&pic);

    int ok = file_write_atomic(path, out.d, out.len);
    bb_free(&out);
    SQT_LOG("flac_tag: done  ok=%d", ok);
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
    if (!text || !*text) {
        SQT_LOG("bb_text_atom: SKIPPED atom type=(%d,%d,%d,%d) - text is %s", 
                (unsigned char)atom_type[0], (unsigned char)atom_type[1], 
                (unsigned char)atom_type[2], (unsigned char)atom_type[3],
                (!text) ? "NULL" : "empty");
        return;
    }
    size_t before = b->len;
    size_t ap = bb_box_begin(b, atom_type);
    size_t dp = bb_box_begin(b, "data");
    bb_be32(b, 1); bb_be32(b, 0);
    bb_put(b, text, strlen(text));
    bb_box_end(b, dp); bb_box_end(b, ap);
    size_t after = b->len;
    SQT_LOG("bb_text_atom: wrote atom type=(%d,%d,%d,%d) text='%s' (%zu bytes, %zu -> %zu)", 
            (unsigned char)atom_type[0], (unsigned char)atom_type[1], 
            (unsigned char)atom_type[2], (unsigned char)atom_type[3],
            text, strlen(text), before, after);
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
    bb_box_end(b, dp); bb_box_end(b, ap);
}

/* NOTE: writes into *b (caller-allocated, zeroed).  Never returns Bb. */
static void build_ilst(Bb *b, const Track *t, const uint8_t *cover, size_t cover_sz) {
    SQT_LOG("build_ilst: ENTRY  track ptr=%p  t->title='%s'  t->artist='%s'  t->album='%s'",
            (void*)t, t->title, t->artist, t->album);

    /* Use byte arrays instead of hex escapes to avoid TinyCC compilation issues */
    uint8_t nam_atom[] = {0xa9, 'n', 'a', 'm'};
    uint8_t art_atom[] = {0xa9, 'A', 'R', 'T'};
    uint8_t alb_atom[] = {0xa9, 'a', 'l', 'b'};
    uint8_t day_atom[] = {0xa9, 'd', 'a', 'y'};
    
    bb_text_atom(b, (const char*)nam_atom, t->title);
    SQT_LOG("build_ilst: after title atom");
    
    bb_text_atom(b, (const char*)art_atom, t->artist);
    SQT_LOG("build_ilst: after artist atom");
    
    bb_text_atom(b, "aART",    t->artist);
    SQT_LOG("build_ilst: after albumartist atom");
    
    SQT_LOG("build_ilst: ALBUM CHECK - ptr=%p  value='%s'  len=%zu  empty=%d  NULL=%d",
            (void*)t->album, t->album, strlen(t->album), 
            (t->album[0]==0), (t->album==NULL));
    bb_text_atom(b, (const char*)alb_atom, t->album);
    SQT_LOG("build_ilst: after album atom - buffer now len=%zu", b->len);
    
    bb_text_atom(b, (const char*)day_atom, t->year);
    SQT_LOG("build_ilst: after year atom");
    
    bb_text_atom(b, "cprt",    t->copyright);
    SQT_LOG("build_ilst: after copyright atom");
    
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
        bb_box_end(b, dp); bb_box_end(b, ap);
    }
    if (cover && cover_sz > 0) {
        uint32_t img_flags = 13;
        if (cover_sz >= 4 && cover[0]==0x89 && cover[1]=='P') img_flags = 14;
        size_t ap = bb_box_begin(b, "covr");
        size_t dp = bb_box_begin(b, "data");
        bb_be32(b, img_flags); bb_be32(b, 0);
        bb_put(b, cover, cover_sz);
        bb_box_end(b, dp); bb_box_end(b, ap);
    }
    SQT_LOG("build_ilst: DONE  b->len=%zu  b->ptr=%p", b->len, (void*)b->d);
}

/* NOTE: writes into *out (caller-allocated, zeroed).  Never returns Bb. */
static void build_meta_content(Bb *out, const Bb *ilst_data) {
    bb_be32(out, 0); /* FullBox version+flags */
    {
        size_t hp = bb_box_begin(out, "hdlr");
        bb_be32(out, 0); bb_be32(out, 0);
        bb_put (out, "mdir", 4);
        bb_be32(out, 0); bb_be32(out, 0); bb_be32(out, 0);
        bb_u8  (out, 0);
        bb_box_end(out, hp);
    }
    {
        size_t ip = bb_box_begin(out, "ilst");
        bb_put(out, ilst_data->d, ilst_data->len);
        bb_box_end(out, ip);
    }
    SQT_LOG("build_meta_content: done  len=%zu", out->len);
}

static int m4a_tag(const char *path, const Track *t, const char *cover_path) {
    SQT_LOG("m4a_tag START  path='%s'", path);
    SQT_LOG("  title='%s' artist='%s' album='%s' year='%s'",
            t->title, t->artist, t->album, t->year);

    size_t fsz;
    uint8_t *file = file_read(path, &fsz);
    SQT_LOG("m4a_tag: file_read => ptr=%p  fsz=%zu", (void*)file, fsz);
    if (!file) { SQT_LOG("m4a_tag: file_read NULL"); return -1; }

    if (fsz >= 8)
        SQT_LOG("m4a_tag: first 8 bytes: %02x %02x %02x %02x | %c%c%c%c",
                file[0],file[1],file[2],file[3],
                file[4],file[5],file[6],file[7]);

    /* locate moov */
    size_t moov_start=0, moov_coff=0, moov_csz=0, moov_end_abs=0;
    int found_moov = 0;
    {
        size_t cur = 0;
        while (cur < fsz) {
            char ty[4]; size_t co, cs;
            size_t next = box_next(file, fsz, cur, ty, &co, &cs);
            if (!next) { SQT_LOG("m4a_tag: box_next=0 at %zu", cur); break; }
            SQT_LOG("m4a_tag: box '%c%c%c%c'  sz=%zu  at=%zu",
                    ty[0],ty[1],ty[2],ty[3], next-cur, cur);
            if (memcmp(ty, "moov", 4) == 0) {
                moov_start=cur; moov_coff=co; moov_csz=cs; moov_end_abs=next;
                found_moov=1; break;
            }
            cur = next;
        }
    }
    SQT_LOG("m4a_tag: moov found=%d  start=%zu  end=%zu  csz=%zu",
            found_moov, moov_start, moov_end_abs, moov_csz);
    if (!found_moov) { SQT_LOG("m4a_tag: no moov"); free(file); return -1; }

    /* moov before mdat? */
    int moov_before_mdat = 0;
    {
        size_t cur = 0;
        while (cur < fsz) {
            char ty[4]; size_t co, cs;
            size_t next = box_next(file, fsz, cur, ty, &co, &cs);
            if (!next) break;
            if (memcmp(ty, "mdat", 4) == 0) {
                moov_before_mdat = (moov_start < cur);
                SQT_LOG("m4a_tag: mdat at %zu  moov_before_mdat=%d", cur, moov_before_mdat);
                break;
            }
            cur = next;
        }
    }

    /* cover */
    uint8_t *cover = NULL; size_t cover_sz = 0;
    if (cover_path && cover_path[0]) {
        cover = file_read(cover_path, &cover_sz);
        SQT_LOG("m4a_tag: cover => ptr=%p  sz=%zu", (void*)cover, cover_sz);
    }

    /* build ilst — pointer API, no struct return */
    Bb ilst = {0};
    SQT_LOG("m4a_tag: calling build_ilst (ptr api)  &ilst=%p", (void*)&ilst);
    build_ilst(&ilst, t, cover, cover_sz);
    SQT_LOG("m4a_tag: after build_ilst  ilst.len=%zu  ilst.ptr=%p", ilst.len, (void*)ilst.d);

    if (!ilst.d || ilst.len == 0) {
        SQT_LOG("m4a_tag: ERROR ilst empty after build");
        free(cover); free(file); return -1;
    }
    if (ilst.len > 100000000UL) {
        SQT_LOG("m4a_tag: ERROR ilst.len insane=%zu", ilst.len);
        bb_free(&ilst); free(cover); free(file); return -1;
    }
    free(cover);

    /* build meta content — pointer API */
    Bb meta_content = {0};
    build_meta_content(&meta_content, &ilst);
    SQT_LOG("m4a_tag: meta_content.len=%zu", meta_content.len);
    bb_free(&ilst);

    /* rebuild moov body */
    Bb moov_body = {0};
    {
        size_t cur = moov_coff, end = moov_end_abs;
        int kept = 0, dropped = 0;
        while (cur < end) {
            char ty[4]; size_t co, cs;
            size_t next = box_next(file, end, cur, ty, &co, &cs);
            if (!next) break;
            if (memcmp(ty, "udta", 4) != 0) {
                bb_put(&moov_body, file + cur, next - cur);
                kept++;
            } else {
                SQT_LOG("m4a_tag: dropped old udta  sz=%zu", next-cur);
                dropped++;
            }
            cur = next;
        }
        SQT_LOG("m4a_tag: moov children kept=%d dropped=%d", kept, dropped);
    }

    /* append new udta → meta */
    {
        size_t udta_pos = bb_box_begin(&moov_body, "udta");
        size_t meta_pos = bb_box_begin(&moov_body, "meta");
        bb_put(&moov_body, meta_content.d, meta_content.len);
        bb_box_end(&moov_body, meta_pos);
        bb_box_end(&moov_body, udta_pos);
        SQT_LOG("m4a_tag: new udta+meta appended  moov_body.len=%zu", moov_body.len);
    }
    bb_free(&meta_content);

    /* wrap in moov box */
    Bb new_moov = {0};
    {
        size_t mp = bb_box_begin(&new_moov, "moov");
        bb_put(&new_moov, moov_body.d, moov_body.len);
        bb_box_end(&new_moov, mp);
    }
    bb_free(&moov_body);
    SQT_LOG("m4a_tag: new_moov.len=%zu  old=%zu", new_moov.len, moov_end_abs-moov_start);

    /* patch stco/co64 if needed */
    if (moov_before_mdat) {
        int64_t delta = (int64_t)new_moov.len - (int64_t)(moov_end_abs - moov_start);
        SQT_LOG("m4a_tag: stco delta=%lld", (long long)delta);
        if (delta != 0)
            patch_offsets(new_moov.d, 8, new_moov.len - 8, delta);
    }

    /* assemble final file */
    Bb out = {0};
    bb_put(&out, file, moov_start);
    bb_put(&out, new_moov.d, new_moov.len);
    bb_put(&out, file + moov_end_abs, fsz - moov_end_abs);
    SQT_LOG("m4a_tag: final output=%zu bytes", out.len);

    free(file); bb_free(&new_moov);
    int ok = file_write_atomic(path, out.d, out.len);
    bb_free(&out);
    SQT_LOG("m4a_tag: done  ok=%d", ok);
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

    /* If the file opens with an ID3v2 header (from a prior tag pass that
     * prepended one for macOS compatibility), seek past it to read the
     * actual format marker. */
    if (nr >= 10 && magic[0]=='I' && magic[1]=='D' && magic[2]=='3') {
        size_t id3_sz = id3v2_total_size(magic, nr);
        if (id3_sz >= 10) {
            fseek(f, (long)id3_sz, SEEK_SET);
            nr = fread(magic, 1, 4, f);
        }
    }
    fclose(f);

    if (nr < 4) { SQT_LOG("sqt_tag: file too small nr=%zu", nr); return -1; }
    SQT_LOG("sqt_tag: format magic %02x %02x %02x %02x", magic[0], magic[1], magic[2], magic[3]);
    if (memcmp(magic, "fLaC", 4) == 0) {
        SQT_LOG("sqt_tag: dispatching to flac_tag");
        return flac_tag(path, t, cover_jpg);
    }
    SQT_LOG("sqt_tag: dispatching to m4a_tag");
    return m4a_tag(path, t, cover_jpg);
}
