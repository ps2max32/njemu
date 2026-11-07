/* The GS on maxsdk: GIF chains built here, no toolkit in between.

   Drawing goes into a chain of CNT and REF tags in one of two buffers, written
   through UCAB, and the GIF channel sends it at the flip, or earlier when the
   buffer fills. Sprites go as REGLIST (UV, XYZ2): a quadword a vertex.

   All waits sleep, on the GIF channel's interrupt and the timer of usleep: the
   sound thread runs below the main thread, in these waits.

   With VM what the DMAC reads and what interrupt handlers touch must not be
   paged: the chains, atlases, the UI's buffers and this file's state (the
   vblank handler reads its displays) are pinned; palettes, which are the
   emulators' own variables, are copied into the chain.

   GS memory is laid out once, for the largest output, and stays: a change of
   output mode resets the CRTC and the GS registers, not GS memory, so nothing
   is uploaded again.

   Colour: 16-bit (CT16) throughout. CLUT entries are CT16 with TEXA 0 and 0x80,
   so a visible entry (bit 15 clear) has alpha 0 and 0x8000, the transparent
   one, alpha 0x80; the game's sprites pass an alpha test for 0. They need no
   blending, so ALPHA_1 stays the UI's Cs*As + Cd*(1 - As). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <mx/cpu.h>
#include <mx/dma.h>
#include <mx/gs.h>
#include <mx/irq.h>
#include <mx/mem.h>
#include <mx/pkt.h>
#include <mx/tex.h>
#include <mx/thread.h>
#include <mx/timer.h>

#include "emucfg.h"
#include "common/display_mode.h"
#include "common/emulator_options.h"
#include "common/video_driver.h"
#include "common/video_geometry.h"
#include "maxsdk/maxsdk_test.h"
#include "maxsdk/maxsdk_video.h"
#include "ps2/ps2_native_240p_policy.h"
#include "ps2/ps2_video_filter_policy.h"

/* CLUT: rows of 256 CT16 entries in a buffer of its own for each bank, read in
   CSM2: TEXCLUT's COV picks the row. */
#define CLUT_WIDTH 256
#define CLUT_CBW (CLUT_WIDTH / 64)
#define CLUT_BANKS_MAX 2

/* The work frame holds every system's source clip, and the GUI's 480 x 272. */
#define WORK_WIDTH BUF_WIDTH
#define WORK_HEIGHT SCR_HEIGHT

#define MAX_WIDTH 704
#define MAX_HEIGHT 480

#define QUEUE_BYTES (512u * 1024u)
/* room kept for what closes a chain and for a batch's registers */
#define QUEUE_TAIL_QW 32u
#define BATCH_MAX 2048u

/* primitive coordinates: window (0, 0) is at 2048, 2048 */
#define OFFSET (2048 << 4)

#define TEXCLUT(cbw, cou, cov) \
	((uint64_t)(cbw) | (uint64_t)(cou) << 6 | (uint64_t)(cov) << 12)
#define ALPHA_TEST_EQUAL 4
#define Z_ALWAYS 1
#define Z_GEQUAL 2
#define WHITE 0x80808080ull /* RGBAQ: a texel as it is */

typedef struct output_mode {
	struct mx_video_mode video;
	uint16_t width, height;
	uint8_t aspect_num, aspect_den;
} output_mode_t;

/* The whole NTSC raster, as far as DISPLAY reaches: 2880 VCK at 4 a pixel, so a
   704-pixel framebuffer has the pixel aspect of the 13.5 MHz standard (10:11;
   a line of 240p takes two of 480i: 10:22). All are 60 Hz timings, as the
   emulated systems are, on PAL consoles too. */
static const output_mode_t output_modes[VIDEO_OUTPUT_MODE_COUNT] = {
	{ { 0x02, 0, 0, 60, 2880, 240, 492, 17 }, 704, 240, 10, 22 },
	{ { 0x02, 1, 0, 60, 2880, 480, 492, 34 }, 704, 480, 10, 11 },
	{ { 0x50, 0, 0, 60, 1440, 480, 232, 35 }, 704, 480, 10, 11 },
};

#if (EMU_SYSTEM == CPS1 || EMU_SYSTEM == CPS2)
/* 384 x 224 scanned out as it is, without fractional stretching. */
static const output_mode_t cps_native_mode = {
	{ 0x02, 0, 0, 60, 2880, 240, 492, 17 },
	PS2_CPS_NATIVE_WIDTH, PS2_CPS_NATIVE_HEIGHT, 5, 6
};
#endif

typedef struct texture_layer {
	maxsdk_tex_t tex;
	/* Renderers write atlas rectangles a bit at a time: only rows changed,
	   in bands of DIRTY_BAND, go to the GS. */
	uint64_t dirty_bands;
} texture_layer_t;

#define DIRTY_BAND 8
#define DIRTY_BANDS 64

typedef struct maxsdk_video {
	output_mode_t mode;
	int output_mode;

	/* the framebuffers: pages for the largest output */
	uint32_t fb_page[2];
	struct mx_rt rt[2];
	struct mx_display display[2];
	int draw; /* framebuffer drawn into; the other one is shown */
	int offset_x, offset_y;

	maxsdk_tex_t work;
	uint32_t z_page;
	texture_layer_t *layers;
	uint8_t layer_count;
	uint8_t *layer_mem;

#if defined(GUI)
	/* The GUI's surface for the CPU: layer 0 may be T8, so it has its own. */
	maxsdk_tex_t ui_scratch;
	uint8_t ui_scratch_dirty;
#endif

	uint16_t *clut_base;
	uint16_t clut_entries;
	uint8_t clut_banks;
	uint8_t clut_rows;
	uint32_t clut_page[CLUT_BANKS_MAX];

	uint32_t texclut_cov;

	/* the GS's state, as last written */
	uint8_t alpha_test;
	uint8_t z_test;
	uint8_t z_write;
} maxsdk_video_t;

static maxsdk_video_t *video;

/* ---- the queue ------------------------------------------------------------ */

static uint64_t *buffers[2]; /* the DMAC's view */
static int buffer;           /* the one being built */
static uint64_t *cur;        /* where it is written, through UCAB */
static uint64_t *cnt_tag;    /* the open CNT tag, or 0 */
static uint64_t *limit;
static int in_flight;
static uint32_t finish_seen;
static int gif_sema MX_CORE_DATA = -1;

MX_CORE_FN static void gif_irq(void *arg)
{
	(void)arg;
	mx_isema_signal(gif_sema);
}

static void queue_reset(void)
{
	cur = MX_UCAB(buffers[buffer]);
	limit = cur + QUEUE_BYTES / sizeof(uint64_t) - QUEUE_TAIL_QW * 2;
	cnt_tag = NULL;
}

/* Until the GS has done what was sent. */
static void wait_idle(void)
{
	uint32_t t0;

	if (!in_flight)
		return;
	while (mx_dma_busy(MX_CH_GIF))
		mx_sema_wait(gif_sema);
	mx_sema_try(gif_sema);
	/* what the GIF holds yet: microseconds, not worth a sleep */
	t0 = mx_count();
	while (mx_gs.finish == finish_seen && mx_count() - t0 < MX_EE_HZ / 4)
		;
	in_flight = 0;
}

static inline void q_open(void)
{
	if (cnt_tag == NULL) {
		cnt_tag = cur;
		cur += 2;
	}
}

static inline void q_close(void)
{
	if (cnt_tag != NULL) {
		uint32_t qwc = (uint32_t)(cur - cnt_tag) / 2 - 1;

		cnt_tag[0] = mx_dmatag(MX_TAG_CNT, qwc, MX_PADDR_NONE, 0);
		cnt_tag[1] = 0;
		cnt_tag = NULL;
	}
}

static inline void ad_begin(uint32_t writes)
{
	q_open();
	cur[0] = mx_giftag(writes, 1, -1, MX_GIF_FLG_PACKED, 1);
	cur[1] = MX_GIF_REG_AD;
	cur += 2;
}

static inline void ad(uint32_t reg, uint64_t value)
{
	cur[0] = value;
	cur[1] = reg;
	cur += 2;
}

