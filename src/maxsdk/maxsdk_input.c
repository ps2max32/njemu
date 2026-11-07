/* The pads on <mx/pad.h>: two ports, no multitap. The IOP writes each port's
   state into EE memory, so a sample is a memory read. Select + Start + L1 + R1
   is maxsdk's: it ends the program from any screen. */

#include <stdlib.h>

#include <mx/pad.h>

#include "common/input_driver.h"
#include "maxsdk/maxsdk_test.h"

#define MAX_PORTS 2

static int port_of(uint32_t controller)
{
	uint32_t index = 0;
	int port;

	for (port = 0; port < MAX_PORTS; port++) {
		if (mx_pad_connected(port)) {
			if (index == controller)
				return port;
			index++;
		}
	}
	return -1;
}

static uint32_t decode_buttons(uint32_t pad)
{
	uint32_t buttons = 0;

	buttons |= (pad & MX_PAD_UP) ? PLATFORM_PAD_UP : 0;
	buttons |= (pad & MX_PAD_DOWN) ? PLATFORM_PAD_DOWN : 0;
	buttons |= (pad & MX_PAD_LEFT) ? PLATFORM_PAD_LEFT : 0;
	buttons |= (pad & MX_PAD_RIGHT) ? PLATFORM_PAD_RIGHT : 0;
	buttons |= (pad & MX_PAD_CIRCLE) ? PLATFORM_PAD_B1 : 0;
	buttons |= (pad & MX_PAD_CROSS) ? PLATFORM_PAD_B2 : 0;
	buttons |= (pad & MX_PAD_SQUARE) ? PLATFORM_PAD_B3 : 0;
	buttons |= (pad & MX_PAD_TRIANGLE) ? PLATFORM_PAD_B4 : 0;
	buttons |= (pad & MX_PAD_L1) ? PLATFORM_PAD_L : 0;
	buttons |= (pad & MX_PAD_R1) ? PLATFORM_PAD_R : 0;
	buttons |= (pad & MX_PAD_START) ? PLATFORM_PAD_START : 0;
	buttons |= (pad & MX_PAD_SELECT) ? PLATFORM_PAD_SELECT : 0;
	return buttons;
}

static void *maxsdk_init(void)
{
	static int input;

	return mx_pad_init() < 0 ? NULL : &input;
}

static void maxsdk_free(void *data)
{
	(void)data;
}

static uint32_t maxsdk_controllerCount(void *data)
{
	uint32_t count = 0;
	int port;

	(void)data;
	for (port = 0; port < MAX_PORTS; port++)
		count += mx_pad_connected(port) ? 1 : 0;
#ifdef MAXSDK_TEST
	if (count == 0)
		count = 1; /* the script's */
#endif
	return count;
}

static bool maxsdk_sample(void *data, uint32_t controller, input_state_t *state)
{
	uint8_t sticks[4];
	int port = port_of(controller);

	(void)data;
#ifdef MAXSDK_TEST
	if (state != NULL && controller == 0) {
		input_state_reset(state);
		state->buttons = maxsdk_test_buttons();
		if (port >= 0)
			state->buttons |= decode_buttons(mx_pad_buttons(port));
		return true;
	}
#endif
	if (port < 0 || state == NULL)
		return false;
	input_state_reset(state);
	state->buttons = decode_buttons(mx_pad_buttons(port));
	if (mx_pad_sticks(port, sticks)) {
		state->lx = sticks[2];
		state->ly = sticks[3];
		state->axis_flags = INPUT_AXIS_LX | INPUT_AXIS_LY;
	}
	return true;
}

input_driver_t input_maxsdk = {
	"maxsdk",
	maxsdk_init,
	maxsdk_free,
	NULL,
	maxsdk_controllerCount,
	maxsdk_sample,
	NULL,
	NULL,
};
