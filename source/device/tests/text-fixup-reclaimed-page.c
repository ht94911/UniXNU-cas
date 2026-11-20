#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <bsd/string.h>
#define PAGE_SIZE 4096u
#define PAGE_MASK 4095u
#define __arm64__ 1
#define CHECK_CS_VALIDATION_BITMAP 1
#define NULLVP NULL
#define KERN_SUCCESS 0
#define VMP_CS_ALL_TRUE 15
#define VMP_CS_ALL_FALSE 0
#define TRUE 1
#define FALSE 0
#define CS_BITMAP_CHECK 3
#define VM_OBJECT_NULL NULL
#define TUNABLE_STR(name,size,arg,value) char name[size]
#define STARTUP(a,b,c)
#define os_atomic_inc(p,ordering) (++*(p))
typedef uint64_t memory_object_offset_t;
typedef uint64_t vm_map_size_t;
typedef uint64_t vm_map_offset_t;
typedef uint64_t pmap_paddr_t;
typedef int kern_return_t;
typedef int boolean_t;
typedef void *DTEntry;
#define kSuccess 0
struct vnode {const char *v_name;};
struct fake_object {int alive,terminating;struct vnode *pager;uint64_t paging_offset;};
struct fake_page {struct fake_object *object;int vmp_wpmapped,vmp_cs_validated,vmp_cs_tainted;uint64_t vmp_offset;unsigned char *bytes;};
typedef struct fake_object *vm_object_t;
typedef struct fake_page *vm_page_t;
static const char *selected;static unsigned selected_size;
static int SecureDTLookupEntry(void*a,const char*b,DTEntry*c){(void)a;assert(!strcmp(b,"/chosen"));*c=(void*)1;return 0;}
static int SecureDTGetProperty(DTEntry a,const char*b,const void**c,unsigned*d){(void)a;assert(!strcmp(b,"text-fixups"));*c=selected;*d=selected_size;return 0;}
#define VM_PAGE_OBJECT(p) ((p)->object)
#define VM_PAGE_GET_PHYS_PAGE(p) ((uintptr_t)(p)->bytes >> 12)
#define PAGE_SHIFT 12
#define VMP_CS_TAINTED(p,s,o) ((void)(s),(void)(o),(p)->vmp_cs_tainted)
#define VMP_CS_VALIDATED(p,s,o) ((void)(s),(void)(o),(p)->vmp_cs_validated)
#define VMP_CS_SET_TAINTED(p,s,o,v) ((void)(s),(void)(o),(p)->vmp_cs_tainted=(v)?15:0)
#define VMP_CS_SET_VALIDATED(p,s,o,v) ((void)(s),(void)(o),(p)->vmp_cs_validated=(v)?15:0)
#define vm_object_lock_assert_held(o) ((void)(o))
#define vm_object_lock_assert_exclusive(o) ((void)(o))
static int cs_debug,vm_cs_validated_dirtied,vm_cs_bitmap_validated,bitmap_result;
static unsigned checks,lookups,syncs;
static int vnode_pager_cs_check_validation_bitmap(struct vnode*v,uint64_t o,int mode){(void)v;(void)o;assert(mode==3);++checks;return bitmap_result;}
static struct vnode *vnode_pager_lookup_vnode(struct vnode *p){++lookups;return p;}
static uintptr_t phystokv(uintptr_t p){return p;}
static void cache_sync_page(uintptr_t p){(void)p;++syncs;}

#include "applicator.inc"
#include "validation.inc"
static unsigned char data[PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));
static unsigned char original[PAGE_SIZE];
static struct vnode vnode={"locationd"};
static struct fake_object object={1,0,&vnode,0};
static struct fake_page page={&object,0,0,0,0x1000,data};
static void reset(void){memcpy(data,original,PAGE_SIZE);page.vmp_cs_validated=page.vmp_cs_tainted=page.vmp_wpmapped=0;page.vmp_offset=0x1000;object.alive=1;object.terminating=0;object.pager=&vnode;object.paging_offset=0;vnode.v_name="locationd";checks=lookups=syncs=0;bitmap_result=0;}
int main(void){
 memset(original,0x6b,PAGE_SIZE);original[0x24]=1;original[0x25]=2;original[0x26]=3;original[0x27]=4;
 selected="locationd,1024,01020304,aabbccdd";selected_size=strlen(selected)+1;cs_text_fixup_init();assert(cs_text_fixup_count==1);
 reset();assert(cs_text_fixup_page(&vnode,0x1000,data)==1);assert(data[0x24]==0xaa);
 reset();assert(before_validate(&page,PAGE_SIZE,0)==TRUE);assert(data[0x24]==1&&syncs==0);puts("PASS old bitmap re-read retains original code (regression reproduced)");
 unsigned cases=0;
 reset();assert(after_validate(&page,PAGE_SIZE,0)==TRUE);assert(data[0x24]==0xaa&&syncs==1&&checks==1&&lookups==1);++cases;
 assert(after_validate(&page,PAGE_SIZE,0)==TRUE);assert(syncs==1);++cases;
 reset();vnode.v_name="unselected";assert(after_validate(&page,PAGE_SIZE,0)==TRUE);assert(data[0x24]==1&&syncs==0);++cases;
 reset();data[0x24]=7;assert(after_validate(&page,PAGE_SIZE,0)==TRUE);assert(data[0x24]==7&&syncs==0);++cases;
 reset();page.vmp_cs_tainted=15;assert(after_validate(&page,PAGE_SIZE,0)==TRUE);assert(data[0x24]==1&&checks==0&&syncs==0);++cases;
 reset();page.vmp_wpmapped=1;assert(after_validate(&page,PAGE_SIZE,0)==TRUE);assert(page.vmp_cs_tainted==15&&data[0x24]==1&&checks==0);++cases;
 reset();bitmap_result=5;assert(after_validate(&page,PAGE_SIZE,0)==FALSE);assert(data[0x24]==1&&syncs==0);++cases;
 reset();bitmap_result=5;object.alive=0;assert(after_validate(&page,PAGE_SIZE,0)==TRUE);assert(data[0x24]==1&&lookups==0);++cases;
 reset();page.vmp_offset=0xfff;object.paging_offset=1;assert(after_validate(&page,PAGE_SIZE,0)==TRUE);assert(data[0x24]==0xaa&&syncs==1);++cases;
 reset();page.vmp_offset=0x2000;assert(after_validate(&page,PAGE_SIZE,0)==TRUE);assert(data[0x24]==1&&syncs==0);++cases;
 reset();assert(after_validate(&page,PAGE_SIZE,0)==TRUE);memcpy(data,original,PAGE_SIZE);page.vmp_cs_validated=0;assert(after_validate(&page,PAGE_SIZE,0)==TRUE);assert(data[0x24]==0xaa&&syncs==2);++cases;
 printf("PASS reclaimed-page cases %u, selected bytes and original validity rules retained\n",cases);return 0;
}
