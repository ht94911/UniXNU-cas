/* SPDX-License-Identifier: BSD-2-Clause */
/* Included after scm_smc in NezhaScm.c. No second SMC or WAITQ implementation. */
#include "../platform/NbScm.h"
extern int nb_native_low_sync(void *cpu,uint64_t physical,uint64_t bytes);
static int nb_scm_result(const struct scm_res *r)
{
    if (r->a0) return NB_IO;
    if ((int64_t)r->a1 == -22) return -22;
    return r->a1 ? NB_IO : NB_OK;
}
int nezha_bb_scm_pas(unsigned command,unsigned image,uint64_t a,uint64_t b)
{
    if ((image!=4 && image!=0x26 && image!=15) ||
        (command!=1 && command!=2 && command!=5 && command!=6)) return NB_ARGUMENT;
    if (command==1 && (!a || (a&4095) || a>=0x100000000ULL)) return NB_ARGUMENT;
    struct scm_res r={0,0,0,0};
    uint64_t info=command==1 ? SCM_ARGS(2)|(SCM_RW<<6) : command==2 ? SCM_ARGS(3):SCM_ARGS(1);
    scm_smc(SCM_FN(2,command),info,image,a,b,0,&r);
    printf("NEZHA27 BB PAS: command=%u image=%#x a0=%#llx a1=%#llx a2=%#llx a3=%#llx\n",
           command,image,(unsigned long long)r.a0,(unsigned long long)r.a1,
           (unsigned long long)r.a2,(unsigned long long)r.a3);
    return nb_scm_result(&r);
}
int nezha_bb_scm_query(unsigned command,unsigned image,uint64_t result[4])
{
    if (!result || (image!=4 && image!=0x26) || (command!=7 && command!=9)) return NB_ARGUMENT;
    struct scm_res r={0,0,0,0};
    scm_smc(SCM_FN(2,command),SCM_ARGS(1),image,0,0,0,&r);
    result[0]=r.a0;result[1]=r.a1;result[2]=r.a2;result[3]=r.a3;
    printf("NEZHA27 BB PASQUERY: command=%u image=%#x a0=%#llx a1=%#llx a2=%#llx a3=%#llx\n",
           command,image,(unsigned long long)r.a0,(unsigned long long)r.a1,
           (unsigned long long)r.a2,(unsigned long long)r.a3);
    return r.a0 ? NB_IO:NB_OK;
}
int nezha_bb_scm_assign(void *page,uint64_t pa,int *bridge_state,uint64_t *handle,
                        NbRange region,int to_remote,int shared)
{
    if (!bridge_state || !handle || *bridge_state<0) return NB_OWNERSHIP;
    int e=nb_scm_assignment_page(page,pa,region,to_remote,shared);if(e)return e;
    e=nb_native_low_sync(page,pa,4096);if(e)return e;
    struct scm_res r={0,0,0,0};
    if (!*bridge_state) {
        /* Same registered parameter page for every request, retained at boot.
         * Qcom tzmem: NS/S permissions RW, one HLOS VM, one physical page. */
        *bridge_state=-1;
        scm_smc(SCM_FN(0xc,0x1e),SCM_ARGS(4),pa|6,pa|6,4096|(1u<<9),3,&r);
        e=nb_scm_result(&r);if(e)return e;
        *handle=r.a2;*bridge_state=1;
    }
    scm_smc(SCM_FN(0xc,0x16),SCM_ARGS(7)|(1u<<4)|(1u<<8)|(1u<<12),pa+64,16,pa,pa+256,&r);
    return nb_scm_result(&r);
}
