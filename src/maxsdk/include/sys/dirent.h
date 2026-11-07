/* sys/dirent.h for picolibc's <dirent.h>, which has none for this target:
   directories of the IOP file server (src/maxsdk/maxsdk_posix.c, on mx_fio_dopen). */
#ifndef NJEMU_SYS_DIRENT_H
#define NJEMU_SYS_DIRENT_H

#include <sys/types.h>

#define MAXNAMLEN 255

#define DT_UNKNOWN 0
#define DT_DIR 4
#define DT_REG 8

struct dirent {
  ino_t d_ino;
  unsigned char d_type;
  char d_name[MAXNAMLEN + 1];
};

typedef struct njemu_dir DIR;

#endif
