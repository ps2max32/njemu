/******************************************************************************

	maxsdk_ui_draw.c

	ui_draw_driver_t on maxsdk: the UI's textures in GS memory, from staging
	buffers in EE memory, sent when they change. Drawing is video_driver's.

******************************************************************************/

#include <stdlib.h>
#include <string.h>
#include <mx/gs.h>
#include <mx/mem.h>
#include "common/ui_draw_driver.h"
#include "common/ui_layout.h"
#include "common/ui_texture_layout.h"
#include "common/video_driver.h"
#include "common/video_geometry.h"
#include "maxsdk/maxsdk_video.h"

/******************************************************************************
	Texture management
******************************************************************************/

typedef struct maxsdk_ui_texture {
	maxsdk_tex_t texture;       /* in GS memory (CT32 / ABGR8888) */
	uint16_t *buffer;           /* CPU-side staging buffer (16-bit ABGR4444 or ABGR1555) */
	uint32_t *upload_buffer;    /* 32-bit ABGR8888 buffer used to feed the GS */
	int width, height;
	int pitch;                  /* Row stride in pixels */
	int format;                 /* UI_PIXFMT_4444 or UI_PIXFMT_5551 */
	int buffer_valid;           /* 1 if buffer has valid data */
	int vram_valid;             /* 1 if VRAM texture is up-to-date */
} maxsdk_ui_texture_t;

#define UI_FONT_RING_SIZE   32
#define UI_FONT_RING_WIDTH  40
#define UI_FONT_RING_HEIGHT 32

typedef struct maxsdk_ui_font_ring_entry {
	maxsdk_tex_t texture;
	uint32_t *upload_buffer;
} maxsdk_ui_font_ring_entry_t;

typedef struct maxsdk_ui_data {
	void *video_data;
	maxsdk_ui_texture_t textures[UI_TEXTURE_MAX];
	maxsdk_ui_font_ring_entry_t font_ring[UI_FONT_RING_SIZE];
	int font_ring_next;
	int prepared_font_ring;
	int prepared_font_scratch;
} maxsdk_ui_data_t;

static maxsdk_ui_data_t ps2_ui;

static int ensure_vram(maxsdk_ui_texture_t *tex);
static void maxsdk_ui_release_buffers(maxsdk_ui_data_t *d);

typedef struct maxsdk_ui_buffer_shape {
	uint16_t pitch;
	uint16_t height;
} maxsdk_ui_buffer_shape_t;

static const maxsdk_ui_buffer_shape_t maxsdk_ui_buffer_shapes[UI_TEXTURE_MAX] = {
	[UI_TEXTURE_FONT]      = { BUF_WIDTH, 48 },
	[UI_TEXTURE_SMALLFONT] = { BUF_WIDTH, 16 },
	[UI_TEXTURE_BOXSHADOW] = { 72, 8 },
};


/******************************************************************************
	Helpers
******************************************************************************/

/******************************************************************************
	Driver interface implementation
******************************************************************************/

/*------------------------------------------------------
	Init / Term
------------------------------------------------------*/

