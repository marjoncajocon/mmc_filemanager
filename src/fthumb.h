/* fthumb.h -- thumbnails for images and videos, decoded in the background.
**
** thumb_get never blocks: it returns the texture when ready, otherwise
** queues the file for a worker thread and returns NULL (the caller draws
** the type icon meanwhile). Finished decodes are uploaded by thumb_pump on
** the main thread, which also wakes a redraw.
**
** The texture is usually a view of a shared atlas page (gfx_view_new):
** draw it with gfx_tex / gfx_tex_rounded and size it with SDL_QueryTexture
** (both understand views); do not pass it to other SDL calls. It stays
** valid until the next thumb_pump.
**
** Design decisions:
**   - Memory is bounded: atlas pages live in an LRU capped by total bytes
**     (default 12 MB); decoded pixels are downscaled on the worker before
**     they ever reach the main thread.
**   - Small JPEG/PNG thumbs are also cached on disk (PLACE_CACHE) keyed by
**     path + size + mtime, so reopening a photo folder is instant.
*/
#ifndef FTHUMB_H
#define FTHUMB_H

#include "fgfx.h"

void thumb_init(void);
void thumb_shutdown(void);
/* px = edge length wanted in pixels (the thumb fits inside px x px). */
SDL_Texture *thumb_get(const char *path, i64 mtime, u64 size, int px);
void thumb_pump(void);
void thumb_reset(void);      /* renderer reset: drop all textures */
void thumb_cancel_all(void); /* folder changed: forget queued requests */
bool thumb_supported(FmType t);

#endif
