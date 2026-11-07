/* The program on mx-kernel: the IOP, its modules and the file systems are the
   kernel's, so there is nothing to reset or load here (no IOP work, no device
   names: libs/maxsdk/docs/mx-os.md). */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mx/bios.h>
#include <mx/mx.h>
#include <mx/snd.h>
#include <mx/thread.h>
#include <mx/vm.h>

#include "emucfg.h"
#include "common/memory_sizes.h"
#include "common/platform_driver.h"
#include "common/runtime_paths.h"
#include "maxsdk/maxsdk_time.h"

#if NJEMU_SOUND_THREAD_PRIORITY <= NJEMU_MAIN_THREAD_PRIORITY
#error "The sound thread must run below the main emulation thread"
#endif

#ifdef MAXSDK_VM
/* A large game lives in VM: what comes from a ROM zip goes through the swap
   file (the writer keeps frames clean ahead of it), and processed sprite and
   sample data is mapped from its files: the largest Neo Geo sets have 64 MiB of
   sprites and 16 MiB of samples. */
const struct mx_vm_config mx_vm_config = {
	.swap = "swap.bin",
	.map_size = 96u << 20,
	.clean_frames = 32,
};
#endif

static void *maxsdk_init(void)
{
	static int platform;

	maxsdk_time_init();
	if (mx_snd_init() < 0) {
		printf("No sound: the IOP's audio modules did not load\n");
		return NULL;
	}
	return &platform;
}

static void maxsdk_free(void *data)
{
	(void)data;
	maxsdk_time_exit();
}

static void maxsdk_main(void *data, int argc, char *argv[])
{
	char picture_dir[PATH_MAX];

	(void)data;
	(void)argc;
	(void)argv;

	if (snprintf(picture_dir, sizeof(picture_dir), "%sPICTURE", launchDir) >=
		(int)sizeof(picture_dir)) {
		screenshotDir[0] = '\0';
		return;
	}
	mkdir(picture_dir, 0777);
	if (snprintf(screenshotDir, sizeof(screenshotDir), "%s/%s", picture_dir,
		SYSTEM_NAME) >= (int)sizeof(screenshotDir))
		screenshotDir[0] = '\0';
}

static uint64_t largest_block(uint64_t limit)
{
	uint32_t low = 0;
	uint32_t high = (uint32_t)(limit / CACHE_BLOCK_SIZE);

	while (low < high) {
		uint32_t mid = low + (high - low + 1u) / 2u;
		void *probe = malloc((size_t)mid * CACHE_BLOCK_SIZE);

		if (probe != NULL) {
			free(probe);
			low = mid;
		} else {
			high = mid - 1u;
		}
	}
	return (uint64_t)low * CACHE_BLOCK_SIZE;
}

static bool maxsdk_queryMemoryInfo(void *data, platform_memory_info_t *out)
{
	const uint32_t baseline_reservation = 4u * 1024u * 1024u;
	uint32_t total = mx_vm_active() ? mx_vm_size() : (uint32_t)mx_bios_get_memory_size();
	uint64_t policy_cap = total > baseline_reservation ? total - baseline_reservation : 0;
	uint64_t largest;

	(void)data;
	if (out == NULL)
		return false;
	memset(out, 0, sizeof(*out));
	largest = largest_block(policy_cap);
	out->physical_total_bytes = total;
	out->budget_cap_bytes = policy_cap;
	/* no query of the free heap: the largest block is the estimate of both */
	out->free_bytes = largest;
	out->largest_free_block_bytes = largest;
	out->capabilities = PLATFORM_MEMORY_CAP_QUERY_FREE | PLATFORM_MEMORY_CAP_QUERY_LARGEST_BLOCK;
	out->reliability_flags = PLATFORM_MEMORY_FREE_IS_ESTIMATE | PLATFORM_MEMORY_LARGEST_IS_PROBED;
	platform_memory_info_normalize(out);
	return total != 0;
}

static ui_language_t maxsdk_getSystemLanguage(void *data)
{
	/* maxsdk does not read the console's settings; the menu can change it */
	(void)data;
	return UI_LANG_ENGLISH;
}

#ifdef MAXSDK_TEST
/* A run without a player: status 1 also when the program gives up by itself,
   its data in a directory of its own beside the ELF (TESTDIR). */
static void test_end(void)
{
	MX_TEST_END(1);
	fflush(stdout);
	mx_exit(1);
}

static void maxsdk_prepareLaunch(int argc, char *argv[])
{
	(void)argc;
	(void)argv;
	atexit(test_end);
#ifdef MAXSDK_TEST_DIR
	if (chdir(MAXSDK_TEST_DIR) < 0)
		printf("test: no directory " MAXSDK_TEST_DIR "\n");
#endif
}
#else
#define maxsdk_prepareLaunch NULL
#endif

platform_driver_t platform_maxsdk = {
	"maxsdk",
	maxsdk_init,
	maxsdk_free,
	maxsdk_main,
	maxsdk_queryMemoryInfo,
	maxsdk_getSystemLanguage,
	maxsdk_prepareLaunch,
};
