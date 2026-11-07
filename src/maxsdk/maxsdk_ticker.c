#include <stdlib.h>

#include "common/ticker_driver.h"
#include "maxsdk/maxsdk_time.h"

static void *maxsdk_init(void)
{
	static int ticker;

	maxsdk_time_init();
	return &ticker;
}

static void maxsdk_free(void *data)
{
	(void)data;
}

static uint64_t maxsdk_currentUs(void *data)
{
	(void)data;
	return maxsdk_time_us();
}

ticker_driver_t ticker_maxsdk = {
	"maxsdk",
	maxsdk_init,
	maxsdk_free,
	maxsdk_currentUs,
};