static void q_submit(void)
{
	if (cur == MX_UCAB(buffers[buffer]))
		return;
	ad_begin(1);
	ad(MX_GS_REG_FINISH, 1);
	q_close();
	cur[0] = mx_dmatag(MX_TAG_END, 0, MX_PADDR_NONE, 0);
	cur[1] = 0;
	cur += 2;
	wait_idle();
	mx_sync();
	finish_seen = mx_gs.finish;
	in_flight = 1;
	mx_dma_chain(MX_CH_GIF, buffers[buffer], 0);
	buffer ^= 1;
	queue_reset();
}

/* Room for qw quadwords in the buffer. What was queued keeps its order, and the
   GS keeps its registers, so a batch can go on after this. */
static inline void q_reserve(uint32_t qw)
{
	if (cur + (size_t)qw * 2 >= limit)
		q_submit();
}

/* ---- registers ------------------------------------------------------------ */

static uint64_t test_value(void)
{
	return mx_gs_test(video->alpha_test, ALPHA_TEST_EQUAL, 0, 0, 0, 0, 1, video->z_test);
}

static void set_alpha_test(int on)
{
	video->alpha_test = (uint8_t)on;
	q_reserve(2);
	ad_begin(1);
	ad(MX_GS_REG_TEST_1, test_value());
}

static uint64_t zbuf_value(void)
{
	return mx_gs_zbuf(video->z_page, MX_GS_PSM_Z16, !video->z_write);
}

static void set_target(uint32_t page, uint32_t fbw, uint32_t psm, uint32_t width, uint32_t height)
{
	q_reserve(3);
	ad_begin(2);
	ad(MX_GS_REG_FRAME_1, mx_gs_frame(page, fbw, psm, 0));
	ad(MX_GS_REG_SCISSOR_1, mx_gs_scissor(0, width - 1, 0, height - 1));
}

static void set_target_tex(const maxsdk_tex_t *tex)
{
	set_target(tex->page, tex->tbw, tex->psm, tex->width, tex->height);
}

static void set_target_frame(int index)
{
	const struct mx_rt *rt = &video->rt[index];

	set_target(rt->fbp, rt->fbw, rt->psm, rt->w, rt->h);
}

static void set_scissor(uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1)
{
	q_reserve(2);
	ad_begin(1);
	ad(MX_GS_REG_SCISSOR_1, mx_gs_scissor(x0, x1, y0, y1));
}

/* The state that a GS reset clears. */
static void write_gs_state(void)
{
	q_reserve(13);
	ad_begin(12);
	ad(MX_GS_REG_PRMODECONT, 1);
	ad(MX_GS_REG_XYOFFSET_1, mx_gs_xyoffset(OFFSET, OFFSET));
	ad(MX_GS_REG_TEST_1, test_value());
	ad(MX_GS_REG_ZBUF_1, zbuf_value());
	ad(MX_GS_REG_CLAMP_1, mx_gs_clamp(1, 1, 0, 0, 0, 0));
	ad(MX_GS_REG_COLCLAMP, 1);
	ad(MX_GS_REG_DTHE, 0);
	ad(MX_GS_REG_PABE, 0);
	ad(MX_GS_REG_FBA_1, 0);
	ad(MX_GS_REG_ALPHA_1, mx_gs_alpha(0, 1, 0, 1, 0));
	ad(MX_GS_REG_TEXA, MX_SET(GS_TEXA_TA0, 0) | MX_SET(GS_TEXA_TA1, 0x80));
	ad(MX_GS_REG_TEX1_1, mx_gs_tex1(0, 0, 0, 0, 0, 0, 0));
	video->texclut_cov = ~0u;
}

/* ---- textures ------------------------------------------------------------- */

static uint8_t log2_up(uint32_t v)
{
	uint8_t n = 0;

	while ((1u << n) < v)
		n++;
	return n;
}

bool maxsdk_tex_alloc(maxsdk_tex_t *tex, int width, int height, uint32_t psm)
{
	uint32_t pages = mx_rt_pages((uint32_t)width, (uint32_t)height, psm);
	uint32_t page = mx_vram_alloc(pages);

	if (page == ~0u)
		return false;
	tex->page = page;
	tex->pages = pages;
	tex->tbw = mx_tex_tbw((uint32_t)width, psm);
	tex->psm = psm;
	tex->width = (uint32_t)width;
	tex->height = (uint32_t)height;
	tex->tw = log2_up((uint32_t)width);
	tex->th = log2_up((uint32_t)height);
	return true;
}

void maxsdk_tex_free(maxsdk_tex_t *tex)
{
	if (tex->pages != 0)
		mx_vram_free(tex->page, tex->pages);
	tex->pages = 0;
}

static uint32_t tex_bytes(uint32_t width, uint32_t height, uint32_t psm)
{
	switch (psm) {
	case MX_GS_PSM_T8:
		return width * height;
	case MX_GS_PSM_CT32:
		return width * height * 4;
	default:
		return width * height * 2;
	}
}

/* Queue a transfer to (0, y) of the buffer at block bp. */
static void upload(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t y,
	uint32_t width, uint32_t height, const void *pixels)
{
	uint32_t bytes = tex_bytes(width, height, psm);
	uint32_t qwc = (bytes + 15) / 16;
	const uint8_t *data = (const uint8_t *)pixels;
	int inline_copy = mx_is_vm(data);

	if (!inline_copy)
		mx_dcache_writeback(data, bytes);
	q_reserve(8 + (qwc / 0x7FFF + 1) * 4 + (inline_copy ? qwc : 0));
	ad_begin(4);
	ad(MX_GS_REG_BITBLTBUF, mx_gs_bitbltbuf(0, 0, 0, bp, bw, psm));
	ad(MX_GS_REG_TRXPOS, mx_gs_trxpos(0, 0, 0, y, 0));
	ad(MX_GS_REG_TRXREG, mx_gs_trxreg(width, height));
	ad(MX_GS_REG_TRXDIR, 0);
	while (qwc > 0) {
		uint32_t n = qwc > 0x7FFF ? 0x7FFF : qwc;

		q_open();
		cur[0] = mx_giftag(n, 1, -1, MX_GIF_FLG_IMAGE, 0);
		cur[1] = 0;
		cur += 2;
		if (inline_copy) {
			memcpy(cur, data, (size_t)n * 16);
			cur += n * 2;
		} else {
			q_close();
			cur[0] = mx_dmatag(MX_TAG_REF, n, mx_paddr_of(data), 0);
			cur[1] = 0;
			cur += 2;
		}
		data += n * 16;
		qwc -= n;
	}
	ad_begin(1);
	ad(MX_GS_REG_TEXFLUSH, 0);
}

void maxsdk_tex_upload(const maxsdk_tex_t *tex, const void *pixels, int width, int height)
{
	upload(MX_GS_PAGE(tex->page), tex->tbw, tex->psm, 0, (uint32_t)width, (uint32_t)height, pixels);
}

static uint64_t tex0_value(const maxsdk_tex_t *tex, int tcc)
{
	return mx_gs_tex0(MX_GS_PAGE(tex->page), tex->tbw, tex->psm, tex->tw, tex->th, (uint32_t)tcc, 0);
}

/* ---- output --------------------------------------------------------------- */

static int sanitize_output_mode(int index)
{
	return index < 0 || index >= VIDEO_OUTPUT_MODE_COUNT ? VIDEO_OUTPUT_480I : index;
}

static const output_mode_t *output_mode_for(int output_mode, int display_mode)
{
#if (EMU_SYSTEM == CPS1 || EMU_SYSTEM == CPS2)
	if (ps2_use_native_cps_240p(1, output_mode == VIDEO_OUTPUT_240P,
		display_mode == DISPLAY_MODE_NATIVE_PIXELS))
		return &cps_native_mode;
#else
	(void)display_mode;
#endif
	return &output_modes[sanitize_output_mode(output_mode)];
}

bool maxsdk_video_is_ntsc(void)
{
	return video != NULL && video->mode.video.code == 0x02;
}

bool maxsdk_video_is_240p(void)
{
	return maxsdk_video_is_ntsc() && !video->mode.video.interlace;
}

/* The board's offset of the progressive modes: BIOSes after 2001-06-08 have it
   (ROMVER at the end of the ROM: VVVVRTYYYYMMDD). */
