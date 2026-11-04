
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
using VkResult=int;constexpr VkResult VK_SUCCESS=0;
static int reserve_result=0,reserve_words=0,writes=0;
struct CS {uint32_t *cur=nullptr;};
struct CB {CS cs;struct {int record_result=0;} vk;};
static uint32_t memory[4096];
static VkResult tu_cs_reserve_space(CS *cs,unsigned words){reserve_words=words;if(!reserve_result)cs->cur=memory;return reserve_result;}
static VkResult begin(CB *cmd_buffer){
   /* Packet emitters assume backing storage. Report a failed first allocation
    * from this VkResult API before trace/status/init packets touch a null CS.
    * This is the same4096-dword initial block selected at command creation. */
   VkResult cs_result = tu_cs_reserve_space(&cmd_buffer->cs, 4096);
   if (cs_result != VK_SUCCESS) {
      cmd_buffer->vk.record_result = cs_result;
      return cs_result;
   }


 ++writes;*cmd_buffer->cs.cur=42;return 0;
}
static int alive=0;static bool began=false;static std::string picked;
struct Gpu {unsigned last=0;void mark(unsigned n){last=n;}};
struct Job {char cache[512]{};Job(){++alive;}~Job(){--alive;}};
constexpr int W_OK=2;
static int fixture_access(const char *p,int){return !strcmp(p,"/var/mobile/Library/Caches/published/m2v")?0:-1;}
static void streamText7(Gpu&,const char*){}
#define access fixture_access
static void cache_check(Gpu &gpu,const char *runtimeCache){Job *job=new Job{};
    if (runtimeCache && *runtimeCache) {
        if (strlcpy(job->cache, runtimeCache, sizeof job->cache) >= sizeof job->cache ||
            access(job->cache, W_OK) != 0) {
            streamText7(gpu, "M cache rejected; test not started; UI remains independent");
            gpu.mark(0x65f1);
            delete job;
            return;
        }
        streamText7(gpu, "M cache: reuse published runtime writable directory");
    }

 began=true;picked=job->cache;delete job;
}
int main(){
 for(int result:{-1,-2,-3,-4,-5}){CB cb;reserve_result=result;writes=0;assert(begin(&cb)==result && cb.vk.record_result==result && writes==0 && cb.cs.cur==nullptr);}
 CB cb;reserve_result=0;writes=0;assert(begin(&cb)==0 && writes==1 && reserve_words==4096 && memory[0]==42);
 Gpu good;began=false;cache_check(good,"/var/mobile/Library/Caches/published/m2v");assert(began && picked=="/var/mobile/Library/Caches/published/m2v" && alive==0);
 for(const std::string &path:{std::string("/private/var/tmp/m2v"),std::string(512,'x'),std::string(900,'x')}){
  Gpu bad;began=false;cache_check(bad,path.c_str());assert(!began && bad.last==0x65f1 && alive==0);
 }
 puts("PASS: five real-error guard paths,no null CS write,normal4096-word allocation,published cache reuse,unwritable/oversize cache refusal,owner cleanup");
}
