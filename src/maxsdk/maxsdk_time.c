/* Time for the maxsdk backend: a 64-bit clock and sleeping.

   Waiting must sleep, never spin: the sound thread runs below the main thread,
   so a main thread that spins starves the audio. The EE kernel has no sleep of
   its own that maxsdk wraps, so a timer interrupt at 1 kHz wakes sleepers. The
   same interrupt extends COP0 Count, which wraps every 14.5 s, to 64 bits.

   The handler and what it reads are core: with VM, nothing here may fault. */

#include <errno.h>
#include <unistd.h>

#include <mx/cpu.h>
#include <mx/thread.h>
#include <mx/timer.h>

#include "maxsdk/maxsdk_time.h"

#define TICK_PERIOD (MX_BUS_HZ / 256u / 1000u) /* timer clocks a millisecond */
#define SLEEPERS 8

static struct {
	int sema;  /* created at the first use of the slot, kept */
	int used;
	uint64_t until;
} sleepers[SLEEPERS] MX_CORE_DATA;

static volatile uint32_t count_high MX_CORE_DATA;
static volatile uint32_t count_last MX_CORE_DATA;
static int started;

MX_CORE_FN static uint64_t cycles_locked(void)
{
	uint32_t count = mx_count();

	if (count < count_last)
		count_high++;
	count_last = count;
	return (uint64_t)count_high << 32 | count;
}

MX_CORE_FN static void tick(void *arg)
{
	uint64_t now = cycles_locked();
	int i;

	(void)arg;
	for (i = 0; i < SLEEPERS; i++) {
		if (sleepers[i].used && now >= sleepers[i].until) {
			sleepers[i].used = 0;
			mx_isema_signal(sleepers[i].sema);
		}
	}
}

void maxsdk_time_init(void)
{
	int i;

	if (started)
		return;
	started = 1;
	for (i = 0; i < SLEEPERS; i++)
		sleepers[i].sema = -1;
	mx_timer_periodic(0, TICK_PERIOD, tick, 0);
}

void maxsdk_time_exit(void)
{
	if (started)
		mx_timer_periodic(0, 0, 0, 0);
	started = 0;
}

uint64_t maxsdk_time_us(void)
{
	int was = mx_di();
	uint64_t cycles = cycles_locked();

	mx_irq_restore(was);
	return cycles / (MX_EE_HZ / 1000000u);
}

int usleep(useconds_t us)
{
	uint64_t until;
	int slot = -1;
	int was;
	int i;

	maxsdk_time_init();
	was = mx_di();
	until = cycles_locked() + (uint64_t)us * (MX_EE_HZ / 1000000u);
	for (i = 0; i < SLEEPERS; i++) {
		if (!sleepers[i].used) {
			slot = i;
			sleepers[i].used = 1;
			sleepers[i].until = ~0ull; /* not due until set below */
			break;
		}
	}
	mx_irq_restore(was);
	if (slot < 0) {
		/* more sleepers than slots: let the others run instead */
		mx_thread_yield();
		return 0;
	}
	if (sleepers[slot].sema < 0)
		sleepers[slot].sema = mx_sema_create(0, 1);
	was = mx_di();
	sleepers[slot].until = until;
	mx_irq_restore(was);
	mx_sema_wait(sleepers[slot].sema);
	return 0;
}
