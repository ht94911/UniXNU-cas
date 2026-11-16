#include <sys/types.h>
#define DEVFS_CHAR 1
void *devfs_make_node(dev_t,int,uid_t,gid_t,int,const char *,...);