static void add_board_offset(struct mx_video_mode *mode)
{
	const volatile char *date = (const volatile char *)0x1FC7FF4E + 6;
	uint32_t value = 0;
	int dx, dy;
	int i;

	if (mode->code == 0x02)
		return;
	for (i = 0; i < 8; i++)
		value = value * 10 + (uint32_t)(date[i] - '0');
	if (value > 20010608) {
		mx_video_board_offset(mode->code, &dx, &dy);
		mode->dx = (uint16_t)(mode->dx + dx);
		mode->dy = (uint16_t)(mode->dy + dy);
	}
}

/* The displays and render targets of the mode in video->mode. */
static void setup_displays(void)
{
	const output_mode_t *mode = &video->mode;
	int i;

	for (i = 0; i < 2; i++) {
		mx_rt_init(&video->rt[i], video->fb_page[i], mode->width, mode->height,
			MX_GS_PSM_CT16, 0, MX_RT_NOZ);
		mx_display_init(&video->display[i], &video->rt[i]);
	}
}

static void place_displays(void)
{
	uint32_t mag = mx_circuit_magh(&mx_video, &video->display[0].c[1]);
	int i;

	for (i = 0; i < 2; i++) {
		struct mx_circuit *circuit = &video->display[i].c[1];

		circuit->dx = video->offset_x * (int)mag;
		/* an interlaced picture keeps the order of its fields */
		circuit->dy = mx_video.interlace ? video->offset_y & ~1 : video->offset_y;
	}
}

static void maxsdk_setOutputOffset(void *data, int x, int y)
{
	maxsdk_video_t *v = (maxsdk_video_t *)data;

	if (v == NULL)
		return;
	v->offset_x = x;
	v->offset_y = y;
	place_displays();
	mx_display_apply(&v->display[v->draw ^ 1]);
}

static void clear_rect(uint32_t width, uint32_t height, uint64_t rgbaq)
{
	uint32_t slices = (width + 63) / 64;
	uint32_t i;

	/* Unconditional writes, in strips a page wide: the texture cache and the
	   page buffers like them better. */
	q_reserve(slices * 2 + 8);
	ad_begin(slices * 2 + 3);
	ad(MX_GS_REG_TEST_1, mx_gs_test(0, ALPHA_TEST_EQUAL, 0, 0, 0, 0, 1, Z_ALWAYS));
	ad(MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0));
	ad(MX_GS_REG_RGBAQ, rgbaq);
	for (i = 0; i < slices; i++) {
		uint32_t x0 = i * 64;
		uint32_t x1 = x0 + 64 < width ? x0 + 64 : width;

		ad(MX_GS_REG_XYZ2, mx_gs_xyz(OFFSET + (x0 << 4), OFFSET, 0));
		ad(MX_GS_REG_XYZ2, mx_gs_xyz(OFFSET + (x1 << 4), OFFSET + (height << 4), 0));
	}
	ad_begin(1);
	ad(MX_GS_REG_TEST_1, test_value());
}

static uint64_t rgbaq_of(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
	return r | g << 8 | b << 16 | (uint64_t)a << 24 | 0x3F800000ull << 32;
}

static void fill_frame(int index, uint64_t rgbaq)
{
	uint32_t width, height;

	switch (index) {
	case COMMON_GRAPHIC_OBJECTS_SHOW_FRAME_BUFFER:
		set_target_frame(video->draw ^ 1);
		width = video->rt[0].w;
		height = video->rt[0].h;
		break;
	case COMMON_GRAPHIC_OBJECTS_DRAW_FRAME_BUFFER:
		set_target_frame(video->draw);
		width = video->rt[0].w;
		height = video->rt[0].h;
		break;
	case COMMON_GRAPHIC_OBJECTS_SCREEN_BITMAP:
		set_target_tex(&video->work);
		width = video->work.width;
		height = video->work.height;
		break;
	default:
		return;
	}
	clear_rect(width, height, rgbaq);
}

static void maxsdk_flushAndWait(void *data)
{
	(void)data;
	q_submit();
	wait_idle();
}

static void maxsdk_flipScreen(void *data, bool vsync)
{
	maxsdk_video_t *v = (maxsdk_video_t *)data;

	/* the back buffer is shown once its drawing is done */
	maxsdk_flushAndWait(v);
	if (vsync) {
		mx_display_show(&v->display[v->draw]);
		while (mx_gs.pending != NULL)
			usleep(500);
	} else {
		mx_display_apply(&v->display[v->draw]);
	}
#ifdef MAXSDK_TEST
	maxsdk_test_flip(&v->rt[v->draw]);
#endif
	v->draw ^= 1;
	set_target_frame(v->draw);
}

static int set_output(const output_mode_t *mode)
{
	struct mx_video_mode timing = mode->video;

	add_board_offset(&timing);
	if (mx_video_set(&timing) < 0)
		return 0;
	video->mode = *mode;
	setup_displays();
	place_displays();
	write_gs_state();
	fill_frame(COMMON_GRAPHIC_OBJECTS_SHOW_FRAME_BUFFER, rgbaq_of(0, 0, 0, 0x80));
	fill_frame(COMMON_GRAPHIC_OBJECTS_DRAW_FRAME_BUFFER, rgbaq_of(0, 0, 0, 0x80));
	maxsdk_flushAndWait(video);
	mx_display_apply(&video->display[video->draw ^ 1]);
	set_target_frame(video->draw);
	video_set_pixel_aspect_ratio(mode->aspect_num, mode->aspect_den);
	return 1;
}

static int maxsdk_setOutputMode(void *data, int mode_index)
{
	maxsdk_video_t *v = (maxsdk_video_t *)data;
	const output_mode_t *requested;
	output_mode_t previous;

	if (v == NULL)
		return 0;
	mode_index = sanitize_output_mode(mode_index);
	requested = output_mode_for(mode_index, option_display_mode);
	if (mode_index == v->output_mode && v->mode.width == requested->width &&
		v->mode.height == requested->height) {
		maxsdk_setOutputOffset(v, option_video_offset_x, option_video_offset_y);
		return 1;
	}
	previous = v->mode;
	maxsdk_flushAndWait(v);
	v->offset_x = option_video_offset_x;
	v->offset_y = option_video_offset_y;
	if (set_output(requested)) {
		v->output_mode = mode_index;
		return 1;
	}
	/* this console does not have the mode */
	(void)set_output(&previous);
	return 0;
}

/* ---- init ----------------------------------------------------------------- */

static void *maxsdk_getNativeContext(void *data)
{
	return data;
}

static void maxsdk_getOutputSize(void *data, int *width, int *height)
{
	maxsdk_video_t *v = (maxsdk_video_t *)data;

	if (width)
		*width = v ? v->mode.width : SCR_WIDTH;
	if (height)
		*height = v ? v->mode.height : SCR_HEIGHT;
}

static void maxsdk_getPresentationViewport(void *data, int output_width, int output_height,
	int *x, int *y, int *width, int *height)
{
	int viewport_height = output_height;
	int viewport_y = 0;

	(void)data;
	/* NTSC's active picture is 224 or 448 lines, in 240 or 480: keep game and
	   text inside it. Horizontally all of the raster shows. */
	if (maxsdk_video_is_ntsc()) {
		viewport_height = maxsdk_video_is_240p() ? 224 : 448;
		if (viewport_height > output_height)
			viewport_height = output_height;
		viewport_y = (output_height - viewport_height) / 2;
	}
	if (x) *x = 0;
	if (y) *y = viewport_y;
	if (width) *width = output_width;
	if (height) *height = viewport_height;
}

static void maxsdk_free(void *data);

