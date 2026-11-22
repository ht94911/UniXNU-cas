#include <stddef.h>
#include <sys/types.h>
#if !defined(__APPLE__)
typedef ssize_t user_ssize_t;
#endif
struct uio {uint8_t *bytes;size_t capacity,offset;};
user_ssize_t uio_resid(struct uio *);
int uiomove(caddr_t,int,struct uio *);
