#include <assert.h>
#include <string.h>
#include "../package/nezha27-input/NezhaTouchSelfTest.h"
int main(void) {
 struct nezha_touch_test t={0};
 struct nezha_touch_frame f;
 memset(&f,0x5a,sizeof(f));
 assert(nezha_test_next(&t,1,100,0,0,1,&f)==NZ_TEST_NONE && !t.phase);
 assert(nezha_test_next(&t,1,100,1,1,1,&f)==NZ_TEST_NONE);
 assert(nezha_test_next(&t,1,100,1,0,0,&f)==NZ_TEST_NONE);
 assert(nezha_test_next(&t,1000,100,1,0,1,&f)==NZ_TEST_FRAME);
 assert(t.phase==1 && f.active==1 && f.x[0]==444 && f.y[0]==1872);
 assert(f.sequence==0xf0000001 && f.x[1]==0);
 assert(nezha_test_next(&t,1099,100,1,0,0,&f)==NZ_TEST_HOLD);
 assert(nezha_test_next(&t,1100,100,1,0,0,&f)==NZ_TEST_FRAME);
 assert(t.phase==2 && f.active==0 && f.sequence==0xf0000002);
 assert(nezha_test_next(&t,1200,100,1,0,1,&f)==NZ_TEST_NONE);
 memset(&t,0,sizeof(t));
 assert(nezha_test_next(&t,1000,100,1,0,1,&f)==NZ_TEST_FRAME);
 assert(nezha_test_next(&t,1001,100,1,1,0,&f)==NZ_TEST_FRAME && f.active==0);
 memset(&t,0,sizeof(t));
 assert(nezha_test_next(&t,UINT64_MAX-49,100,1,0,1,&f)==NZ_TEST_FRAME);
 assert(nezha_test_next(&t,20,100,1,0,0,&f)==NZ_TEST_HOLD);
 assert(nezha_test_next(&t,51,100,1,0,0,&f)==NZ_TEST_FRAME && f.active==0);
}
