// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "sensors-hid-host-shim.h"
using vm_size_t=size_t;
using event_t=void *;
inline uint64_t sensors_host_counter=19200000,sensors_host_frequency=19200000;
inline uint64_t sensors_host_us=1000000,sensors_host_ticks=19200000;
inline unsigned sensors_host_selected=1,sensors_host_wakes=0,sensors_host_acks=0;
inline std::function<void()> sensors_host_worker;
inline bool PE_parse_boot_argn(const char *,void *p,size_t){*static_cast<unsigned *>(p)=sensors_host_selected;return true;}
inline uint64_t na_native_now_us(void *){return sensors_host_us;}
inline uint64_t na_native_host_time(void *){return sensors_host_ticks;}
inline void thread_wakeup(event_t){sensors_host_wakes++;}
inline void IOSleep(unsigned ms){sensors_host_us+=ms*1000;if(sensors_host_worker)sensors_host_worker();}
struct IONotifier {};
using IOServiceInterestHandler=IOReturn (*)(void *,void *,UInt32,IOService *,void *,vm_size_t);
inline IOServiceInterestHandler sensors_host_power;
inline IONotifier *registerPrioritySleepWakeInterest(IOServiceInterestHandler h,void *){sensors_host_power=h;static IONotifier n;return &n;}
inline IOReturn acknowledgeSleepWakeNotification(void *){sensors_host_acks++;return 0;}
constexpr UInt32 kIOMessageCanSystemSleep=1,kIOMessageSystemWillSleep=2,kIOMessageSystemHasPoweredOn=3;
