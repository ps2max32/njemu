/* The POSIX calls under picolibc's stdio, on the IOP file server (<mx/sif.h>).

   The paths are Unix ones: "/" is the directory of the ELF, so "/d3/d3.hog"
   is "d3/d3.hog" there; a relative path is taken from the
   current directory (chdir, "/" at the start). Descriptors 0, 1 and 2 are
   the console (maxsdk's glue: mx_glue_write); the files are 3 and up.

   The server has no stat: a path is a directory if it opens as one, else
   a file if it opens (its size by lseek). It has no rename: a copy. */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <limits.h>
#include <unistd.h>

#include <mx/sif.h>

ssize_t mx_glue_write(int fd, const void *buf, size_t n);

#define FIO_RDWR 0x0003
#define FIO_APPEND 0x0100
#define NFILES 32
#define FD0 3
#define IO_PIECE (256u * 1024u)

/* ---- paths ------------------------------------------------------------------- */

static char cwd[PATH_MAX] = "/";

/* path (absolute or relative) as an absolute one with "." and ".." gone,
   in out (PATH_MAX); returns out */
static char *normalize(const char *path, char *out) {
  char tmp[PATH_MAX * 2];
  if (path[0] == '/')
    snprintf(tmp, sizeof tmp, "%s", path);
  else
    snprintf(tmp, sizeof tmp, "%s/%s", cwd, path);
  char *o = out;
  *o++ = '/';
  for (char *p = tmp; *p;) {
    while (*p == '/')
      p++;
    char *e = p;
    while (*e && *e != '/')
      e++;
    size_t n = (size_t)(e - p);
    if (n == 0 || (n == 1 && p[0] == '.')) {
    } else if (n == 2 && p[0] == '.' && p[1] == '.') {
      if (o > out + 1) {
        o--;
        while (o > out + 1 && o[-1] != '/')
          o--;
      }
    } else if (o + n + 1 < out + PATH_MAX) {
      memcpy(o, p, n);
      o += n;
      *o++ = '/';
    }
    p = e;
  }
  if (o > out + 1)
    o--;
  *o = 0;
  return out;
}

/* the file server's name of path: the absolute path without its first '/',
   so in the directory of the ELF */
static const char *host_path(const char *path, char *out) {
  char abs[PATH_MAX];
  normalize(path, abs);
  snprintf(out, PATH_MAX + 8, "%s", abs + 1);
  return out;
}

char *getcwd(char *buf, size_t size) {
  size_t n = strlen(cwd) + 1;
  if (!buf) {
    buf = malloc(size > n ? size : n);
    if (!buf)
      return NULL;
    size = size > n ? size : n;
  }
  if (size < n) {
    errno = ERANGE;
    return NULL;
  }
  memcpy(buf, cwd, n);
  return buf;
}

int chdir(const char *path) {
  struct stat st;
  if (stat(path, &st) < 0)
    return -1;
  if (!S_ISDIR(st.st_mode)) {
    errno = ENOTDIR;
    return -1;
  }
  char abs[PATH_MAX];
  normalize(path, abs);
  strcpy(cwd, abs);
  return 0;
}

char *realpath(const char *path, char *resolved) {
  struct stat st;
  if (stat(path, &st) < 0)
    return NULL;
  if (!resolved && !(resolved = malloc(PATH_MAX)))
    return NULL;
  return normalize(path, resolved);
}

/* ---- mapped files ------------------------------------------------------------- */

#ifdef MAXSDK_VM
#include <mx/vm.h>

#include "maxsdk/maxsdk_map.h"

/* A file as read-only memory: its pages come from the file when touched and are
   dropped when evicted, so large ROM data takes no swap file and no loading. */
const uint8_t *maxsdk_map_file(const char *path, size_t size) {
  char hp[PATH_MAX + 8];
  u32 mapped = 0;
  const void *p = mx_vm_map_file(host_path(path, hp), &mapped);

  if (p != NULL && mapped < size) {
    mx_vm_unmap_file(p);
    return NULL;
  }
  return (const uint8_t *)p;
}

void maxsdk_unmap_file(const uint8_t *p) { mx_vm_unmap_file(p); }
#endif

/* ---- files ------------------------------------------------------------------- */

static struct {
  int used, fio;
} files[NFILES];

static int fio_of(int fd) {
  int i = fd - FD0;
  if (i < 0 || i >= NFILES || !files[i].used) {
    errno = EBADF;
    return -1;
  }
  return files[i].fio;
}

