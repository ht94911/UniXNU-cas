#include <stddef.h>
#include <sys/types.h>
typedef ssize_t user_ssize_t;
struct uio {uint8_t *bytes;size_t capacity,offset;};
user_ssize_t uio_resid(struct uio *);
int uiomove(caddr_t,int,struct uio *);