static void *maxsdk_init(layer_texture_info_t *layer_textures, uint8_t layer_textures_count,
	clut_info_t *clut_info)
{
	const output_mode_t *mode;
	size_t total = 0;
	size_t offset = 0;
	int i;

	video = (maxsdk_video_t *)mx_dma_alloc(sizeof(*video));
	if (video == NULL)
		return NULL;
	memset(video, 0, sizeof(*video));

	for (i = 0; i < 2; i++) {
		if (buffers[i] == NULL)
			buffers[i] = (uint64_t *)mx_dma_alloc(QUEUE_BYTES);
		if (buffers[i] == NULL)
			goto failed;
	}
	if (gif_sema < 0)
		gif_sema = mx_sema_create(0, 1);
	in_flight = 0;
	buffer = 0;
	queue_reset();

	video->z_test = Z_ALWAYS;
	video->output_mode = sanitize_output_mode(option_video_output_mode);
	video->offset_x = option_video_offset_x;
	video->offset_y = option_video_offset_y;
	mode = output_mode_for(video->output_mode, option_display_mode);

	/* mx_gs_init_video clears the GS memory map, so the first mode is set
	   with it; the buffers are placed before anything is drawn */
	if (mx_gs_init_video(&mode->video) < 0)
		goto failed;
	mx_irq_dmac(MX_CH_GIF, gif_irq, NULL);

	for (i = 0; i < 2; i++) {
		video->fb_page[i] = mx_vram_alloc(mx_rt_pages(MAX_WIDTH, MAX_HEIGHT, MX_GS_PSM_CT16));
		if (video->fb_page[i] == ~0u)
			goto failed;
	}
	if (!maxsdk_tex_alloc(&video->work, WORK_WIDTH, WORK_HEIGHT, MX_GS_PSM_CT16))
		goto failed;
#if (EMU_SYSTEM == CPS2)
	/* the priority passes draw into the work frame, so Z is as large as that */
	video->z_page = mx_vram_alloc(mx_rt_pages(WORK_WIDTH, WORK_HEIGHT, MX_GS_PSM_Z16));
	if (video->z_page == ~0u)
		goto failed;
#endif

	for (i = 0; i < layer_textures_count; i++)
		total += layer_textures[i].width * layer_textures[i].height *
			layer_textures[i].bytes_per_pixel;
	video->layer_mem = (uint8_t *)mx_dma_alloc((uint32_t)total);
	video->layers = (texture_layer_t *)calloc(layer_textures_count, sizeof(texture_layer_t));
	if (video->layer_mem == NULL || video->layers == NULL)
		goto failed;
	video->layer_count = layer_textures_count;
	for (i = 0; i < layer_textures_count; i++) {
		maxsdk_tex_t *tex = &video->layers[i].tex;
		const layer_texture_info_t *info = &layer_textures[i];

		if (!maxsdk_tex_alloc(tex, (int)info->width, (int)info->height,
			info->bytes_per_pixel == 1 ? MX_GS_PSM_T8 : MX_GS_PSM_CT16))
			goto failed;
		tex->mem = video->layer_mem + offset;
		offset += info->width * info->height * info->bytes_per_pixel;
	}

	video->clut_base = clut_info->base;
	video->clut_entries = clut_info->entries_per_bank;
	video->clut_banks = clut_info->bank_count;
	video->clut_rows = (uint8_t)((clut_info->entries_per_bank + CLUT_WIDTH - 1) / CLUT_WIDTH);
	for (i = 0; i < video->clut_banks && i < CLUT_BANKS_MAX; i++) {
		video->clut_page[i] = mx_vram_alloc(mx_rt_pages(CLUT_WIDTH, video->clut_rows, MX_GS_PSM_CT16));
		if (video->clut_page[i] == ~0u)
			goto failed;
	}

#if defined(GUI)
	if (!maxsdk_tex_alloc(&video->ui_scratch, BUF_WIDTH, SCR_HEIGHT, MX_GS_PSM_CT16))
		goto failed;
	video->ui_scratch.mem = (uint8_t *)mx_dma_alloc(BUF_WIDTH * SCR_HEIGHT * 2);
	if (video->ui_scratch.mem == NULL)
		goto failed;
	memset(video->ui_scratch.mem, 0, BUF_WIDTH * SCR_HEIGHT * 2);
#endif

	if (!set_output(mode))
		goto failed;
	fill_frame(COMMON_GRAPHIC_OBJECTS_SCREEN_BITMAP, rgbaq_of(0, 0, 0, 0x80));
	set_target_frame(video->draw);
	return video;

failed:
	printf("video: out of memory\n");
	maxsdk_free(video);
	return NULL;
}

static void maxsdk_free(void *data)
{
	maxsdk_video_t *v = (maxsdk_video_t *)data;

	if (v == NULL)
		return;
	if (cur != NULL)
		maxsdk_flushAndWait(v);
	mx_irq_dmac(MX_CH_GIF, NULL, NULL);
	video_set_pixel_aspect_ratio(1, 1);
#if defined(GUI)
	if (v->ui_scratch.mem != NULL)
		mx_dma_free(v->ui_scratch.mem);
#endif
	if (v->layer_mem != NULL)
		mx_dma_free(v->layer_mem);
	free(v->layers);
	mx_dma_free(v);
	video = NULL;
}

/* ---- frames --------------------------------------------------------------- */

static void maxsdk_waitVsync(void *data)
{
	uint32_t vsync = mx_gs.vsync;

	(void)data;
	while (mx_gs.vsync == vsync)
		usleep(500);
#ifdef MAXSDK_TEST
	maxsdk_test_flip(&video->rt[video->draw ^ 1]);
#endif
}

static void maxsdk_beginFrame(void *data)
{
	(void)data;
	set_target_frame(video->draw);
}

static void maxsdk_endFrame(void *data)
{
	(void)data; /* the chain goes at the flip */
}

static void *maxsdk_frameAddr(void *data, int frame_index, int x, int y)
{
	maxsdk_video_t *v = (maxsdk_video_t *)data;
	maxsdk_tex_t *tex = NULL;
	uint32_t bytes;

#if defined(GUI)
	if (frame_index == COMMON_GRAPHIC_OBJECTS_INITIAL_TEXTURE_LAYER) {
		tex = &v->ui_scratch;
		v->ui_scratch_dirty = 1;
	}
#endif
	if (tex == NULL && frame_index >= COMMON_GRAPHIC_OBJECTS_INITIAL_TEXTURE_LAYER) {
		int layer = frame_index - COMMON_GRAPHIC_OBJECTS_INITIAL_TEXTURE_LAYER;

		if (layer >= v->layer_count)
			return NULL;
		tex = &v->layers[layer].tex;
	}
	/* frame buffers are in GS memory only */
	if (tex == NULL || tex->mem == NULL || x < 0 || y < 0 ||
		(uint32_t)x >= tex->width || (uint32_t)y >= tex->height)
		return NULL;
	bytes = tex->psm == MX_GS_PSM_T8 ? 1 : 2;
	return tex->mem + ((size_t)y * tex->width + (size_t)x) * bytes;
}

static void maxsdk_scissor(void *data, uint16_t left, uint16_t top, uint16_t right, uint16_t bottom)
{
	(void)data;
	set_scissor(left, right, top, bottom);
}

static void maxsdk_clearFrame(void *data, int index)
{
	(void)data;
	fill_frame(index, rgbaq_of(0, 0, 0, 0x80));
}

static void maxsdk_clearScreen(void *data)
{
	/* both buffers: what the GUI left must not show when they swap */
	maxsdk_clearFrame(data, COMMON_GRAPHIC_OBJECTS_SHOW_FRAME_BUFFER);
	maxsdk_clearFrame(data, COMMON_GRAPHIC_OBJECTS_DRAW_FRAME_BUFFER);
}

static void maxsdk_fillFrame(void *data, int frame_index, uint32_t color)
{
	(void)data;
	fill_frame(frame_index, rgbaq_of(color & 0xFF, (color >> 8) & 0xFF,
		(color >> 16) & 0xFF, color >> 24));
}

static void maxsdk_startWorkFrame(void *data, uint32_t color)
{
	maxsdk_video_t *v = (maxsdk_video_t *)data;

	v->texclut_cov = ~0u;
	set_target_tex(&v->work);
	q_reserve(2);
	ad_begin(1);
	ad(MX_GS_REG_TEX1_1, mx_gs_tex1(0, 0, 0, 0, 0, 0, 0));
	clear_rect(v->work.width, v->work.height, rgbaq_of(color & 0xFF, (color >> 8) & 0xFF,
		(color >> 16) & 0xFF, color >> 24));
	set_alpha_test(1);
}

/* ---- copies --------------------------------------------------------------- */

#if defined(GUI)
static void sync_ui_scratch(maxsdk_video_t *v)
{
	if (v->ui_scratch_dirty) {
		maxsdk_tex_upload(&v->ui_scratch, v->ui_scratch.mem, BUF_WIDTH, SCR_HEIGHT);
		v->ui_scratch_dirty = 0;
	}
}
#endif

