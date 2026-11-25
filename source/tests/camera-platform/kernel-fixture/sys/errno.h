#ifndef NEZHA_CAMERA_HOST_ERRNO_H
#define NEZHA_CAMERA_HOST_ERRNO_H
/* Darwin's errno.h includes sys/errno.h, so the fixture must forward
 * directly to the host's next sys/errno.h instead of recursing. */
#if defined(__APPLE__)
#include_next <sys/errno.h>
#else
#include <errno.h>
#endif
#endif
