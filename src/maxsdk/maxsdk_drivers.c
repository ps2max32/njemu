#include <stddef.h>

#include "common/audio_driver.h"
#include "common/cache_storage_driver.h"
#include "common/input_driver.h"
#include "common/platform_driver.h"
#include "common/power_driver.h"
#include "common/thread_driver.h"
#include "common/ticker_driver.h"
#include "common/ui_draw_driver.h"
#include "common/video_driver.h"

extern audio_driver_t audio_maxsdk;
extern input_driver_t input_maxsdk;
extern platform_driver_t platform_maxsdk;
extern thread_driver_t thread_maxsdk;
extern ticker_driver_t ticker_maxsdk;
extern video_driver_t video_maxsdk;
extern const ui_draw_driver_t maxsdk_ui_draw_driver;

/* no accelerated cache reads: a negative handle takes the POSIX path */
static int cache_open(const char *path)
{
	(void)path;
	return -1;
}

static int cache_read_at(int handle, uint64_t offset, void *buffer, size_t size)
{
	(void)handle;
	(void)offset;
	(void)buffer;
	(void)size;
	return -1;
}

static void cache_close(int handle)
{
	(void)handle;
}

static const cache_storage_driver_t cache_storage_maxsdk = {
	cache_open,
	cache_read_at,
	cache_close,
	NULL,
};

audio_driver_t *const audio_driver = &audio_maxsdk;
const cache_storage_driver_t *const cache_storage_driver = &cache_storage_maxsdk;
input_driver_t *const input_driver = &input_maxsdk;
platform_driver_t *const platform_driver = &platform_maxsdk;
const power_driver_t *const power_driver = &power_unsupported;
thread_driver_t *const thread_driver = &thread_maxsdk;
ticker_driver_t *const ticker_driver = &ticker_maxsdk;
video_driver_t *video_driver = &video_maxsdk;

int video_backend_choice_count(void) { return 1; }
int video_filter_count(void) { return 1; }
video_backend_choice_t video_backend_choice_at(int index)
{
	return index == 0 ? (video_backend_choice_t){ 1, "PS2 GS" }
	                  : (video_backend_choice_t){ -1, "" };
}
int video_backend_option_available(int id) { return id == 0 || id == 1; }
void video_backend_select(int id) { (void)id; video_driver = &video_maxsdk; }

const ui_draw_driver_t *const ui_draw_driver = &maxsdk_ui_draw_driver;
