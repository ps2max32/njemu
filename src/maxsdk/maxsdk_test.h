#ifndef NJEMU_MAXSDK_TEST_H
#define NJEMU_MAXSDK_TEST_H

#include <stdint.h>

struct mx_rt;

uint32_t maxsdk_test_buttons(void);
void maxsdk_test_flip(const struct mx_rt *shown);

#endif /* NJEMU_MAXSDK_TEST_H */
