// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>
#include "Sm8850DsiFifo.h"
#include "NezhaDsiObservation.h"

// Board mapping only. Generic FIFO operation stays in the selected SM8850
// source. DSI1,MDSS top,clock/reset registers and memory DMA are untouched.
struct NezhaDsi0 {
    IODeviceMemory *memory=nullptr;
    IOMemoryMap *mapping=nullptr;
    volatile uint32_t *words=nullptr;
    struct sm_dsi_transfer transfer={};
    struct sm_dsi_proof proof={};
    static bool validOffset(uint32_t offset) {
        switch(offset) {
        case SM_DSI_HW:case SM_DSI_CTRL:case SM_DSI_STATUS:case SM_DSI_DMA_CTRL:
        case SM_DSI_LENGTH:case SM_DSI_TRIGGER:case SM_DSI_INT:case SM_DSI_TPG:
        case SM_DSI_FIFO:case SM_DSI_FIFO_RESET:case SM_DSI_DMA_CTRL1:return true;
        default:return false;
        }
    }
    static bool read(void *cookie,uint32_t offset,uint32_t *value) {
        auto self=static_cast<NezhaDsi0 *>(cookie);
        if(!self || !self->words || !value || !validOffset(offset))return false;
        __asm__ volatile("dsb sy" ::: "memory");*value=self->words[offset/4];
        nezha_dsi_observation_read(cookie,offset,*value);return true;
    }
    static bool write(void *cookie,uint32_t offset,uint32_t value) {
        auto self=static_cast<NezhaDsi0 *>(cookie);
        if(!self || !self->words || !validOffset(offset) ||
           offset==SM_DSI_HW || offset==SM_DSI_CTRL || offset==SM_DSI_STATUS)return false;
        self->words[offset/4]=value;return true;
    }
    static void barrier(void *) {__asm__ volatile("dsb oshst" ::: "memory");}
    static void delay(void *) {IODelay(1);}
    void close() {
        // Unmap only software ownership;never reset a command of uncertain
        // outcome. A failed transaction must be recovered by a new boot.
        words=nullptr;OSSafeReleaseNULL(mapping);OSSafeReleaseNULL(memory);
    }
    bool openInherited() {
        PE_Video video={};
        if(PE_current_console(&video) || video.v_width!=1200 || video.v_height!=2608 ||
           video.v_depth!=32 || !video.v_baseAddr || video.v_rotate)return false;
        memory=IODeviceMemory::withRange(UINT64_C(0x9ac0000),4096);
        if(!memory)return false;
        mapping=memory->map(kIOMapInhibitCache);
        if(!mapping || mapping->getLength()!=4096 || mapping->getPhysicalAddress()!=UINT64_C(0x9ac0000)) {close();return false;}
        words=reinterpret_cast<volatile uint32_t *>(mapping->getVirtualAddress());
        uint32_t version=0,control=0,dma=0,status=0;
        if(!read(this,SM_DSI_HW,&version) || !read(this,SM_DSI_CTRL,&control) ||
           version!=UINT32_C(0x200a0000) || (control&5)!=5 || (control&2) ||
           !read(this,SM_DSI_DMA_CTRL,&dma) || !read(this,SM_DSI_STATUS,&status)) {close();return false;}
        printf("NEZHA DSI0: inherited version=%x ctrl=%x dma=%x status=%x;GOP active,single route,no clock writes\n",version,control,dma,status);
        // The firmware hands over a live sole DSI0 command engine. No other
        // selected XNU driver owns or changes these command/clock registers.
        proof.lease_epoch=1;proof.hardware_version=version;proof.control=control;
        proof.dma_control=dma;proof.route_qualified=true;proof.clocks_owned=true;
        proof.exclusive_command_owner=true;proof.use_lpm=false; // stock DT dsi_hs_mode
        transfer.io={this,read,write,barrier,delay};return true;
    }
};