/* The texture that a frame index reads. */
static bool resolve_source(maxsdk_video_t *v, int index, maxsdk_tex_t *tex)
{
	int layer;

	memset(tex, 0, sizeof(*tex));
	switch (index) {
	case COMMON_GRAPHIC_OBJECTS_SHOW_FRAME_BUFFER:
	case COMMON_GRAPHIC_OBJECTS_DRAW_FRAME_BUFFER: {
		const struct mx_rt *rt = &v->rt[index == COMMON_GRAPHIC_OBJECTS_DRAW_FRAME_BUFFER ?
			v->draw : v->draw ^ 1];

		tex->page = rt->fbp;
		tex->tbw = rt->fbw;
		tex->psm = rt->psm;
		tex->width = rt->w;
		tex->height = rt->h;
		tex->tw = log2_up(rt->w);
		tex->th = log2_up(rt->h);
		return true;
	}
	case COMMON_GRAPHIC_OBJECTS_SCREEN_BITMAP:
		*tex = v->work;
		return true;
	case COMMON_GRAPHIC_OBJECTS_INITIAL_TEXTURE_LAYER:
#if defined(GUI)
		sync_ui_scratch(v);
		*tex = v->ui_scratch;
		return true;
#else
		if (v->layer_count == 0)
			return false;
		*tex = v->layers[0].tex;
		return true;
#endif
	default:
		layer = index - COMMON_GRAPHIC_OBJECTS_INITIAL_TEXTURE_LAYER;
		if (layer < 0 || layer >= v->layer_count)
			return false;
		*tex = v->layers[layer].tex;
		return true;
	}
}

static bool set_destination(maxsdk_video_t *v, int index)
{
	int layer;

	switch (index) {
	case COMMON_GRAPHIC_OBJECTS_SHOW_FRAME_BUFFER:
		set_target_frame(v->draw ^ 1);
		return true;
	case COMMON_GRAPHIC_OBJECTS_DRAW_FRAME_BUFFER:
		set_target_frame(v->draw);
		return true;
	case COMMON_GRAPHIC_OBJECTS_SCREEN_BITMAP:
		set_target_tex(&v->work);
		return true;
	case COMMON_GRAPHIC_OBJECTS_INITIAL_TEXTURE_LAYER:
#if defined(GUI)
		/* what the CPU wrote first: from here on the GS's copy counts */
		sync_ui_scratch(v);
		set_target_tex(&v->ui_scratch);
#else
		set_target_tex(&v->layers[0].tex);
#endif
		return true;
	default:
		layer = index - COMMON_GRAPHIC_OBJECTS_INITIAL_TEXTURE_LAYER;
		if (layer < 0 || layer >= v->layer_count)
			return false;
		set_target_tex(&v->layers[layer].tex);
		return true;
	}
}

static inline uint32_t gs_xy(int value)
{
	int v = (value << 4) + OFFSET;

	return v < 0 ? 0 : v >= (4096 << 4) ? (4096 << 4) - 1 : (uint32_t)v;
}

/* half a pixel up and left: the texel's centre on the pixel's */
static inline uint32_t gs_xy_half(int value)
{
	int v = (value << 4) - 8 + OFFSET;

	return v < 0 ? 0 : v >= (4096 << 4) ? (4096 << 4) - 1 : (uint32_t)v;
}

static inline uint32_t gs_uv(int value, uint32_t extent)
{
	uint32_t v = value < 0 ? 0 : (uint32_t)value << 4;
	uint32_t maximum = extent << 4;

	if (v > maximum)
		v = maximum;
	return v >= (1024u << 4) ? (1024u << 4) - 1 : v;
}

/* A sprite of src, with alpha and tests off. */
static void copy_sprite(const maxsdk_tex_t *src, int filter, uint32_t x0, uint32_t y0,
	uint32_t x1, uint32_t y1, int u0, int v0, int u1, int v1)
{
	q_reserve(12);
	ad_begin(10);
	ad(MX_GS_REG_TEST_1, mx_gs_test(0, ALPHA_TEST_EQUAL, 0, 0, 0, 0, 1, video->z_test));
	ad(MX_GS_REG_TEXFLUSH, 0);
	ad(MX_GS_REG_TEX1_1, mx_gs_tex1(0, 0, (uint32_t)filter, (uint32_t)filter, 0, 0, 0));
	ad(MX_GS_REG_TEX0_1, tex0_value(src, 0));
	ad(MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 1, 0, 0, 0, 1, 0, 0));
	ad(MX_GS_REG_RGBAQ, WHITE);
	ad(MX_GS_REG_UV, mx_gs_uv(gs_uv(u0, src->width), gs_uv(v0, src->height)));
	ad(MX_GS_REG_XYZ2, mx_gs_xyz(x0, y0, 0));
	ad(MX_GS_REG_UV, mx_gs_uv(gs_uv(u1, src->width), gs_uv(v1, src->height)));
	ad(MX_GS_REG_XYZ2, mx_gs_xyz(x1, y1, 0));
	ad_begin(1);
	ad(MX_GS_REG_TEST_1, test_value());
}

/* The same as a strip of two triangles, for what a sprite cannot turn. */
static void copy_quad(const maxsdk_tex_t *src, int filter, const RECT *dst, const int uv[8])
{
	const int x[4] = { dst->left, dst->right, dst->left, dst->right };
	const int y[4] = { dst->top, dst->top, dst->bottom, dst->bottom };
	int i;

	q_reserve(16);
	ad_begin(14);
	ad(MX_GS_REG_TEST_1, mx_gs_test(0, ALPHA_TEST_EQUAL, 0, 0, 0, 0, 1, video->z_test));
	ad(MX_GS_REG_TEXFLUSH, 0);
	ad(MX_GS_REG_TEX1_1, mx_gs_tex1(0, 0, (uint32_t)filter, (uint32_t)filter, 0, 0, 0));
	ad(MX_GS_REG_TEX0_1, tex0_value(src, 0));
	ad(MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_TRISTRIP, 0, 1, 0, 0, 0, 1, 0, 0));
	ad(MX_GS_REG_RGBAQ, WHITE);
	for (i = 0; i < 4; i++) {
		ad(MX_GS_REG_UV, mx_gs_uv(gs_uv(uv[i * 2], src->width), gs_uv(uv[i * 2 + 1], src->height)));
		ad(MX_GS_REG_XYZ2, mx_gs_xyz(gs_xy(x[i]), gs_xy(y[i]), 0));
	}
	ad_begin(1);
	ad(MX_GS_REG_TEST_1, test_value());
}

static void maxsdk_transferWorkFrame(void *data, RECT *src_rect, RECT *dst_rect)
{
	maxsdk_video_t *v = (maxsdk_video_t *)data;
	int src_width = src_rect->right - src_rect->left;
	int src_height = src_rect->bottom - src_rect->top;
	int dst_width = dst_rect->right - dst_rect->left;
	int dst_height = dst_rect->bottom - dst_rect->top;
	int filter = ps2_presentation_uses_nearest_filter(maxsdk_video_is_240p(), src_width,
		src_height, dst_width, dst_height) ? 0 : 1;

	set_target_frame(v->draw);
	copy_sprite(&v->work, filter, gs_xy_half(dst_rect->left), gs_xy_half(dst_rect->top),
		gs_xy_half(dst_rect->right), gs_xy_half(dst_rect->bottom),
		src_rect->left, src_rect->top, src_rect->right, src_rect->bottom);
}

static void maxsdk_copyRect(void *data, int src_index, int dst_index, RECT *src_rect, RECT *dst_rect)
{
	maxsdk_video_t *v = (maxsdk_video_t *)data;
	maxsdk_tex_t src;
	int sw = src_rect->right - src_rect->left;
	int dw = dst_rect->right - dst_rect->left;
	int sh = src_rect->bottom - src_rect->top;
	int dh = dst_rect->bottom - dst_rect->top;

	if (!resolve_source(v, src_index, &src) || !set_destination(v, dst_index))
		return;
	copy_sprite(&src, sw == dw && sh == dh ? 0 : 1, gs_xy(dst_rect->left), gs_xy(dst_rect->top),
		gs_xy(dst_rect->right), gs_xy(dst_rect->bottom),
		src_rect->left, src_rect->top, src_rect->right, src_rect->bottom);
}