static void *maxsdk_ui_draw_init(void *video_data)
{
	int i;

	memset(&ps2_ui, 0, sizeof(maxsdk_ui_data_t));
	ps2_ui.video_data = video_data;

	/* CPU staging buffers only need the maximum shape used by each common UI
	 * texture. The old four 512x512 allocations consumed ~2 MiB of EE RAM even
	 * though the live atlases total under 100 KiB. VRAM remains lazy. */
	for (i = 0; i < UI_TEXTURE_MAX; i++)
	{
		maxsdk_ui_texture_t *tex = &ps2_ui.textures[i];
		const maxsdk_ui_buffer_shape_t shape = maxsdk_ui_buffer_shapes[i];
		size_t buffer_size =
			(size_t)shape.pitch * (size_t)shape.height * sizeof(uint16_t);

		tex->width = 0;
		tex->height = 0;
		tex->pitch = shape.pitch;
		tex->format = UI_PIXFMT_4444;
		tex->buffer_valid = 0;
		tex->vram_valid = 0;

		tex->buffer = (uint16_t *)aligned_alloc(64, (buffer_size + 63) & ~(size_t)63);
		if (!tex->buffer) {
			maxsdk_ui_release_buffers(&ps2_ui);
			return NULL;
		}
		memset(tex->buffer, 0, buffer_size);

		/* upload_buffer is the 32-bit ABGR8888 version we hand to the GS.
		 * The PS2 has no native 4444 format, so we expand from tex->buffer
		 * at upload time. Lazily allocated by ensure_vram(). */
		tex->upload_buffer = NULL;
		memset(&tex->texture, 0, sizeof(tex->texture));
	}

	/* The big-font slot is updated through getTextureBasePtr (no size hint
	 * comes through clearTexture/uploadTexture), so seed it with the same
	 * 512x48 the Desktop reference uses for the font scratch area. */
	ps2_ui.textures[UI_TEXTURE_FONT].width  = 512;
	ps2_ui.textures[UI_TEXTURE_FONT].height = 48;
	if (!ensure_vram(&ps2_ui.textures[UI_TEXTURE_FONT])) {
		maxsdk_ui_release_buffers(&ps2_ui);
		return NULL;
	}

	/* Normal glyphs/shadows are tiny, but the common renderer rewrites one
	 * 512-pixel-pitch scratch buffer for every character. Keep a ring of compact
	 * GS destinations so up to 32 glyph upload+draw pairs can stay in one gsKit
	 * queue. This avoids both a full GS FINISH per character and the historical
	 * 512x48 upload for a ~10x14 glyph. */
	for (i = 0; i < UI_FONT_RING_SIZE; i++)
	{
		maxsdk_ui_font_ring_entry_t *entry = &ps2_ui.font_ring[i];
		size_t upload_size =
			(size_t)UI_FONT_RING_WIDTH * UI_FONT_RING_HEIGHT * sizeof(uint32_t);

		if (!maxsdk_tex_alloc(&entry->texture, UI_FONT_RING_WIDTH,
			UI_FONT_RING_HEIGHT, MX_GS_PSM_CT32)) {
			maxsdk_ui_release_buffers(&ps2_ui);
			return NULL;
		}

		entry->upload_buffer = (uint32_t *)mx_dma_alloc(upload_size);
		if (!entry->upload_buffer) {
			maxsdk_ui_release_buffers(&ps2_ui);
			return NULL;
		}
		memset(entry->upload_buffer, 0, upload_size);
	}
	ps2_ui.font_ring_next = 0;

	return &ps2_ui;
}

/* Round w/h up to a multiple of 8. */
static uint32_t round_tex_dim(int v) { return (uint32_t)(v + 7) & ~7u; }

static int ensure_vram(maxsdk_ui_texture_t *tex)
{
	if (tex->width == 0 || tex->height == 0) return 0;

	uint32_t want_w = round_tex_dim(tex->width);
	uint32_t want_h = round_tex_dim(tex->height);

	if (tex->texture.pages == 0 ||
	    tex->texture.width  < want_w ||
	    tex->texture.height < want_h)
	{
		/* draws queued from the old pages and buffer must be done */
		video_driver->flushAndWait(ps2_ui.video_data);
		maxsdk_tex_free(&tex->texture);
		if (!maxsdk_tex_alloc(&tex->texture, (int)want_w, (int)want_h, MX_GS_PSM_CT32))
		{
			tex->vram_valid = 0;
			return 0;
		}

		/* (Re)allocate the 32-bit upload staging buffer. */
		if (tex->upload_buffer)
			mx_dma_free(tex->upload_buffer);
		tex->upload_buffer = (uint32_t *)mx_dma_alloc(want_w * want_h * 4);
		if (tex->upload_buffer)
			memset(tex->upload_buffer, 0,
				(size_t)want_w * (size_t)want_h * 4);
		tex->vram_valid = 0;
	}
	return tex->texture.pages != 0 && tex->upload_buffer != NULL;
}

static void maxsdk_ui_release_buffers(maxsdk_ui_data_t *d)
{
	int i;

	if (!d)
		return;

	for (i = 0; i < UI_TEXTURE_MAX; i++)
	{
		free(d->textures[i].buffer);
		d->textures[i].buffer = NULL;
		if (d->textures[i].upload_buffer)
			mx_dma_free(d->textures[i].upload_buffer);
		d->textures[i].upload_buffer = NULL;
		maxsdk_tex_free(&d->textures[i].texture);
		d->textures[i].buffer_valid = 0;
		d->textures[i].vram_valid = 0;
	}

	for (i = 0; i < UI_FONT_RING_SIZE; i++)
	{
		if (d->font_ring[i].upload_buffer)
			mx_dma_free(d->font_ring[i].upload_buffer);
		d->font_ring[i].upload_buffer = NULL;
		maxsdk_tex_free(&d->font_ring[i].texture);
	}
}

