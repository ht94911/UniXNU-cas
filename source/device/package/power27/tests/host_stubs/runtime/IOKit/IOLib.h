#pragma once
#include <cstddef>
#include <cstdint>
#include <cassert>
#include <cstdio>
#include <cstring>
struct IOLock { bool held; };
inline IOLock *IOLockAlloc(){static IOLock pool[8]={};static unsigned n;assert(n<8);return &pool[n++];}
inline void IOLockLock(IOLock *p){assert(p&&!p->held);p->held=true;}
inline void IOLockUnlock(IOLock *p){assert(p&&p->held);p->held=false;}
inline bool IOLockTryLock(IOLock *p){if(!p||p->held)return false;p->held=true;return true;}
inline void IOLog(const char *,...){}
void IOSleep(unsigned);
