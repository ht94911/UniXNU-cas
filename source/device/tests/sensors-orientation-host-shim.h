// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "sensors-hid-host-shim.h"
#include <cstring>
using IOHIDReportType=uint32_t;
using SInt32=int32_t;
constexpr IOHIDReportType kIOHIDReportTypeInput=0,kIOHIDReportTypeFeature=2;
constexpr unsigned kIODirectionNone=0;
struct IOMemoryDescriptor : OSObject {
    std::vector<uint8_t> bytes;
    size_t getLength()const{return bytes.size();}
    size_t writeBytes(size_t at,const void *data,size_t size){
        if(at>bytes.size()||size>bytes.size()-at)return 0;
        memcpy(bytes.data()+at,data,size);return size;
    }
    size_t readBytes(size_t at,void *data,size_t size){
        if(at>bytes.size()||size>bytes.size()-at)return 0;
        memcpy(data,bytes.data()+at,size);return size;
    }
};
struct IOBufferMemoryDescriptor : IOMemoryDescriptor {
    static IOBufferMemoryDescriptor *withBytes(const void *p,size_t n,unsigned){
        auto m=new IOBufferMemoryDescriptor;m->bytes.assign(static_cast<const uint8_t *>(p),static_cast<const uint8_t *>(p)+n);return m;
    }
};
struct OSString : OSObject {static OSString *withCString(const char *){return new OSString;}};
struct HostOrientationReport {uint64_t timestamp;std::vector<uint8_t> bytes;};
inline std::vector<HostOrientationReport> hostOrientationReports;
inline int32_t hostOrientationReportFailure;
struct IOHIDDevice;
inline std::function<void(IOHIDDevice *)> hostOrientationPublishHook;
struct IOHIDDevice : IOService {
protected:
    virtual OSString *newTransportString()const{return nullptr;}
    virtual OSString *newProductString()const{return nullptr;}
public:
    virtual bool matchPropertyTable(OSDictionary *,SInt32 *){return true;}
    virtual IOReturn newReportDescriptor(IOMemoryDescriptor **out)const=0;
    virtual IOReturn handleReportWithTime(uint64_t t,IOMemoryDescriptor *r,IOHIDReportType,IOOptionBits){
        if(hostOrientationReportFailure)return hostOrientationReportFailure;
        hostOrientationReports.push_back({t,r->bytes});return kIOReturnSuccess;
    }
    virtual IOReturn getReport(IOMemoryDescriptor *,IOHIDReportType,IOOptionBits){return kIOReturnUnsupported;}
    virtual IOReturn setReport(IOMemoryDescriptor *,IOHIDReportType,IOOptionBits){return kIOReturnUnsupported;}
    bool start(IOService *p)override{provider=p;if(hostOrientationPublishHook)hostOrientationPublishHook(this);return true;}
};
