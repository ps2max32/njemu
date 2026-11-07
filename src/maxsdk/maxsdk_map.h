#ifndef NJEMU_MAXSDK_MAP_H
#define NJEMU_MAXSDK_MAP_H

#include <stddef.h>
#include <stdint.h>

/* A file of at least size bytes as read-only memory, or NULL. */
const uint8_t *maxsdk_map_file(const char *path, size_t size);
void maxsdk_unmap_file(const uint8_t *p);

#endif /* NJEMU_MAXSDK_MAP_H */