/* The screen-flip bit: the source turned by 180 degrees. */
static void maxsdk_copyRectFlip(void *data, int src_index, int dst_index, RECT *src_rect, RECT *dst_rect)
{
	maxsdk_video_t *v = (maxsdk_video_t *)data;
	maxsdk_tex_t src;
	int sw = src_rect->right - src_rect->left;
	int dw = dst_rect->right - dst_rect->left;
	int sh = src_rect->bottom - src_rect->top;
	int dh = dst_rect->bottom - dst_rect->top;
	const int uv[8] = {
		src_rect->right, src_rect->bottom, src_rect->left, src_rect->bottom,
		src_rect->right, src_rect->top, src_rect->left, src_rect->top,
	};

	if (!resolve_source(v, src_index, &src) || !set_destination(v, dst_index))
		return;
	copy_quad(&src, sw == dw && sh == dh ? 0 : 1, dst_rect, uv);
}

/* A vertical CPS raster upright: the axes swap. */
static void maxsdk_copyRectRotate(void *data, int src_index, int dst_index, RECT *src_rect, RECT *dst_rect)
{
	maxsdk_video_t *v = (maxsdk_video_t *)data;
	maxsdk_tex_t src;
	int sw = src_rect->right - src_rect->left;
	int dw = dst_rect->right - dst_rect->left;
	int sh = src_rect->bottom - src_rect->top;
	int dh = dst_rect->bottom - dst_rect->top;
	const int uv[8] = {
		src_rect->right, src_rect->top, src_rect->right, src_rect->bottom,
		src_rect->left, src_rect->top, src_rect->left, src_rect->bottom,
	};

	if (!resolve_source(v, src_index, &src) || !set_destination(v, dst_index))
		return;
	copy_quad(&src, sw == dh && sh == dw ? 0 : 1, dst_rect, uv);
}

static void maxsdk_drawTexture(void *data, int src_index, int dst_index, RECT *src_rect, RECT *dst_rect)
{
	maxsdk_copyRect(data, src_index, dst_index, src_rect, dst_rect);
}

static int maxsdk_readFrame(void *data, int frame_index, int x, int y, int width, int height,
	uint16_t *dst, int dst_pitch)
{
	maxsdk_video_t *v = (maxsdk_video_t *)data;
	maxsdk_tex_t source;
	uint16_t *readback;
	size_t size;
	int row;
	int result;

	if (v == NULL || dst == NULL || width <= 0 || height <= 0 || dst_pitch < width)
		return 0;
	if (!resolve_source(v, frame_index, &source) || source.psm != MX_GS_PSM_CT16)
		return 0;
	if (x < 0 || y < 0 || (uint32_t)(x + width) > source.width ||
		(uint32_t)(y + height) > source.height || (source.width & 63) != 0)
		return 0;

	/* whole lines: the source's pitch */
	size = (size_t)source.width * (size_t)height * sizeof(uint16_t);
	readback = (uint16_t *)mx_dma_alloc((uint32_t)size);
	if (readback == NULL)
		return 0;
	maxsdk_flushAndWait(v);
	result = mx_gs_download(readback, MX_GS_PAGE(source.page), source.tbw, MX_GS_PSM_CT16,
		0, (uint32_t)y, source.width, (uint32_t)height) == 0;
	if (result) {
		for (row = 0; row < height; row++)
			memcpy(dst + (size_t)row * (size_t)dst_pitch,
				readback + (size_t)row * source.width + x,
				(size_t)width * sizeof(uint16_t));
	}
	mx_dma_free(readback);
	return result;
}

/* ---- atlases and palettes ------------------------------------------------- */

static void mark_dirty(texture_layer_t *layer, int y, int height)
{
	int first = y / DIRTY_BAND;
	int last = (y + height - 1) / DIRTY_BAND;
	int band;

	if (last >= DIRTY_BANDS)
		last = DIRTY_BANDS - 1;
	for (band = first; band <= last; band++)
		layer->dirty_bands |= 1ull << band;
}

static void maxsdk_commitTextureUpdates(void *data, uint8_t texture_index)
{
	maxsdk_video_t *v = (maxsdk_video_t *)data;
	texture_layer_t *layer;
	uint32_t bytes_per_pixel;
	uint64_t dirty;
	int band = 0;

	if (v == NULL || texture_index >= v->layer_count)
		return;
	layer = &v->layers[texture_index];
	bytes_per_pixel = layer->tex.psm == MX_GS_PSM_T8 ? 1 : 2;
	dirty = layer->dirty_bands;
	layer->dirty_bands = 0;
	while (dirty != 0) {
		int end;
		uint32_t y0, y1;

		while (!(dirty & (1ull << band)))
			band++;
		end = band + 1;
		while (end < DIRTY_BANDS && (dirty & (1ull << end)))
			end++;
		y0 = (uint32_t)band * DIRTY_BAND;
		y1 = (uint32_t)end * DIRTY_BAND;
		if (y1 > layer->tex.height)
			y1 = layer->tex.height;
		upload(MX_GS_PAGE(layer->tex.page), layer->tex.tbw, layer->tex.psm, y0,
			layer->tex.width, y1 - y0,
			layer->tex.mem + (size_t)y0 * layer->tex.width * bytes_per_pixel);
		while (band < end)
			dirty &= ~(1ull << band++);
	}
}

static void maxsdk_uploadClut(void *data, uint16_t *clut, uint8_t bank_index)
{
	maxsdk_video_t *v = (maxsdk_video_t *)data;
	ptrdiff_t offset;

	if (v == NULL || clut == NULL || bank_index >= v->clut_banks)
		return;
	offset = clut - v->clut_base;
	if (offset < 0 || offset >= (ptrdiff_t)v->clut_entries * v->clut_banks)
		return;
	upload(MX_GS_PAGE(v->clut_page[bank_index]), CLUT_CBW, MX_GS_PSM_CT16, 0,
		CLUT_WIDTH, v->clut_rows, clut);
}

static void maxsdk_writeIndexedTextureRect(void *data, uint8_t texture_index, int x, int y,
	int width, int height, const uint8_t *pixels, int src_pitch)
{
	maxsdk_video_t *v = (maxsdk_video_t *)data;
	maxsdk_tex_t *tex;
	int row;

	if (v == NULL || texture_index >= v->layer_count || pixels == NULL ||
		width <= 0 || height <= 0 || src_pitch < width)
		return;
	tex = &v->layers[texture_index].tex;
	if (tex->psm != MX_GS_PSM_T8 || x < 0 || y < 0 ||
		(uint32_t)(x + width) > tex->width || (uint32_t)(y + height) > tex->height)
		return;
	for (row = 0; row < height; row++)
		memcpy(tex->mem + (size_t)(y + row) * tex->width + (size_t)x,
			pixels + (size_t)row * (size_t)src_pitch, (size_t)width);
	mark_dirty(&v->layers[texture_index], y, height);
}

static void maxsdk_writeDirectTextureRect(void *data, uint8_t texture_index, int x, int y,
	int width, int height, const uint16_t *pixels, int src_pitch)
{
	maxsdk_video_t *v = (maxsdk_video_t *)data;
	maxsdk_tex_t *tex;
	uint16_t *dst;
	int row;

	if (v == NULL || texture_index >= v->layer_count || pixels == NULL ||
		width <= 0 || height <= 0 || src_pitch < width)
		return;
	tex = &v->layers[texture_index].tex;
	if (tex->psm != MX_GS_PSM_CT16 || x < 0 || y < 0 ||
		(uint32_t)(x + width) > tex->width || (uint32_t)(y + height) > tex->height)
		return;
	dst = (uint16_t *)tex->mem;
	for (row = 0; row < height; row++)
		memcpy(dst + (size_t)(y + row) * tex->width + (size_t)x,
			pixels + (size_t)row * (size_t)src_pitch, (size_t)width * sizeof(uint16_t));
	mark_dirty(&v->layers[texture_index], y, height);
}

/* ---- sprites -------------------------------------------------------------- */