int open(const char *path, int flags, ...) {
  char hp[PATH_MAX + 8];
  int f;
  switch (flags & O_ACCMODE) {
  case O_WRONLY: f = MX_FIO_WRONLY; break;
  case O_RDWR: f = FIO_RDWR; break;
  default: f = MX_FIO_RDONLY; break;
  }
  if (flags & O_CREAT)
    f |= MX_FIO_CREAT;
  if (flags & O_TRUNC)
    f |= MX_FIO_TRUNC;
  if (flags & O_APPEND)
    f |= FIO_APPEND;
  int i;
  for (i = 0; i < NFILES && files[i].used; i++)
    ;
  if (i == NFILES) {
    errno = EMFILE;
    return -1;
  }
  int fio = mx_fio_open(host_path(path, hp), f);
  if (fio < 0) {
    errno = ENOENT;
    return -1;
  }
  files[i].used = 1, files[i].fio = fio;
  return FD0 + i;
}

int close(int fd) {
  int fio = fio_of(fd);
  if (fio < 0)
    return -1;
  files[fd - FD0].used = 0;
  /* the server's file goes with its last descriptor (dup) */
  for (int i = 0; i < NFILES; i++)
    if (files[i].used && files[i].fio == fio)
      return 0;
  mx_fio_close(fio);
  return 0;
}

/* another descriptor of the same server file, sharing its position */
int dup(int fd) {
  int fio = fio_of(fd);
  if (fio < 0)
    return -1;
  for (int i = 0; i < NFILES; i++)
    if (!files[i].used) {
      files[i].used = 1, files[i].fio = fio;
      return FD0 + i;
    }
  errno = EMFILE;
  return -1;
}

ssize_t read(int fd, void *buf, size_t n) {
  if (fd < FD0) {
    errno = EBADF;
    return -1;
  }
  int fio = fio_of(fd);
  if (fio < 0)
    return -1;
  /* a piece at a time: mx_fio_read pins all of a buffer in VM, and a ROM is
     larger than the frames there are */
  size_t total = 0;
  while (total < n) {
    u32 len = n - total > IO_PIECE ? IO_PIECE : (u32)(n - total);
    int r = mx_fio_read(fio, (char *)buf + total, len);
    if (r < 0) {
      errno = EIO;
      return total ? (ssize_t)total : -1;
    }
    total += (size_t)r;
    if ((u32)r < len)
      break;
  }
  return (ssize_t)total;
}

ssize_t write(int fd, const void *buf, size_t n) {
  if (fd == 1 || fd == 2)
    return mx_glue_write(fd, buf, n);
  int fio = fio_of(fd);
  if (fio < 0)
    return -1;
  /* mx_fio_write: 0, not the bytes, when it wrote them all */
  for (size_t total = 0; total < n;) {
    u32 len = n - total > IO_PIECE ? IO_PIECE : (u32)(n - total);
    if (mx_fio_write(fio, (const char *)buf + total, len) < 0) {
      errno = EIO;
      return -1;
    }
    total += len;
  }
  return (ssize_t)n;
}

off_t lseek(int fd, off_t off, int whence) {
  int fio = fio_of(fd);
  if (fio < 0)
    return -1;
  int r = mx_fio_lseek(fio, (s32)off, whence);
  if (r < 0) {
    errno = EINVAL;
    return -1;
  }
  return r;
}

static void fill_stat(struct stat *st, int dir, long size) {
  memset(st, 0, sizeof *st);
  st->st_mode = dir ? S_IFDIR | 0755 : S_IFREG | 0644;
  st->st_nlink = 1;
  st->st_size = size;
  st->st_blksize = 2048;
  st->st_blocks = (size + 511) / 512;
}

int fstat(int fd, struct stat *st) {
  if (fd >= 0 && fd < FD0) {
    fill_stat(st, 0, 0);
    st->st_mode = S_IFCHR | 0666;
    return 0;
  }
  int fio = fio_of(fd);
  if (fio < 0)
    return -1;
  int cur = mx_fio_lseek(fio, 0, MX_FIO_SEEK_CUR);
  int size = mx_fio_lseek(fio, 0, MX_FIO_SEEK_END);
  mx_fio_lseek(fio, cur, MX_FIO_SEEK_SET);
  fill_stat(st, 0, size);
  return 0;
}

