#pragma once
/* tag.h — squidget native FLAC + M4A/MP4 tagger (no external tools required)
 *
 * sqt_tag() inspects the file's magic bytes and dispatches to the
 * appropriate format-specific writer.  On any error the original file
 * is left untouched (writes go to a .sqttmp sidecar, then atomically
 * renamed over the target only on success).
 */

/* Forward declare to avoid circular include with squidget.h */
typedef struct Track Track;

/* Embed metadata + optional cover art directly into an audio file.
 *   path      – path to a FLAC or M4A/MP4 file
 *   t         – track metadata to embed
 *   cover_jpg – path to a JPEG or PNG cover image, or NULL
 * Returns 0 on success, -1 on failure.
 */
int sqt_tag(const char *path, const Track *t, const char *cover_jpg);