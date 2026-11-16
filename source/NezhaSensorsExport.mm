// SPDX-License-Identifier: BSD-2-Clause-Patent
// Root diagnostic job exports only the dedicated sensor app's test log.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include "QcomUserLogTransport.h"
#pragma clang diagnostic pop
#include <dirent.h>
#include <signal.h>
static volatile sig_atomic_t stopping;
static void stop(int){stopping=1;}
int main(){
    if(geteuid()!=0)return 2;
    signal(SIGTERM,stop);signal(SIGINT,stop);signal(SIGALRM,stop);alarm(900);
    FILE *input=nullptr;
    for(unsigned wait=0;!stopping&&wait<300;wait++){
        const char *base="/private/var/mobile/Containers/Data/Application";
        DIR *directory=opendir(base);char selected[512]={};unsigned found=0;
        if(directory){
            while(struct dirent *entry=readdir(directory)){
                if(entry->d_name[0]=='.')continue;
                char path[512];int length=snprintf(path,sizeof path,"%s/%s/Documents/nezha-sensors001.jsonl",base,entry->d_name);
                if(length<=0||(size_t)length>=sizeof path)continue;
                struct stat info{};
                if(!lstat(path,&info)&&S_ISREG(info.st_mode)){memcpy(selected,path,(size_t)length+1);found++;}
            }
            closedir(directory);
        }
        if(found==1)input=fopen(selected,"r");
        if(input)break;sleep(1);
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