/* Convert one row of 16-bit ABGR4444 / ABGR1555 to 32-bit ABGR8888. */
static void convert_row_to_8888(uint32_t *dst, const uint16_t *src, int w, int format)
{
	int x;
	if (format == UI_PIXFMT_4444)
	{
		for (x = 0; x < w; x++)
		{
			uint16_t p = src[x];
			/* The GS uses 0x80 as 1.0 alpha for blending.  Expanding the
			 * 4-bit UI alpha to 0xFF makes source alpha almost 2.0, which
			 * produces dark/inverted-looking glyphs over the menu. */
			uint8_t a = (uint8_t)((((p >> 12) & 0xF) * 0x80) / 0xF);
			uint8_t b = ((p >> 8)  & 0xF) * 17;
			uint8_t g = ((p >> 4)  & 0xF) * 17;
			uint8_t r =  (p        & 0xF) * 17;
			/* GS CT32 layout: bytes are R, G, B, A (low->high). */
			dst[x] = ((uint32_t)a << 24) | ((uint32_t)b << 16)
			       | ((uint32_t)g << 8)  |  (uint32_t)r;
		}
	}
	else  /* UI_PIXFMT_5551 */
	{
		for (x = 0; x < w; x++)
		{
			uint16_t p = src[x];
			/* The UI's 5551 atlas uses bit 15 as a transparency marker:
			 * empty pixels are written as 0x8000 while glyph pixels come from
			 * MAKECOL15() with bit 15 clear. */
			uint8_t a = (p & 0x8000) ? 0 : 0x80;
			uint8_t b = (((p >> 10) & 0x1F) * 255) / 31;
			uint8_t g = (((p >> 5)  & 0x1F) * 255) / 31;
			uint8_t r = (( p        & 0x1F) * 255) / 31;
			dst[x] = ((uint32_t)a << 24) | ((uint32_t)b << 16)
			       | ((uint32_t)g << 8)  |  (uint32_t)r;
		}
	}
}

static void expand_buffer_to_upload(maxsdk_ui_texture_t *tex)
{
	if (!tex->upload_buffer || !tex->buffer) return;

	int w = (int)tex->texture.width;
	int copy_h = tex->height < (int)tex->texture.height ? tex->height : (int)tex->texture.height;
	int copy_w = tex->width < w ? tex->width : w;
	int y;

	for (y = 0; y < copy_h; y++)
	{
		convert_row_to_8888(
			tex->upload_buffer + (size_t)y * w,
			tex->buffer        + (size_t)y * tex->pitch,
			copy_w, tex->format);
		/* Zero-fill any padding to the rounded width. */
		if (copy_w < w)
			memset(tex->upload_buffer + (size_t)y * w + copy_w, 0,
				(size_t)(w - copy_w) * 4);
	}
}

static void expand_region_to_upload(maxsdk_ui_texture_t *tex,
	uint32_t *dst, int width, int height)
{
	int y;

	if (!tex || !tex->buffer || !dst || width <= 0 || height <= 0)
		return;

	for (y = 0; y < height; y++)
		convert_row_to_8888(dst + (size_t)y * width,
			tex->buffer + (size_t)y * tex->pitch,
			width, tex->format);
}

static void maxsdk_ui_draw_term(void *data)
{
	maxsdk_ui_data_t *d = (maxsdk_ui_data_t *)data;
	maxsdk_ui_release_buffers(d);
}

static void maxsdk_ui_draw_getLogicalSize(void *data, int output_width, int output_height,
	int *logical_width, int *logical_height)
{
	int width;
	int height;

	(void)data;
	ui_layout_compute_responsive_size(output_width, output_height, &width, &height);

	/* Keep the established 480-unit horizontal menu layout in 240p, but use a
	 * dedicated 240-line logical canvas instead of inheriting the much denser
	 * responsive 480-line layout. The 224-line active picture then scales this
	 * canvas only slightly vertically, keeping text readable while still
	 * exposing enough rows for practical menu navigation. */
	if (maxsdk_video_is_240p()) {
		width = 480;
		height = 240;
	}

	if (logical_width) *logical_width = width;
	if (logical_height) *logical_height = height;
}