static void maxsdk_blitSpriteVertices(void *data, uint8_t texture_index, const uint16_t *clut,
	uint8_t bank_index, uint32_t vertices_count, const video_sprite_vertex_t *vertices)
{
	maxsdk_video_t *v = (maxsdk_video_t *)data;
	const maxsdk_tex_t *tex;
	uint64_t tex0;
	uint32_t offset = 0;
	uint32_t u_limit, v_limit;

	if (v == NULL || vertices == NULL || vertices_count == 0 || texture_index >= v->layer_count)
		return;
	tex = &v->layers[texture_index].tex;
	tex0 = tex0_value(tex, 1);
	if (tex->psm == MX_GS_PSM_T8 && clut != NULL) {
		ptrdiff_t entry = clut - v->clut_base;
		uint32_t cov;

		if (bank_index >= v->clut_banks || entry < 0 ||
			entry >= (ptrdiff_t)v->clut_entries * v->clut_banks)
			return;
		cov = (uint32_t)entry / CLUT_WIDTH - (uint32_t)bank_index * v->clut_rows;
		if (cov != v->texclut_cov) {
			v->texclut_cov = cov;
			q_reserve(2);
			ad_begin(1);
			ad(MX_GS_REG_TEXCLUT, TEXCLUT(CLUT_CBW, 0, cov));
		}
		tex0 |= MX_SET(GS_TEX0_CBP, MX_GS_PAGE(v->clut_page[bank_index])) |
			MX_SET(GS_TEX0_CPSM, MX_GS_PSM_CT16) | MX_SET(GS_TEX0_CSM, 1) |
			MX_SET(GS_TEX0_CLD, 1);
	}

	u_limit = tex->width;
	v_limit = tex->height;
	while (offset < vertices_count) {
		uint32_t count = vertices_count - offset;
		const video_sprite_vertex_t *src = &vertices[offset];
		uint64_t *c;
		uint32_t i;

		if (count > BATCH_MAX)
			count = BATCH_MAX;
		q_reserve(count + 8);
		ad_begin(3);
		ad(MX_GS_REG_TEX0_1, tex0);
		ad(MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 1, 0, 0, 0, 1, 0, 0));
		ad(MX_GS_REG_RGBAQ, WHITE);
		cur[0] = mx_giftag(count, 1, -1, MX_GIF_FLG_REGLIST, 2);
		cur[1] = mx_gif_regs2(MX_GIF_REG_UV, MX_GIF_REG_XYZ2);
		c = cur + 2;
		for (i = 0; i < count; i++, src++, c += 2) {
			c[0] = mx_gs_uv(gs_uv(src->u, u_limit), gs_uv(src->v, v_limit));
			c[1] = mx_gs_xyz(gs_xy_half(src->x), gs_xy_half(src->y), (uint32_t)(int32_t)src->z);
		}
		cur = c;
		offset += count;
	}
}

static void maxsdk_blitPointVertices(void *data, uint32_t points_count,
	const video_point_vertex_t *vertices)
{
	maxsdk_video_t *v = (maxsdk_video_t *)data;
	uint32_t offset = 0;
	int alpha_test;

	if (v == NULL || vertices == NULL || points_count == 0)
		return;
	alpha_test = v->alpha_test;
	set_alpha_test(0);
	while (offset < points_count) {
		uint32_t count = points_count - offset;
		const video_point_vertex_t *src = &vertices[offset];
		uint64_t *c;
		uint32_t i;

		if (count > BATCH_MAX)
			count = BATCH_MAX;
		q_reserve(count + 6);
		ad_begin(1);
		ad(MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_POINT, 0, 0, 0, 0, 0, 0, 0, 0));
		cur[0] = mx_giftag(count, 1, -1, MX_GIF_FLG_REGLIST, 2);
		cur[1] = mx_gif_regs2(MX_GIF_REG_RGBAQ, MX_GIF_REG_XYZ2);
		c = cur + 2;
		for (i = 0; i < count; i++, src++, c += 2) {
			c[0] = rgbaq_of(GETR15(src->color), GETG15(src->color), GETB15(src->color), 0x80);
			c[1] = mx_gs_xyz(gs_xy_half(src->x), gs_xy_half(src->y), (uint32_t)(int32_t)src->z);
		}
		cur = c;
		offset += count;
	}
	set_alpha_test(alpha_test);
}

/* ---- depth (CPS2's priorities) ------------------------------------------- */

static void set_z(int z_test, int z_write)
{
	video->z_test = (uint8_t)z_test;
	video->z_write = (uint8_t)z_write;
	q_reserve(3);
	ad_begin(2);
	ad(MX_GS_REG_TEST_1, test_value());
	ad(MX_GS_REG_ZBUF_1, zbuf_value());
}

static void maxsdk_enableDepthTest(void *data)
{
	(void)data;
	set_z(Z_GEQUAL, 1);
}

static void maxsdk_disableDepthTest(void *data)
{
	(void)data;
	set_z(Z_ALWAYS, 0);
}

static void maxsdk_clearDepthBuffer(void *data)
{
	(void)data;
	/* with Z written, a clear sets all of Z to 0; the colour is about to be
	   drawn over, startWorkFrame having cleared it already */
	set_z(video->z_test, 1);
	clear_rect(WORK_WIDTH, WORK_HEIGHT, rgbaq_of(0, 0, 0, 0x80));
	set_z(video->z_test, 0);
}

static void maxsdk_clearColorBuffer(void *data)
{
	(void)data;
	/* Z writes are off: colour only, inside the current scissor */
	clear_rect(WORK_WIDTH, WORK_HEIGHT, rgbaq_of(0, 0, 0, 0x80));
}

/* ---- UI ------------------------------------------------------------------- */

/* The GS has 0x80 as alpha 1.0; colours are 0xAABBGGRR. */
static inline uint32_t ui_alpha(uint32_t alpha)
{
	return (alpha * 0x80 + 0x7F) / 0xFF;
}

static inline uint64_t ui_rgbaq(uint32_t color)
{
	return rgbaq_of(color & 0xFF, (color >> 8) & 0xFF, (color >> 16) & 0xFF, ui_alpha(color >> 24));
}

static uint64_t ui_lerp_rgbaq(uint32_t color1, uint32_t color2, float t)
{
	uint32_t r1 = color1 & 0xFF, g1 = (color1 >> 8) & 0xFF, b1 = (color1 >> 16) & 0xFF;
	uint32_t r2 = color2 & 0xFF, g2 = (color2 >> 8) & 0xFF, b2 = (color2 >> 16) & 0xFF;
	uint32_t a1 = color1 >> 24, a2 = color2 >> 24;

	return rgbaq_of((uint32_t)((float)r1 + ((float)r2 - (float)r1) * t),
		(uint32_t)((float)g1 + ((float)g2 - (float)g1) * t),
		(uint32_t)((float)b1 + ((float)b2 - (float)b1) * t),
		ui_alpha((uint32_t)((float)a1 + ((float)a2 - (float)a1) * t)));
}

/* Tests off for what the UI draws, then as they were. */
static void ui_begin(void)
{
	q_reserve(2);
	ad_begin(1);
	ad(MX_GS_REG_TEST_1, mx_gs_test(0, ALPHA_TEST_EQUAL, 0, 0, 0, 0, 1, video->z_test));
}

static void ui_end(void)
{
	q_reserve(2);
	ad_begin(1);
	ad(MX_GS_REG_TEST_1, test_value());
}

static void maxsdk_drawUISprite(void *data, void *tex, int tex_format, int tex_swizzled,
	int tex_width, int tex_height, int tex_stride, int su, int sv, int sw, int sh,
	int dx, int dy, int dw, int dh, int blend)
{
	const maxsdk_tex_t *texture = (const maxsdk_tex_t *)tex;

	(void)data;
	(void)tex_format;
	(void)tex_swizzled;
	(void)tex_width;
	(void)tex_height;
	(void)tex_stride;
	if (texture == NULL || texture->pages == 0)
		return;
	ui_begin();
	q_reserve(10);
	ad_begin(8);
	ad(MX_GS_REG_TEX1_1, mx_gs_tex1(0, 0, 0, 0, 0, 0, 0));
	ad(MX_GS_REG_TEX0_1, tex0_value(texture, blend != 0));
	ad(MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 1, 0, blend != 0, 0, 1, 0, 0));
	ad(MX_GS_REG_RGBAQ, WHITE);
	ad(MX_GS_REG_UV, mx_gs_uv(gs_uv(su, texture->width), gs_uv(sv, texture->height)));
	ad(MX_GS_REG_XYZ2, mx_gs_xyz(gs_xy_half(dx), gs_xy_half(dy), 0));
	ad(MX_GS_REG_UV, mx_gs_uv(gs_uv(su + sw, texture->width), gs_uv(sv + sh, texture->height)));
	ad(MX_GS_REG_XYZ2, mx_gs_xyz(gs_xy_half(dx + dw), gs_xy_half(dy + dh), 0));
	ui_end();
}

