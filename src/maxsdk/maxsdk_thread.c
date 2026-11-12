/* Threads on <mx/thread.h>. A thread's stack is pinned: never VM. suspendThread
   and resumeThread have no caller in the common code; maxsdk has no call for
   them. */

#include <stdlib.h>

#include <mx/mem.h>
#include <mx/thread.h>

#include "common/thread_driver.h"

typedef struct maxsdk_thread {
	int id;
	int running;
	uint32_t priority;
	uint32_t stack_size;
	void *stack;
	int32_t (*func)(uint32_t, void *);
} maxsdk_thread_t;

static void child(void *arg)
{
	maxsdk_thread_t *t = (maxsdk_thread_t *)arg;

	t->func(0, NULL);
}

static void join(maxsdk_thread_t *t)
{
	if (t->running) {
		mx_thread_join(t->id);
		t->running = 0;
		t->id = -1;
	}
}

static void *maxsdk_init(void)
{
	maxsdk_thread_t *t = (maxsdk_thread_t *)calloc(1, sizeof(*t));

	if (t != NULL)
		t->id = -1;
	return t;
}

static void cleanup(maxsdk_thread_t *t)
{
	join(t);
	if (t->stack != NULL)
		mx_dma_free(t->stack);
	t->stack = NULL;
	t->func = NULL;
}

static void maxsdk_free(void *data)
{
	maxsdk_thread_t *t = (maxsdk_thread_t *)data;

	if (t == NULL)
		return;
	cleanup(t);
	free(t);
}

static bool maxsdk_createThread(void *data, const char *name,
	int32_t (*func)(uint32_t, void *), uint32_t priority, uint32_t stack_size)
{
	maxsdk_thread_t *t = (maxsdk_thread_t *)data;

	(void)name;
	if (t == NULL || func == NULL || stack_size == 0)
		return false;
	t->stack = mx_dma_alloc(stack_size);
	if (t->stack == NULL)
		return false;
	t->func = func;
	t->priority = priority;
	t->stack_size = stack_size;
	return true;
}

static void maxsdk_startThread(void *data)
{
	maxsdk_thread_t *t = (maxsdk_thread_t *)data;

	if (t == NULL || t->func == NULL || t->running)
		return;
	t->id = mx_thread_start(child, t, t->stack, t->stack_size, (int)t->priority);
	t->running = t->id >= 0;
}

static void maxsdk_waitThreadEnd(void *data)
{
	if (data != NULL)
		join((maxsdk_thread_t *)data);
}

static void maxsdk_wakeupThread(void *data)
{
	maxsdk_thread_t *t = (maxsdk_thread_t *)data;

	if (t != NULL && t->running)
		mx_thread_wakeup(t->id);
}

static void maxsdk_deleteThread(void *data)
{
	if (data != NULL)
		cleanup((maxsdk_thread_t *)data);
}

static void maxsdk_resumeThread(void *data)
{
	(void)data;
}

static void maxsdk_suspendThread(void *data)
{
	(void)data;
}

static void maxsdk_sleepThread(void *data)
{
	(void)data;
	mx_thread_sleep();
}

static void maxsdk_yieldThread(void)
{
	int self = mx_thread_self();
	int priority = mx_thread_prio(self);

	/* The EE kernel does not time-slice. Sound runs one priority below the
	   emulation: join its queue and rotate it, then come back. */
	if (mx_thread_set_prio(self, priority + 1) < 0)
		return;
	mx_thread_rotate(priority + 1);
	mx_thread_set_prio(self, priority);
}

static void maxsdk_exitThread(void *data, int32_t exit_code)
{
	/* the thread ends by returning from its function, in child() */
	(void)data;
	(void)exit_code;
}

thread_driver_t thread_maxsdk = {
	"maxsdk",
	maxsdk_init,
	maxsdk_free,
	maxsdk_createThread,
	maxsdk_startThread,
	maxsdk_waitThreadEnd,
	maxsdk_wakeupThread,
	maxsdk_deleteThread,
	maxsdk_resumeThread,
	maxsdk_suspendThread,
	maxsdk_sleepThread,
	maxsdk_yieldThread,
	maxsdk_exitThread,
};
