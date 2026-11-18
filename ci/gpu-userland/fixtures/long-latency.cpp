#include <cassert>
#include <cstring>
#include <string>
#include <vector>
#include <climits>
#include "NativeLongLatency.h"
int main() {
 std::vector<std::string> out;
 auto emit=[&](const char *s){assert(strlen(s)<=88);out.emplace_back(s);};
 nblonglatency::Command row{1,1000000,1046435,1052064,1106105,1106200,1106300,1012,1012,4000000,4043953,64,385};
 nblonglatency::command(row,emit);assert(out.size()==5);
 assert(out[0]=="lf0 i=1 t=1012 c=1012 u=1000000");
 assert(out[1]=="lf1 i=1 m=1046435 s=1052064");
 assert(out[2]=="lf2 i=1 g=1106105 a=1106200");
 assert(out[3]=="lf3 i=1 z=1106300 p=64 d=385");
 assert(out[4]=="lf4 i=1 x=4000000 y=4043953");
 out.clear();row.handlersEnded=1099999;row.handlersBegan=1099900;row.callback=1099800;
 nblonglatency::command(row,emit);assert(out.empty());
 row.handlersEnded=1100000;nblonglatency::command(row,emit);assert(out.size()==5);
 out.clear();row.created=0;nblonglatency::command(row,emit);assert(out.empty());
 row={1,100,99,0,200000,200001,200002,1,1,0,0,1,1};nblonglatency::command(row,emit);assert(out.empty());
 row={1,100,101,102,100001,100002,100003,1,1,0,0,1,1};nblonglatency::command(row,emit);assert(out.empty());
 row={UINT64_MAX,1,2,3,UINT64_MAX-2,UINT64_MAX-1,UINT64_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX};nblonglatency::command(row,emit);assert(out.size()==5);
 out.clear();const char *label="-[NBDeviceMethods newBufferWithBytesNoCopy:length:options:deallocator:]";
 nblonglatency::api(7,1012,1000,16999,(uintptr_t)label,344000,true,emit);assert(out.empty());
 nblonglatency::api(7,1012,1000,17000,(uintptr_t)label,344000,true,emit);assert(out.size()==3);assert(out[0]=="ls0 i=7 t=1012 b=1000 e=17000");
 assert(out[2].substr(0,10)=="ls2 i=7 n=");
 out.clear();nblonglatency::api(UINT64_MAX,UINT32_MAX,1,UINT64_MAX,(uintptr_t)label,UINT64_MAX,true,emit);assert(out.size()==3);
 out.clear();nblonglatency::api(1,1,2,1,(uintptr_t)label,0,false,emit);assert(out.empty());
 out.clear();nblonglatency::producerGap(5,1012,1000000,1100000,1101000,emit);assert(out.size()==2);
 assert(out[0]=="lg0 i=5 t=1012 b=1000000 c=1100000");
 assert(out[1]=="lg1 i=5 m=1101000");
 out.clear();nblonglatency::producerGap(5,1012,1000000,1099999,1101000,emit);assert(out.empty());
 puts("PASS 13 wire threshold/order/width and original106ms command/gap scenarios");
}
