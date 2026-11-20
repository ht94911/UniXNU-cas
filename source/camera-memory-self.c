/* SPDX-License-Identifier: MIT */
#include "NezhaCameraMemory.h"
#include <assert.h>
#include <stdio.h>
static unsigned calls;
static int result;
static int control(uint32_t command,int32_t pid,uint32_t flags,void *buffer,size_t size)
{
    assert(command==22 && pid==471);
    assert(flags==718u*1048576u && !buffer && !size);
    ++calls;
    return result;
}
int main(void)
{
    assert(nezha_camera_memory_increase(NULL,"cameracaptured",471)==-1 && !calls);
    assert(nezha_camera_memory_increase(control,NULL,471)==-1 && !calls);
    assert(nezha_camera_memory_increase(control,"",471)==-1 && !calls);
    assert(nezha_camera_memory_increase(control,"Camera",471)==-1 && !calls);
    assert(nezha_camera_memory_increase(control,"cameracapture",471)==-1 && !calls);
    assert(nezha_camera_memory_increase(control,"cameracaptured2",471)==-1 && !calls);
    assert(nezha_camera_memory_increase(control,"cameracaptured",0)==-1 && !calls);
    assert(nezha_camera_memory_increase(control,"cameracaptured",-1)==-1 && !calls);
    result=1;
    assert(nezha_camera_memory_increase(control,"cameracaptured",471)==1 && calls==1);
    result=0;
    assert(nezha_camera_memory_increase(control,"cameracaptured",471)==0 && calls==2);
    puts("PASS: ten self-only finite memory increase cases; exact process/pid/cmd22, one call and unchanged error");
}