int stat(const char *path, struct stat *st) {
  char hp[PATH_MAX + 8];
  host_path(path, hp);
  /* a directory first: PCSX2's host: opens directories as files too */
  int d = mx_fio_dopen(hp);
  if (d >= 0) {
    mx_fio_dclose(d);
    fill_stat(st, 1, 0);
    return 0;
  }
  int fio = mx_fio_open(hp, MX_FIO_RDONLY);
  if (fio >= 0) {
    int size = mx_fio_lseek(fio, 0, MX_FIO_SEEK_END);
    mx_fio_close(fio);
    fill_stat(st, 0, size);
    return 0;
  }
  errno = ENOENT;
  return -1;
}

int lstat(const char *path, struct stat *st) { return stat(path, st); }

int access(const char *path, int mode) {
  struct stat st;
  return stat(path, &st);
}

int isatty(int fd) { return fd >= 0 && fd < FD0; }

/* mkdir, remove and rmdir: done when the path is (not) there after (PCSX2
   answers a remove with an error but removes the file) */
static int exists(const char *path, int dir) {
  struct stat st;
  return stat(path, &st) == 0 && (dir < 0 || S_ISDIR(st.st_mode) == dir);
}

int mkdir(const char *path, mode_t mode) {
  char hp[PATH_MAX + 8];
  if (exists(path, -1)) {
    errno = EEXIST;
    return -1;
  }
  mx_fio_mkdir(host_path(path, hp), (int)mode);
  if (!exists(path, 1)) {
    errno = EACCES;
    return -1;
  }
  return 0;
}
int unlink(const char *path) {
  char hp[PATH_MAX + 8];
  if (!exists(path, 0)) {
    errno = ENOENT;
    return -1;
  }
  mx_fio_remove(host_path(path, hp));
  if (exists(path, 0)) {
    errno = EACCES;
    return -1;
  }
  return 0;
}
int rmdir(const char *path) {
  char hp[PATH_MAX + 8];
  mx_fio_rmdir(host_path(path, hp));
  if (exists(path, 1)) {
    errno = ENOTEMPTY;
    return -1;
  }
  return 0;
}
int remove(const char *path) { return exists(path, 1) ? rmdir(path) : unlink(path); }
/* the server has no rename: a copy, then the old one removed */
int rename(const char *from, const char *to) {
  if (exists(to, 0))
    unlink(to);
  int in = open(from, O_RDONLY), out = in < 0 ? -1 : open(to, O_WRONLY | O_CREAT | O_TRUNC);
  if (in < 0 || out < 0) {
    if (in >= 0)
      close(in);
    errno = ENOENT;
    return -1;
  }
  static char buf[8192];
  ssize_t n;
  while ((n = read(in, buf, sizeof buf)) > 0)
    write(out, buf, (size_t)n);
  close(in);
  close(out);
  return unlink(from);
}
int chmod(const char *path, mode_t mode) { return 0; }
int fchmod(int fd, mode_t mode) { return 0; }
int ftruncate(int fd, off_t len) {
  errno = EROFS;
  return -1;
}
int truncate(const char *path, off_t len) {
  errno = EROFS;
  return -1;
}

/* ---- directories ----------------------------------------------------------- */

struct njemu_dir {
  int fio;
  struct dirent ent;
};

DIR *opendir(const char *path) {
  char hp[PATH_MAX + 8];
  int d = mx_fio_dopen(host_path(path, hp));
  if (d < 0) {
    errno = ENOENT;
    return NULL;
  }
  DIR *dir = calloc(1, sizeof *dir);
  if (!dir) {
    mx_fio_dclose(d);
    errno = ENOMEM;
    return NULL;
  }
  dir->fio = d;
  return dir;
}

struct dirent *readdir(DIR *dir) {
  u32 size;
  if (mx_fio_dread(dir->fio, dir->ent.d_name, sizeof dir->ent.d_name, &size) <= 0)
    return NULL;
  dir->ent.d_ino = 1;
  dir->ent.d_type = DT_UNKNOWN;
  return &dir->ent;
}

int closedir(DIR *dir) {
  mx_fio_dclose(dir->fio);
  free(dir);
  return 0;
}

/* ---- time and the process ----------------------------------------------------- */

int clock_gettime(clockid_t id, struct timespec *tp) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  tp->tv_sec = tv.tv_sec;
  tp->tv_nsec = tv.tv_usec * 1000;
  return 0;
}


pid_t getpid(void) { return 1; }

int gethostname(char *name, size_t len) {
  snprintf(name, len, "ps2");
  return 0;
}

