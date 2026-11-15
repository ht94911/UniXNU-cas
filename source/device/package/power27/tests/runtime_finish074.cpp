// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <cstdlib>
#define main prior_shutdown_cases_not_executed
#include "runtime_shutdown.cpp"
#undef main
int main(int argc,char **argv) {
 assert(argc==2);const unsigned scenario=(unsigned)strtoul(argv[1],nullptr,10);assert(scenario<8);
 assert(np_power_finish_request()==-1&&!stopRequested);no_inline_cleanup();
 bind(2);
 if(scenario==1)lifecycle=3;
 if(scenario==2){lifecycle=1;suspendRequested=1;assert(np_power_finish_request()==0&&!stopRequested);no_inline_cleanup();return 0;}
 if(scenario==3){lifecycle=3;suspendRequested=0;assert(np_power_finish_request()==0&&!stopRequested);no_inline_cleanup();return 0;}
 if(scenario==4){system_inshutdown=1;assert(np_power_finish_request()==-1&&!stopRequested);no_inline_cleanup();return 0;}
 if(scenario==5){workerExited=1;assert(np_power_finish_request()==-1&&!stopRequested);no_inline_cleanup();return 0;}
 if(scenario==6){terminal_phase=3;terminal_errno=ENOENT;}
 if(scenario==7){control.buck.owned=64;control.buck.state=NP_BUCK_UNCERTAIN;}
 if(scenario==1)suspendRequested=1;
 assert(np_power_finish_request()==1&&stopRequested);no_inline_cleanup();
 assert(np_power_finish_request()==0);no_inline_cleanup();
 worker(nullptr,0);const int error=scenario==6?ENOENT:scenario==7?EIO:0;
 assert(workerExited&&log_stops==1&&shutdown_rows==1);
 assert(exit_errno==error&&lifecycle==(error?-1:5));
 assert(np_power_finish_request()==-1&&np_power_lifecycle_request(1)==-1);
 assert(footer_closed==(scenario!=6));terminal_worker(error);
 printf("PASS finish074 case%u: one queued finish, actualworker log closure, retained errno%d\n",scenario,error);return 0;
}