static void maxsdk_drawUILine(void *data, int x1, int y1, int x2, int y2, uint32_t color)
{
	(void)data;
	ui_begin();
	q_reserve(6);
	ad_begin(4);
	ad(MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_LINE, 0, 0, 0, (color >> 24) != 0xFF, 0, 0, 0, 0));
	ad(MX_GS_REG_RGBAQ, ui_rgbaq(color));
	ad(MX_GS_REG_XYZ2, mx_gs_xyz(gs_xy(x1), gs_xy(y1), 0));
	ad(MX_GS_REG_XYZ2, mx_gs_xyz(gs_xy(x2), gs_xy(y2), 0));
	ui_end();
}

static void maxsdk_drawUILineGradient(void *data, int x1, int y1, int x2, int y2,
	uint32_t color1, uint32_t color2)
{
	int dx = x2 - x1;
	int dy = y2 - y1;
	int adx = dx < 0 ? -dx : dx;
	int ady = dy < 0 ? -dy : dy;
	int steps = adx > ady ? adx : ady;
	uint64_t *c;
	int i;

	(void)data;
	ui_begin();
	q_reserve((uint32_t)steps + 8);
	ad_begin(1);
	ad(MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_POINT, 0, 0, 0, 1, 0, 0, 0, 0));
	cur[0] = mx_giftag((uint32_t)steps + 1, 1, -1, MX_GIF_FLG_REGLIST, 2);
	cur[1] = mx_gif_regs2(MX_GIF_REG_RGBAQ, MX_GIF_REG_XYZ2);
	c = cur + 2;
	for (i = 0; i <= steps; i++, c += 2) {
		float t = steps != 0 ? (float)i / (float)steps : 0.0f;

		c[0] = ui_lerp_rgbaq(color1, color2, t);
		c[1] = mx_gs_xyz(gs_xy(x1 + (int)((float)dx * t)), gs_xy(y1 + (int)((float)dy * t)), 0);
	}
	cur = c;
	ui_end();
}

static void maxsdk_drawUIRect(void *data, int x, int y, int w, int h, uint32_t color)
{
	uint64_t rgbaq = ui_rgbaq(color);
	int ex = x + w;
	int ey = y + h;

	(void)data;
	ui_begin();
	q_reserve(12);
	ad_begin(10);
	ad(MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_LINE, 0, 0, 0, (color >> 24) != 0xFF, 0, 0, 0, 0));
	ad(MX_GS_REG_RGBAQ, rgbaq);
	ad(MX_GS_REG_XYZ2, mx_gs_xyz(gs_xy(x), gs_xy(y), 0));
	ad(MX_GS_REG_XYZ2, mx_gs_xyz(gs_xy(ex), gs_xy(y), 0));
	ad(MX_GS_REG_XYZ2, mx_gs_xyz(gs_xy(ex), gs_xy(y), 0));
	ad(MX_GS_REG_XYZ2, mx_gs_xyz(gs_xy(ex), gs_xy(ey), 0));
	ad(MX_GS_REG_XYZ2, mx_gs_xyz(gs_xy(ex), gs_xy(ey), 0));
	ad(MX_GS_REG_XYZ2, mx_gs_xyz(gs_xy(x), gs_xy(ey), 0));
	ad(MX_GS_REG_XYZ2, mx_gs_xyz(gs_xy(x), gs_xy(ey), 0));
	ad(MX_GS_REG_XYZ2, mx_gs_xyz(gs_xy(x), gs_xy(y), 0));
	ui_end();
}

static void maxsdk_fillUIRect(void *data, int x, int y, int w, int h, uint32_t color)
{
	(void)data;
	ui_begin();
	q_reserve(6);
	ad_begin(4);
	ad(MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 0, 0, (color >> 24) != 0xFF, 0, 0, 0, 0));
	ad(MX_GS_REG_RGBAQ, ui_rgbaq(color));
	ad(MX_GS_REG_XYZ2, mx_gs_xyz(gs_xy(x), gs_xy(y), 0));
	ad(MX_GS_REG_XYZ2, mx_gs_xyz(gs_xy(x + w), gs_xy(y + h), 0));
	ui_end();
}

static void maxsdk_fillUIRectGradient(void *data, int x, int y, int w, int h,
	uint32_t color1, uint32_t color2, int direction)
{
	int lines = direction == 0 ? w : h;
	uint64_t *c;
	int i;

	if (w <= 0 || h <= 0)
		return;
	if (lines == 1) {
		maxsdk_fillUIRect(data, x, y, w, h, color1);
		return;
	}
	ui_begin();
	q_reserve((uint32_t)lines * 2 + 8);
	ad_begin(1);
	ad(MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_LINE, 0, 0, 0, 1, 0, 0, 0, 0));
	cur[0] = mx_giftag((uint32_t)lines * 2, 1, -1, MX_GIF_FLG_REGLIST, 2);
	cur[1] = mx_gif_regs2(MX_GIF_REG_RGBAQ, MX_GIF_REG_XYZ2);
	c = cur + 2;
	for (i = 0; i < lines; i++, c += 4) {
		uint64_t rgbaq = ui_lerp_rgbaq(color1, color2, (float)i / (float)(lines - 1));

		c[0] = rgbaq;
		c[2] = rgbaq;
		if (direction == 0) {
			c[1] = mx_gs_xyz(gs_xy(x + i), gs_xy(y), 0);
			c[3] = mx_gs_xyz(gs_xy(x + i), gs_xy(y + h), 0);
		} else {
			c[1] = mx_gs_xyz(gs_xy(x), gs_xy(y + i), 0);
			c[3] = mx_gs_xyz(gs_xy(x + w), gs_xy(y + i), 0);
		}
	}
	cur = c;
	ui_end();
}

static void maxsdk_setUIScissor(void *data, int x, int y, int w, int h)
{
	maxsdk_video_t *v = (maxsdk_video_t *)data;
	int left, top, right, bottom;

	if (v == NULL || w <= 0 || h <= 0)
		return;
	left = x < 0 ? 0 : x;
	top = y < 0 ? 0 : y;
	right = x + w - 1;
	bottom = y + h - 1;
	if (right >= v->mode.width)
		right = v->mode.width - 1;
	if (bottom >= v->mode.height)
		bottom = v->mode.height - 1;
	if (left <= right && top <= bottom)
		set_scissor((uint32_t)left, (uint32_t)right, (uint32_t)top, (uint32_t)bottom);
}

video_driver_t video_maxsdk = {
	"maxsdk",
	maxsdk_init,
	maxsdk_free,
	maxsdk_waitVsync,
	maxsdk_flipScreen,
	maxsdk_beginFrame,
	maxsdk_endFrame,
	maxsdk_frameAddr,
	maxsdk_readFrame,
	maxsdk_getOutputSize,
	maxsdk_scissor,
	maxsdk_clearScreen,
	maxsdk_clearFrame,
	maxsdk_fillFrame,
	maxsdk_startWorkFrame,
	maxsdk_transferWorkFrame,
	maxsdk_copyRect,
	maxsdk_copyRectFlip,
	maxsdk_copyRectRotate,
	maxsdk_drawTexture,
	maxsdk_commitTextureUpdates,
	maxsdk_uploadClut,
	maxsdk_writeIndexedTextureRect,
	maxsdk_writeDirectTextureRect,
	maxsdk_blitSpriteVertices,
	maxsdk_blitPointVertices,
	maxsdk_enableDepthTest,
	maxsdk_disableDepthTest,
	maxsdk_clearDepthBuffer,
	maxsdk_clearColorBuffer,
	maxsdk_drawUISprite,
	maxsdk_drawUILine,
	maxsdk_drawUILineGradient,
	maxsdk_drawUIRect,
	maxsdk_fillUIRect,
	maxsdk_fillUIRectGradient,
	maxsdk_setUIScissor,
	NULL,
	maxsdk_getPresentationViewport,
	maxsdk_setOutputOffset,
	maxsdk_setOutputMode,
	maxsdk_flushAndWait,
	maxsdk_getNativeContext,
};