static void maxsdk_ui_draw_getOutputViewport(void *data,
	int output_width, int output_height,
	int *x, int *y, int *width, int *height)
{
	int viewport_width = output_width;
	int viewport_height = output_height;
	int viewport_x = 0;
	int viewport_y = 0;

	(void)data;
	/* Start from the gameplay-safe vertical active picture. */
	if (video_driver && video_driver->getPresentationViewport)
		video_driver->getPresentationViewport(video_data,
			output_width, output_height,
			&viewport_x, &viewport_y, &viewport_width, &viewport_height);

	/* NTSC CRT/CRTC presentation commonly hides part of the 704-wide timing at
	 * the horizontal edges. Keep menus and HUD text inside the conventional
	 * centered 640-pixel UI-safe region without constraining gameplay itself.
	 * Applying this inset to gameplay caused the earlier right-edge clipping;
	 * it belongs only to UI/HUD layout. */
	if (maxsdk_video_is_ntsc() && viewport_width >= 704) {
		viewport_x += 32;
		viewport_width -= 64;
	}

	if (x) *x = viewport_x;
	if (y) *y = viewport_y;
	if (width) *width = viewport_width;
	if (height) *height = viewport_height;
}

/*------------------------------------------------------
	Texture management
------------------------------------------------------*/

static void maxsdk_ui_draw_uploadTexture(void *data, int slot,
	const uint16_t *pixels, int w, int h, int pitch, int format, int source_tiled8x8)
{
	maxsdk_ui_data_t *d = (maxsdk_ui_data_t *)data;
	maxsdk_ui_texture_t *tex;

	if (slot < 0 || slot >= UI_TEXTURE_MAX ||
	    w <= 0 || h <= 0 || pitch < w ||
	    pitch > maxsdk_ui_buffer_shapes[slot].pitch ||
	    h > maxsdk_ui_buffer_shapes[slot].height)
		return;

	tex = &d->textures[slot];

	tex->format = format;
	tex->width = w;
	tex->height = h;
	tex->pitch = pitch;

	if (pixels)
	{
		if (!ui_texture_copy_to_linear16(tex->buffer, tex->pitch,
			pixels, pitch, w, h, source_tiled8x8))
			return;
		tex->buffer_valid = 1;
		tex->vram_valid = 0;
	}
	ensure_vram(tex);
}

static void maxsdk_ui_draw_clearTexture(void *data, int slot, int w, int h, int pitch)
{
	maxsdk_ui_data_t *d = (maxsdk_ui_data_t *)data;
	maxsdk_ui_texture_t *tex;
	int y;

	if (slot < 0 || slot >= UI_TEXTURE_MAX ||
	    w <= 0 || h <= 0 || pitch < w ||
	    pitch > maxsdk_ui_buffer_shapes[slot].pitch ||
	    h > maxsdk_ui_buffer_shapes[slot].height)
		return;

	tex = &d->textures[slot];

	for (y = 0; y < h; y++)
		memset(tex->buffer + y * pitch, 0, w * sizeof(uint16_t));

	tex->width = w;
	tex->height = h;
	tex->pitch = pitch;
	tex->buffer_valid = 1;
	tex->vram_valid = 0;
	ensure_vram(tex);
}

static uint16_t *maxsdk_ui_draw_getTextureBasePtr(void *data, int slot)
{
	maxsdk_ui_data_t *d = (maxsdk_ui_data_t *)data;

	if (slot < 0 || slot >= UI_TEXTURE_MAX)
		return NULL;

	/* The caller (ui_draw.c make_font_texture etc.) will write directly to
	 * this buffer, so VRAM is now stale. */
	d->textures[slot].buffer_valid = 1;
	d->textures[slot].vram_valid = 0;
	return d->textures[slot].buffer;
}

/*------------------------------------------------------
	Drawing primitives (delegate to video_driver)
------------------------------------------------------*/

/* A CT32 transfer's width must be even (GS manual 4.1), and an even height makes
   its data whole quadwords. The extra texels are not drawn. PCSX2 takes any
   width; a PS2 shows the glyphs after an odd one wrong. */
#define EVEN(v) (((v) + 1) & ~1)

static void fill_draw(ui_texture_draw_t *draw, maxsdk_tex_t *texture)
{
	draw->texture = texture;
	draw->format = (int)texture->psm;
	draw->swizzled = 0;
	draw->width = (int)texture->width;
	draw->height = (int)texture->height;
	draw->stride = (int)texture->width;
}

