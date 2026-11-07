/* Runs without a player (make FRAMES=n PLAY=... SHOTS=...): scripted pad input
   through the real menus, pictures of the screen at given flips, and at flip n
   the end of the program with the test runners' end marker. */

#ifdef MAXSDK_TEST

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <mx/cpu.h>
#include <mx/gs.h>
#include <mx/mx.h>
#include <mx/sif.h>

#include "common/input_state.h"
#include "maxsdk/maxsdk_test.h"

/* button bits held from a flip on, for a few flips */
static const struct { uint32_t flip, buttons; } play[] = { MAXSDK_TEST_PLAY { 0, 0 } };
static const uint32_t shots[] = { MAXSDK_TEST_SHOTS 0 };
#define HOLD 6

static uint32_t flips;

uint32_t maxsdk_test_buttons(void)
{
	uint32_t buttons = 0;
	int i;

	for (i = 0; play[i].flip != 0; i++)
		if (flips >= play[i].flip && flips < play[i].flip + HOLD)
			buttons |= play[i].buttons;
	return buttons;
}

/* a CT16 buffer as a 24-bit TGA */
static void shot(const struct mx_rt *rt)
{
	static uint16_t line[1024] MX_DMA_LINES;
	static uint8_t out[1024 * 3] MX_DMA_LINES;
	uint8_t header[18] = { 0 };
	char name[32];
	uint32_t y, x;
	int fd;

	snprintf(name, sizeof(name), "shot%u.tga", (unsigned)flips);
	fd = mx_fio_open(name, MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
	if (fd < 0)
		return;
	header[2] = 2;
	header[12] = (uint8_t)rt->w;
	header[13] = (uint8_t)(rt->w >> 8);
	header[14] = (uint8_t)rt->h;
	header[15] = (uint8_t)(rt->h >> 8);
	header[16] = 24;
	header[17] = 0x20; /* top line first */
	mx_fio_write(fd, header, sizeof(header));
	for (y = 0; y < rt->h; y++) {
		if (mx_gs_download(line, MX_GS_PAGE(rt->fbp), rt->fbw, MX_GS_PSM_CT16, 0, y, rt->w, 1) < 0)
			break;
		for (x = 0; x < rt->w; x++) {
			uint32_t c = line[x];

			out[x * 3 + 0] = (uint8_t)((c >> 10 & 31) << 3);
			out[x * 3 + 1] = (uint8_t)((c >> 5 & 31) << 3);
			out[x * 3 + 2] = (uint8_t)((c & 31) << 3);
		}
		mx_fio_write(fd, out, rt->w * 3);
	}
	mx_fio_close(fd);
	printf("test: %s\n", name);
}

void maxsdk_test_flip(const struct mx_rt *shown)
{
	int i;

	flips++;
	for (i = 0; shots[i] != 0; i++)
		if (shots[i] == flips)
			shot(shown);
	if (flips == MAXSDK_TEST) {
		MX_TEST_END(0);
		fflush(stdout);
		_exit(0);
	}
}

#endif /* MAXSDK_TEST */
