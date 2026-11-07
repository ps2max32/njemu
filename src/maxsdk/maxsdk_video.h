#ifndef NJEMU_MAXSDK_VIDEO_H
#define NJEMU_MAXSDK_VIDEO_H

/* What the UI texture adapter shares with the video backend. */

#include <stdbool.h>
#include <stdint.h>

/* A texture in GS memory; mem is its copy in EE memory, if it has one. */
typedef struct maxsdk_tex {
	uint32_t page;   /* first 8 KiB page */
	uint32_t pages;
	uint32_t tbw;    /* buffer width, in 64 pixels */
	uint32_t psm;
	uint32_t width, height;
	uint8_t tw, th;  /* log2 of the size, rounded up */
	uint8_t *mem;
} maxsdk_tex_t;

/* GS memory for a texture of w x h pixels: false when there is none left. */
bool maxsdk_tex_alloc(maxsdk_tex_t *tex, int width, int height, uint32_t psm);
void maxsdk_tex_free(maxsdk_tex_t *tex);

/* Queue the transfer of w x h pixels at pixels (16-byte aligned; stays as it
   is until the GS has it, video_driver->flushAndWait) to the top left of tex. */
void maxsdk_tex_upload(const maxsdk_tex_t *tex, const void *pixels, int width, int height);

/* NTSC timing: the output has overscan, unlike 480p. */
bool maxsdk_video_is_ntsc(void);
/* NTSC timing without interlace. */
bool maxsdk_video_is_240p(void);

#endif /* NJEMU_MAXSDK_VIDEO_H */
