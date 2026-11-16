/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Explicit bounded hardware probe; no launchd job or automatic vibration.
 * Run only in the designated candidate after the phone window is claimed. */
#include "../NezhaHapticsProtocol.h"
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
int main(int argc,char **argv) {
    if(argc!=3 || strcmp(argv[1],"pulse"))return 2;
    char *end=NULL;unsigned long ms=strtoul(argv[2],&end,10);
    if(!end || *end || !ms || ms>100)return 2;
    int fd=open(NZ_HAPTICS_DEVICE,O_WRONLY|O_NONBLOCK|O_CLOEXEC);if(fd<0)return 3;
    NzHapticsStatus s={0};int e=ioctl(fd,NZ_HAPTICS_GET_STATUS,&s);
    NzHapticsPacket p={NZ_HAPTICS_MAGIC,NZ_HAPTICS_ABI,NzHapticsStart,0,s.session,s.epoch,0,0,0};
    if(!e && write(fd,&p,sizeof p)!=(ssize_t)sizeof p)e=1;
    if(!e)e=ioctl(fd,NZ_HAPTICS_GET_STATUS,&s);
    p=(NzHapticsPacket){NZ_HAPTICS_MAGIC,NZ_HAPTICS_ABI,NzHapticsPulse,(uint32_t)ms,s.session,s.epoch,s.next_sequence,250,0};
    if(!e && write(fd,&p,sizeof p)!=(ssize_t)sizeof p)e=1;
    for(unsigned waited=0;!e && waited<200;++waited) {
        usleep(1000);e=ioctl(fd,NZ_HAPTICS_GET_STATUS,&s);
        if(!e && s.error){e=1;break;}
        if(!e && s.completed)break;
    }
    if(!e && !s.completed)e=1;
    p=(NzHapticsPacket){NZ_HAPTICS_MAGIC,NZ_HAPTICS_ABI,NzHapticsStop,0,s.session,s.epoch,s.next_sequence,0,0};
    int stop=write(fd,&p,sizeof p)==(ssize_t)sizeof p?0:errno;
    printf("haptics session=%u epoch=%u requests=%u completed=%u cancelled=%u error=%u stop=%d\n",
        s.session,s.epoch,s.requests,s.completed,s.cancelled,s.error,stop);
    close(fd);return e||stop?1:0;
}
