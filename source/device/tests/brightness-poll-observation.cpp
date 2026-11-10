// SPDX-License-Identifier: BSD-2-Clause-Patent
// Exercise the selected real FIFO state machine, with and without observation.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "NezhaDsiObservation.h"
struct Operation {
    char kind;
    uint32_t offset,value;
    bool success;
    bool operator==(const Operation&) const=default;
};
struct FakeIo {
    uint32_t regs[256]={};
    uint32_t fail_read=UINT32_MAX;
    bool observed=false;
    std::vector<Operation> operations;
    static bool read(void *cookie,uint32_t offset,uint32_t *value) {
        auto self=static_cast<FakeIo *>(cookie);
        if(self->fail_read==offset) {
            self->operations.push_back({'r',offset,0,false});return false;
        }
        *value=self->regs[offset/4];self->operations.push_back({'r',offset,*value,true});
        if(self->observed)nezha_dsi_observation_read(cookie,offset,*value);
        return true;
    }
    static bool write(void *cookie,uint32_t offset,uint32_t value) {
        auto self=static_cast<FakeIo *>(cookie);
        self->operations.push_back({'w',offset,value,true});
        if(offset==SM_DSI_INT) {
            const uint32_t pending=self->regs[offset/4]&UINT32_C(0x55555555);
            self->regs[offset/4]=(pending&~value)|(value&UINT32_C(0xaaaaaaaa));
        } else self->regs[offset/4]=value;
        return true;
    }
    static void barrier(void *cookie) {
        static_cast<FakeIo *>(cookie)->operations.push_back({'b',0,0,true});
    }
    static void delay(void *cookie) {
        static_cast<FakeIo *>(cookie)->operations.push_back({'d',0,1,true});
    }
};
struct Poll {
    uint64_t now;
    uint32_t ints,status,fail=UINT32_MAX;
    uint64_t lease=1;
};
struct Trial {
    std::vector<Operation> operations;
    std::vector<int> results;
    struct nezha_dsi_observation record={};
    uint32_t regs[256]={};
    bool active,poisoned,prepared;
};
static Trial run(const std::vector<Poll>&polls,bool observed) {
    FakeIo io;io.observed=observed;
    io.regs[SM_DSI_HW/4]=UINT32_C(0x200a0000);io.regs[SM_DSI_CTRL/4]=UINT32_C(0x1f5);
    io.regs[SM_DSI_DMA_CTRL/4]=UINT32_C(0x14000000);io.regs[SM_DSI_INT/4]=UINT32_C(0xa000);
    io.regs[SM_DSI_LENGTH/4]=123;
    struct sm_dsi_transfer transfer={};transfer.io={&io,FakeIo::read,FakeIo::write,FakeIo::barrier,FakeIo::delay};
    const struct sm_dsi_proof proof={1,UINT32_C(0x200a0000),UINT32_C(0x1f5),UINT32_C(0x14000000),true,true,true,false};
    const uint8_t packet[8]={3,0,0x39,0xc0,0x51,7,122,0xff};
    if(observed)nezha_dsi_observation_record={};
    assert(sm_dsi_begin(&transfer,&proof,packet,1000)==SM_DSI_SENT);
    if(observed) {
        // Preflight and program reads were outside poll-only guard.
        assert(!nezha_dsi_observation_record.polls&&!nezha_dsi_observation_record.last_valid);
        nezha_dsi_observation_begin(&io,95,76,1914,1895,&transfer);
    }
    Trial trial;
    for(const auto&poll:polls) {
        io.regs[SM_DSI_INT/4]=poll.ints;io.regs[SM_DSI_STATUS/4]=poll.status;io.fail_read=poll.fail;
        if(observed)nezha_dsi_observation_poll_begin(&io,95,poll.now,1895);
        const int result=sm_dsi_poll(&transfer,poll.lease,poll.now);
        if(observed)nezha_dsi_observation_poll_end(&io,95,result,&transfer);
        trial.results.push_back(result);
        if(result<0 || result==SM_DSI_COMPLETE)break;
    }
    if(observed) {
        trial.record=nezha_dsi_observation_record;
        const size_t io_count=io.operations.size();struct nezha_dsi_observation dump;
        FakeIo other;
        assert(!nezha_dsi_observation_fault(&other,95,trial.results.back(),&dump));
        assert(!nezha_dsi_observation_fault(&io,96,trial.results.back(),&dump));
        bool dumped=nezha_dsi_observation_fault(&io,95,trial.results.back(),&dump);
        assert(dumped==(trial.results.back()==SM_DSI_TIMEOUT));
        if(dumped) {
            assert(dump.generation==95&&dump.request==76&&dump.sending==1914&&dump.acknowledged==1895);
            assert(dump.polls==polls.size()&&!dump.polling&&!dump.active&&dump.poisoned&&!dump.prepared);
        }
        assert(!nezha_dsi_observation_fault(&io,95,trial.results.back(),&dump));
        assert(io.operations.size()==io_count); // Copying/dumping consumes no IO.
        auto before=nezha_dsi_observation_record;
        nezha_dsi_observation_read(&io,SM_DSI_INT,UINT32_MAX);
        nezha_dsi_observation_read(&io,SM_DSI_STATUS,UINT32_MAX);
        assert(!memcmp(&before,&nezha_dsi_observation_record,sizeof(before))); // Poll guard closed.
    }
    trial.operations=io.operations;memcpy(trial.regs,io.regs,sizeof(io.regs));
    trial.active=transfer.active;trial.poisoned=transfer.poisoned;trial.prepared=transfer.prepared;
    return trial;
}
static Trial compare(const std::vector<Poll>&polls) {
    Trial observed=run(polls,true),baseline=run(polls,false);
    assert(observed.operations==baseline.operations&&observed.results==baseline.results);
    assert(!memcmp(observed.regs,baseline.regs,sizeof(observed.regs)));
    assert(observed.active==baseline.active&&observed.poisoned==baseline.poisoned&&observed.prepared==baseline.prepared);
    return observed;
}
int main() {
    unsigned scenarios=0;
    // Real completion triggers real cleanup, including its second INT read.
    auto complete=compare({{1100,UINT32_C(0x4000a001),0}});
    assert(complete.results.back()==SM_DSI_COMPLETE&&complete.record.polls==1);
    assert(complete.record.first_valid==3&&complete.record.last_valid==3);
    assert(complete.record.last_ints==UINT32_C(0x4000a001)&&complete.record.last_status==0);
    assert(complete.regs[SM_DSI_INT/4]==UINT32_C(0x4000a000)); // Cleanup did not overwrite observed done bit.
    ++scenarios;
    auto timeout=compare({{1100,0xa000,2},{1200,0xa020,2},{2500,0xa040,2},{6000,0xa080,2}});
    assert((timeout.results==std::vector<int>{SM_DSI_WAIT,SM_DSI_WAIT,SM_DSI_WAIT,SM_DSI_TIMEOUT}));
    assert(timeout.record.polls==4&&timeout.record.max_gap_us==3500&&timeout.record.began_us==1000);
    assert(timeout.record.first_poll_us==1100&&timeout.record.last_poll_us==6000);
    assert(timeout.record.first_ints==0xa000&&timeout.record.last_ints==0xa080&&timeout.record.last_status==2);
    ++scenarios;
    auto delayed=compare({{7000,0xa000,2}});
    assert(delayed.results.back()==SM_DSI_TIMEOUT&&delayed.record.max_gap_us==6000&&delayed.record.polls==1);
    ++scenarios;
    auto boundary=compare({{5999,0xa000,2},{6000,0xa001,0}});
    assert((boundary.results==std::vector<int>{SM_DSI_WAIT,SM_DSI_COMPLETE})); // Completion still precedes timeout check.
    ++scenarios;
    auto int_fail=compare({{1100,0xa000,2,SM_DSI_INT}});
    assert(int_fail.results.back()==SM_DSI_UNCERTAIN&&int_fail.record.last_valid==0);
    ++scenarios;
    auto status_fail=compare({{1100,0xa020,2,SM_DSI_STATUS}});
    assert(status_fail.results.back()==SM_DSI_UNCERTAIN&&status_fail.record.last_valid==1&&status_fail.record.last_ints==0xa020);
    ++scenarios;
    auto int_error=compare({{1100,0x0100a000,2}});
    assert(int_error.results.back()==SM_DSI_UNCERTAIN&&int_error.record.last_valid==3);
    ++scenarios;
    auto status_error=compare({{1100,0xa000,0x80000002}});
    assert(status_error.results.back()==SM_DSI_UNCERTAIN&&status_error.record.last_valid==3);
    ++scenarios;
    auto lease_lost=compare({{1100,0xa000,2,UINT32_MAX,2}});
    assert(lease_lost.results.back()==SM_DSI_UNCERTAIN&&lease_lost.record.last_valid==0);
    ++scenarios;
    // Independently vary owner/generation and out-of-poll accesses.
    FakeIo owner,other;struct sm_dsi_transfer transfer={};transfer.lease_epoch=1;transfer.began_us=1000;
    nezha_dsi_observation_begin(&owner,10,7,8,9,&transfer);
    auto before=nezha_dsi_observation_record;
    nezha_dsi_observation_read(&owner,SM_DSI_INT,1);
    nezha_dsi_observation_poll_begin(&other,10,1100,9);
    nezha_dsi_observation_read(&other,SM_DSI_STATUS,2);
    nezha_dsi_observation_poll_begin(&owner,11,1100,9);
    assert(!memcmp(&before,&nezha_dsi_observation_record,sizeof(before)));
    ++scenarios;
    nezha_dsi_observation_poll_begin(&owner,10,1100,9);
    nezha_dsi_observation_read(&other,SM_DSI_INT,1);
    nezha_dsi_observation_read(&owner,SM_DSI_DMA_CTRL,123);
    assert(nezha_dsi_observation_record.last_valid==0);
    nezha_dsi_observation_read(&owner,SM_DSI_INT,1);
    nezha_dsi_observation_read(&owner,SM_DSI_INT,0); // First successful sample wins.
    nezha_dsi_observation_read(&owner,SM_DSI_STATUS,2);
    nezha_dsi_observation_poll_end(&owner,10,SM_DSI_WAIT,&transfer);
    assert(nezha_dsi_observation_record.last_valid==3&&nezha_dsi_observation_record.last_ints==1);
    ++scenarios;
    nezha_dsi_observation_begin(&owner,11,8,9,10,&transfer);
    assert(nezha_dsi_observation_record.generation==11&&!nezha_dsi_observation_record.polls&&!nezha_dsi_observation_record.first_valid);
    nezha_dsi_observation_poll_begin(&owner,10,1200,10);
    assert(!nezha_dsi_observation_record.polls&&!nezha_dsi_observation_record.polling);
    ++scenarios;
    std::printf("PASS %u selected-FIFO observation scenarios;exact IO and result sequences retained\n",scenarios);
}