static bool maxsdk_ui_draw_prepareTextureDraw(void *data, int slot,
	int su, int sv, int sw, int sh, ui_texture_draw_t *draw)
{
	maxsdk_ui_data_t *d = (maxsdk_ui_data_t *)data;
	maxsdk_ui_texture_t *tex;
	maxsdk_tex_t *gst;

	if (!draw || slot < 0 || slot >= UI_TEXTURE_MAX)
		return false;

	d->prepared_font_ring = 0;
	d->prepared_font_scratch = 0;
	tex = &d->textures[slot];
	gst = &tex->texture;
	if (!gst->pages || !tex->buffer || !tex->upload_buffer)
		return false;

	if (slot == UI_TEXTURE_FONT && su == 0 && sv == 0 &&
	    sw > 0 && sh > 0 &&
	    sw <= UI_FONT_RING_WIDTH && sh <= UI_FONT_RING_HEIGHT)
	{
		maxsdk_ui_font_ring_entry_t *entry = &d->font_ring[d->font_ring_next];

		int even_w = EVEN(sw);
		int even_h = EVEN(sh);

		expand_region_to_upload(tex, entry->upload_buffer, even_w, even_h);
		maxsdk_tex_upload(&entry->texture, entry->upload_buffer, even_w, even_h);
		fill_draw(draw, &entry->texture);
		d->prepared_font_ring = 1;
		return true;
	}

	/* Lazy upload: any path that mutates the CPU buffer (uploadTexture,
	 * clearTexture, getTextureBasePtr) clears vram_valid. Expand the
	 * 16-bit ABGR4444/1555 staging buffer into 32-bit ABGR8888 (the GS has
	 * no 4444 format) and send it.
	 * UI_TEXTURE_FONT is a scratch texture: common/ui_draw.c obtains its CPU
	 * pointer once and rewrites it directly for every glyph/shadow, with no
	 * callback that could invalidate vram_valid, so it goes on every draw. */
	if (tex->buffer_valid && (!tex->vram_valid || slot == UI_TEXTURE_FONT))
	{
		if (slot == UI_TEXTURE_FONT && su == 0 && sv == 0 &&
		    sw > 0 && sh > 0 && sw <= (int)gst->width && sh <= (int)gst->height)
		{
			expand_region_to_upload(tex, tex->upload_buffer, EVEN(sw), EVEN(sh));
			maxsdk_tex_upload(gst, tex->upload_buffer, EVEN(sw), EVEN(sh));
		}
		else
		{
			expand_buffer_to_upload(tex);
			maxsdk_tex_upload(gst, tex->upload_buffer, (int)gst->width, (int)gst->height);
		}
		tex->vram_valid = 1;
	}

	fill_draw(draw, gst);
	d->prepared_font_scratch = slot == UI_TEXTURE_FONT;
	return true;
}

static void maxsdk_ui_draw_finishTextureDraw(void *data, int slot)
{
	maxsdk_ui_data_t *d = (maxsdk_ui_data_t *)data;

	if (slot != UI_TEXTURE_FONT)
		return;

	if (d->prepared_font_ring)
	{
		d->font_ring_next++;
		if (d->font_ring_next == UI_FONT_RING_SIZE)
		{
			/* The ring's upload buffers cannot be reused until the GS has
			 * them. */
			video_driver->flushAndWait(d->video_data);
			d->font_ring_next = 0;
		}
	}
	else if (d->prepared_font_scratch)
	{
		/* Oversized scratch draws (the NJEMU logo) use the single texture:
		 * flush before common/ui_draw.c rewrites that same upload buffer. */
		video_driver->flushAndWait(d->video_data);
	}

	d->prepared_font_ring = 0;
	d->prepared_font_scratch = 0;
}

/******************************************************************************
	Driver instance
******************************************************************************/

const ui_draw_driver_t maxsdk_ui_draw_driver = {
	maxsdk_ui_draw_init,
	maxsdk_ui_draw_term,
	maxsdk_ui_draw_getLogicalSize,
	UI_DRAW_CAP_FILL_LOGICAL_HEIGHT,
	maxsdk_ui_draw_uploadTexture,
	maxsdk_ui_draw_clearTexture,
	maxsdk_ui_draw_getTextureBasePtr,
	maxsdk_ui_draw_prepareTextureDraw,
	maxsdk_ui_draw_finishTextureDraw,
	maxsdk_ui_draw_getOutputViewport,
};
