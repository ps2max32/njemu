#ifndef NJEMU_MAXSDK_TIME_H
#define NJEMU_MAXSDK_TIME_H

#include <stdint.h>

/* Start and stop the timer interrupt behind usleep() and the clock. */
void maxsdk_time_init(void);
void maxsdk_time_exit(void);

/* Microseconds since the start of the program; never goes back. */
uint64_t maxsdk_time_us(void);

#endif /* NJEMU_MAXSDK_TIME_H */
