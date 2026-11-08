/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include <string.h>

/* Existing 0x7fff/0x7ffe + 0x70cc wire, with optional 0x7100 end marker.
 * The owner holds a short CPU lock. No allocation, GPU access or logging here.
 * Keying by PID prevents independent processes from mixing diagnostic lines.
 */
struct NativeGpuTextStream {
    struct Message { uint32_t pid=0,channel=0,truncated=0,legacy=0; char text[64]{}; };
    struct Slot { uint32_t pid=0,channel=0,length=0; bool active=false,truncated=false; char text[64]{}; };
    Slot slots[32]{};
    uint32_t dropped=0;

    static void finish(const Slot &s,Message &out,bool legacy) {
        out.pid=s.pid;out.channel=s.channel;out.truncated=s.truncated;out.legacy=legacy;
        memcpy(out.text,s.text,sizeof(out.text));out.text[63]=0;
    }
    bool accept(uint32_t pid,uint32_t value,Message &out) {
        out={};
        if(!pid)return false;
        Slot *s=nullptr,*free=nullptr;
        for(auto &item:slots) {
            if(item.pid==pid)s=&item;
            if(!item.active && !free)free=&item;
        }
        if(value==0x7fff || value==0x7ffe) {
            if(!s)s=free;
            if(!s){++dropped;return false;}
            const bool previous=s->active && s->length;
            if(previous)finish(*s,out,true);
            *s={};s->pid=pid;s->channel=value==0x7ffe?7:8;s->active=true;
            return previous;
        }
        if(!s || !s->active)return false;
        if(value==0x7100) {
            finish(*s,out,false);s->active=false;return true;
        }
        if(value>=0x7000 && value<0x7100) {
            if(s->length<63) {
                unsigned c=value&255;s->text[s->length++]=char(c>=32 && c<127?c:'?');
                s->text[s->length]=0;
            } else s->truncated=true;
        }
        return false;
    }
};
