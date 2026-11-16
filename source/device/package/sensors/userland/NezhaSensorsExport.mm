// SPDX-License-Identifier: BSD-2-Clause-Patent
// Root diagnostic job exports only the dedicated sensor app's test log.
#include "QcomUserLogTransport.h"
#include <glob.h>
#include <signal.h>
#include <string>
static volatile sig_atomic_t stopping;
static void stop(int){stopping=1;}
int main(){
    if(geteuid()!=0)return 2;
    signal(SIGTERM,stop);signal(SIGINT,stop);signal(SIGALRM,stop);alarm(900);
    FILE *input=nullptr;
    for(unsigned wait=0;!stopping&&wait<300;wait++){
        glob_t paths{};int r=glob("/private/var/mobile/Containers/Data/Application/*/Documents/nezha-sensors001.jsonl",0,nullptr,&paths);
        if(!r&&paths.gl_pathc==1)input=fopen(paths.gl_pathv[0],"r");
        globfree(&paths);if(input)break;sleep(1);
    }
    if(!input){QcomUserLog::status("sensors001: dedicated app log unavailable");return 3;}
    char line[1000];
    while(!stopping){
        if(fgets(line,sizeof line,input)){
            size_t n=strlen(line);if(n&&line[n-1]=='\n')line[n-1]=0;
            if(!QcomUserLog::status(line)){fclose(input);return 4;}
        }else{clearerr(input);usleep(250000);}
    }
    fclose(input);QcomUserLog::status("sensors001: exporter closed");return 0;
}
