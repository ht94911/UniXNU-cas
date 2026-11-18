// Executes the source-extracted assembly/release blocks. The VM stand-in models
// an APFS verification-block UPL waiting on any source page still held busy.
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define PAGE_SIZE 4096u
#define VM_PAGE_NULL NULL
#define KERN_SUCCESS 0
#define KERN_FAILURE 5
typedef uintptr_t vm_offset_t;
typedef uint64_t vm_object_offset_t;
struct object { unsigned paging_in_progress, locked; } backing, shadow;
struct page { struct object *object; bool vmp_busy; } source_page, placeholder;
typedef struct object *vm_object_t;
typedef struct page *vm_page_t;
struct vm_object_fault_info { int unused; };
#define VM_PAGE_OBJECT(p) ((p)->object)
static unsigned releases, frees, peeks, fixups;
static int fail_peek, fail_fixup, saw_busy;
static unsigned block_size;
static void vm_object_lock(vm_object_t p) { assert(!p->locked); p->locked=1; }
static void vm_object_unlock(vm_object_t p) { assert(p->locked); p->locked=0; }
static void vm_page_wakeup_done(vm_object_t o, vm_page_t p) {
    assert(o->locked && p->object==o && p->vmp_busy);p->vmp_busy=false;++releases;
}
static void vm_object_paging_end(vm_object_t p) { assert(p->locked && p->paging_in_progress);--p->paging_in_progress; }
static void free_page(vm_page_t p) { assert(p->object->locked && p->vmp_busy);p->vmp_busy=false;++frees; }
#define VM_PAGE_FREE(p) free_page(p)
static unsigned char expected(unsigned i) { return (unsigned char)(i*13u+(i/PAGE_SIZE)*17u); }
static int dyld_pager_peek(vm_object_t o, vm_object_offset_t offset, void *buffer,
                          struct vm_object_fault_info *info) {
    (void)o;(void)info;++peeks;
    assert(!(offset%PAGE_SIZE) && offset<block_size);
    if (source_page.vmp_busy || placeholder.vmp_busy) { saw_busy=1;return KERN_FAILURE; }
    assert(!backing.paging_in_progress && !shadow.paging_in_progress);
    if (fail_peek && peeks==(unsigned)fail_peek) return KERN_FAILURE;
    for (unsigned i=0;i<PAGE_SIZE;++i) ((unsigned char*)buffer)[i]=expected((unsigned)offset+i);
    return KERN_SUCCESS;
}
static int fixup_page(vm_offset_t contents, uint64_t userVA, void *pager) {
    (void)pager;++fixups;assert(userVA==0x100000000ull);
    unsigned char *p=(void*)contents;
    for(unsigned i=0;i<block_size;++i) assert(p[i]==expected(i));
    if(fail_fixup) return KERN_FAILURE;
    for(unsigned i=0;i<block_size;i+=PAGE_SIZE) p[i]^=0xff;
    return KERN_SUCCESS;
}
static void check(unsigned part, unsigned page_size, bool has_shadow, int failure) {
    block_size=page_size;releases=frees=peeks=fixups=0;saw_busy=0;
    fail_peek=failure>0?failure:0;fail_fixup=failure<0;
    backing=(struct object){.paging_in_progress=1};
    shadow=(struct object){.paging_in_progress=has_shadow?1u:0u};
    source_page=(struct page){.object=&backing,.vmp_busy=true};
    placeholder=(struct page){.object=&shadow,.vmp_busy=has_shadow};
    vm_object_t src_page_object=&backing,src_top_object=has_shadow?&shadow:&backing;
    vm_page_t src_page=&source_page,top_page=has_shadow?&placeholder:VM_PAGE_NULL;
    unsigned char src[PAGE_SIZE],dst[PAGE_SIZE],buffer[4*PAGE_SIZE];
    for(unsigned i=0;i<PAGE_SIZE;++i) src[i]=expected(part*PAGE_SIZE+i);
    memcpy(dst,src,PAGE_SIZE);memset(buffer,0,sizeof buffer);
    vm_offset_t src_vaddr=(uintptr_t)src,dst_vaddr=(uintptr_t)dst;
    void *fixup_buffer=buffer,*pager=NULL;
    struct vm_object_fault_info fault_info={0};
    unsigned mwli_page_size=page_size,i;
    vm_object_offset_t offset=0,cur_offset;
    uint64_t userVA=0x100000000ull+part*PAGE_SIZE;
    int retval=KERN_SUCCESS,kr;
    for(cur_offset=part*PAGE_SIZE;cur_offset<(part+1)*PAGE_SIZE;cur_offset+=PAGE_SIZE) {
        if(mwli_page_size>PAGE_SIZE) {
#include "assemble.inc"
        } else if(fixup_page(dst_vaddr,userVA,pager)!=KERN_SUCCESS) retval=KERN_FAILURE;
#include "release.inc"
    }
    assert(releases==1 && frees==(unsigned)has_shadow);
    assert(!source_page.vmp_busy && !placeholder.vmp_busy);
    assert(!backing.paging_in_progress && !shadow.paging_in_progress && !backing.locked && !shadow.locked);
#if EXPECT_OLD
    if(page_size>PAGE_SIZE) { assert(saw_busy && retval!=0 && peeks==1 && fixups==0);return; }
#else
    assert(!saw_busy);
#endif
    if(failure) {assert(retval!=0 && !memcmp(src,dst,PAGE_SIZE));}
    else {assert(retval==0 && fixups==1 && dst[0]==(unsigned char)(src[0]^0xff));assert(!memcmp(src+1,dst+1,PAGE_SIZE-1));}
}
int main(void) {
    for(unsigned s=0;s<2;++s) {
        check(0,PAGE_SIZE,s,0);check(0,PAGE_SIZE,s,-1);
        for(unsigned part=0;part<4;++part) {
            check(part,4*PAGE_SIZE,s,0);
            check(part,4*PAGE_SIZE,s,-1);
            for(int f=1;f<=3;++f) check(part,4*PAGE_SIZE,s,f);
        }
    }
    puts(EXPECT_OLD?"PASS: original path reproduces busy-source wait; exact source cleanup balanced":"PASS: upstream path releases before every peek; all parts, shadow/no-shadow, read/fixup failures, 4KiB unchanged");
}
