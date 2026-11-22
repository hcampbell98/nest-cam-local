#include <stdlib.h>
#include <sys/stat.h>

extern int __xstat(int ver, const char *path, struct stat *buf);
extern int __fxstat(int ver, int fd, struct stat *buf);
extern int __lxstat(int ver, const char *path, struct stat *buf);

int stat(const char *path, struct stat *buf) { return __xstat(3, path, buf); }
int fstat(int fd, struct stat *buf) { return __fxstat(3, fd, buf); }
int lstat(const char *path, struct stat *buf) { return __lxstat(3, path, buf); }
long __isoc23_strtol(const char *nptr, char **endptr, int base) { return strtol(nptr, endptr, base); }
unsigned long __isoc23_strtoul(const char *nptr, char **endptr, int base) { return strtoul(nptr, endptr, base); }
