# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Add the nezha power backend to the fixed GOP source, keeping its GPU path."""

POWER_MEMBERS = r'''
    IOTimerEventSource *displayPowerTimer=nullptr;
    uint64_t displayPowerSequence=0,displayPowerDeadline=0;
    uint32_t displayPowerTarget=1,displayPowerConfirmed=2,displayPowerIssued=2;
    bool displayPowerSupported=false,displayPowerAwaiting=false;
    uint32_t displayPowerTransitioning=0;
    void stopDisplayPower() {
        setOriginalPowerRequestsEnabled(false);
        __atomic_store_n(&displayPowerConfirmed,2u,__ATOMIC_RELEASE);
        if(displayPowerTimer) {
            displayPowerTimer->cancelTimeout();
            if(IOWorkLoop *loop=getWorkLoop())loop->removeEventSource(displayPowerTimer);
            OSSafeReleaseNULL(displayPowerTimer);
        }
        displayPowerSupported=false;displayPowerAwaiting=false;
        __atomic_store_n(&displayPowerTransitioning,0u,__ATOMIC_RELEASE);
    }
    static void displayPowerFired(OSObject *owner,IOTimerEventSource *) {
        static_cast<NativeLegacyDisplay *>(owner)->serviceDisplayPower();
    }
    uint64_t displayPowerNow() const {
        uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);return ns/1000;
    }
    void finishDisplayPower(IOReturn status) {
        if(!displayPowerAwaiting)return;
        displayPowerAwaiting=false;
        if(status==kIOReturnSuccess) {
            // Original PM155 uses the same powered-only request shape. The
            // backend applies it only after the real DCS command completes.
            OriginalLegacyStateRequest request={1,0,0,displayPowerTarget,1,{}};
            status=applyStateRequest(&request,false);
            if(status==kIOReturnSuccess && originalPoweredState()==displayPowerTarget) {
                __atomic_store_n(&displayPowerConfirmed,displayPowerTarget,__ATOMIC_RELEASE);
                if(displayPowerTarget)armVsync();
                else if(vsyncTimer){vsyncTimer->cancelTimeout();vsyncArmed=false;}
            } else if(status==kIOReturnSuccess)status=kIOReturnNotReady;
        }
        if(status!=kIOReturnSuccess)__atomic_store_n(&displayPowerConfirmed,2u,__ATOMIC_RELEASE);
        __atomic_store_n(&displayPowerTransitioning,0u,__ATOMIC_RELEASE);
        native_legacy_pm_observed(status==kIOReturnSuccess?5:6,status,displayPowerTarget);
        IOLog("NEZHA DISPLAY POWER: target=%u result=%x sequence=%llu requested=%u powered=%u\n",
            displayPowerTarget,(unsigned)status,displayPowerSequence,originalRequestedPower(),originalPoweredState());
        // IOKit has no failed hardware-ACK encoding. Finish the PM call but
        // preserve the last hardware state and withdraw screen validity on error.
        acknowledgeSetPowerState();
    }
    void serviceDisplayPower() {
        if(!logicalPM || !displayPowerSupported || !displayPowerAwaiting)return;
        if(displayPowerNow()>=displayPowerDeadline){finishDisplayPower(kIOReturnTimeout);return;}
        if(!displayPowerSequence) {
            if(nezha_backlight_display_power_request(displayPowerTarget,&displayPowerSequence)) {
                displayPowerTimer->setTimeoutMS(10);return;
            }
        }
        uint32_t on=2;
        const int status=nezha_backlight_display_power_status(displayPowerSequence,&on);
        if(status<0){finishDisplayPower(kIOReturnNotReady);return;}
        if(!status){finishDisplayPower(on==displayPowerTarget?kIOReturnSuccess:kIOReturnNotReady);return;}
        displayPowerTimer->setTimeoutMS(1);
    }
    IOReturn beginDisplayPower(unsigned long ordinal) {
        if(ordinal>1 || !bootMapping || !displayPowerTimer)return kIOReturnUnsupported;
        if(displayPowerAwaiting)return kIOReturnBusy;
        displayPowerTarget=(uint32_t)ordinal;displayPowerSequence=0;
        displayPowerAwaiting=true;
        __atomic_store_n(&displayPowerTransitioning,1u,__ATOMIC_RELEASE);
        // Reuse the backlight's existing bounded startup availability interval.
        displayPowerDeadline=displayPowerNow()+90000000;
        native_legacy_pm_observed(3,kIOReturnSuccess,ordinal);
        serviceDisplayPower();
        return displayPowerAwaiting?90000000:kIOPMAckImplied;
    }
'''

POWER_PUBLIC = r'''
    IOReturn stateChanged() override {
        if(!logicalPM || !displayPowerSupported)return IOMobileFramebufferLegacy::stateChanged();
        const uint32_t desired=originalRequestedPower();
        if(desired>1)return kIOReturnUnsupported;
        if(displayPowerIssued==desired)return kIOReturnSuccess;
        IOReturn status=changePowerStateToPriv(desired);
        if(status==kIOReturnSuccess)displayPowerIssued=desired;
        else {
            __atomic_store_n(&displayPowerConfirmed,2u,__ATOMIC_RELEASE);
            native_legacy_pm_observed(6,status,desired);
        }
        return status;
    }
    int copyScreenState(uint32_t *out) const {
        if(!out || __atomic_load_n(&displayPowerTransitioning,__ATOMIC_ACQUIRE))return -1;
        const uint32_t completed=__atomic_load_n(&displayPowerConfirmed,__ATOMIC_ACQUIRE);
        if(completed>1)return -1;
        const unsigned char *object=reinterpret_cast<const unsigned char *>(this);
        if(!(__atomic_load_n(object+140,__ATOMIC_ACQUIRE)&1) ||
           __atomic_load_n(reinterpret_cast<const uint32_t *>(object+7200),__ATOMIC_ACQUIRE) ||
           originalRequestedPower()!=completed || originalPoweredState()!=completed)return -1;
        uint32_t brightness=2;
        if(nezha_backlight_brightness_state(&brightness) || (!completed && brightness))return -1;
        if(__atomic_load_n(&displayPowerTransitioning,__ATOMIC_ACQUIRE) ||
           __atomic_load_n(&displayPowerConfirmed,__ATOMIC_ACQUIRE)!=completed ||
           originalRequestedPower()!=completed || originalPoweredState()!=completed)return -1;
        *out=completed;return 0;
    }
'''

POWER_SETUP = r'''
        unsigned backlightEnabled=0;PE_parse_boot_argn("nezha-backlight",&backlightEnabled,sizeof(backlightEnabled));
        if(backlightEnabled==1) {
            IOWorkLoop *loop=getWorkLoop();
            displayPowerTimer=IOTimerEventSource::timerEventSource(this,&NativeLegacyDisplay::displayPowerFired);
            if(!loop || !displayPowerTimer || loop->addEventSource(displayPowerTimer)!=kIOReturnSuccess) {
                OSSafeReleaseNULL(displayPowerTimer);IOMobileFramebufferLegacy::stop(provider);return false;
            }
            displayPowerSupported=true;
            // Original Apple platform setup enables request12 before PMinit.
            setOriginalPowerRequestsEnabled(true);
        }
'''

POWER_SETTER = r'''
    IOReturn setPowerState(unsigned long ordinal,IOService *device) override {
        if(!logicalPM || !displayPowerSupported)return IOMobileFramebufferLegacy::setPowerState(ordinal,device);
        if(ordinal>1 || !getWorkLoop())return kIOReturnNotReady;
        struct Args {NativeLegacyDisplay *display;unsigned long ordinal;} args={this,ordinal};
        return getWorkLoop()->runAction([](OSObject *,void *arg,void *,void *,void *)->IOReturn {
            auto a=static_cast<Args *>(arg);return a->display->beginDisplayPower(a->ordinal);
        },this,&args);
    }
'''

POWER_EXPORT = r'''
extern "C" int nezha_display_screen_state(uint32_t *out) {
    if(!out)return -1;
    OSDictionary *matching=IOService::serviceMatching("NativeLegacyDisplay");
    IOService *service=matching?IOService::copyMatchingService(matching):nullptr;
    OSSafeReleaseNULL(matching);
    auto display=OSDynamicCast(NativeLegacyDisplay,service);
    const int status=display?display->copyScreenState(out):-1;
    OSSafeReleaseNULL(service);return status;
}
'''


def replace_one(text, old, new):
    if text.count(old) != 1:
        raise ValueError('Fixed GOP source identity differs at: ' + old[:96])
    return text.replace(old, new)


def adapt_power099(text):
    text = replace_one(text, '    bool logicalPM=false;', '    bool logicalPM=false;\n' + POWER_MEMBERS)
    text = replace_one(text, 'extern "C" void native_legacy_vsync_observed(unsigned event,unsigned hz);',
                       'extern "C" void native_legacy_vsync_observed(unsigned event,unsigned hz);\n'
                       'extern "C" int nezha_backlight_display_power_request(uint32_t,uint64_t *);\n'
                       'extern "C" int nezha_backlight_display_power_status(uint64_t,uint32_t *);\n'
                       'extern "C" int nezha_backlight_brightness_state(uint32_t *);')
    text = replace_one(text, '    void free() override {stopVsync();if(logicalPM)',
                       '    void free() override {stopDisplayPower();stopVsync();if(logicalPM)')
    text = replace_one(text, '    void stop(IOService *provider) override {\n        stopVsync();',
                       '    void stop(IOService *provider) override {\n        stopDisplayPower();stopVsync();')
    text = replace_one(text, '        PMinit();logicalPM=initialized;', POWER_SETUP + '\n        PMinit();logicalPM=initialized;')
    text = replace_one(text, 'if(!logicalPM){native_legacy_pm_observed(1,kIOReturnNotReady,0);',
                       'if(!logicalPM){stopDisplayPower();native_legacy_pm_observed(1,kIOReturnNotReady,0);')
    text = replace_one(text, 'if(status!=kIOReturnSuccess){PMstop();logicalPM=false;',
                       'if(status!=kIOReturnSuccess){stopDisplayPower();PMstop();logicalPM=false;')
    start = text.index('    IOReturn setPowerState(unsigned long ordinal,IOService *device) override {')
    end = text.index('\n    // The explicit offline system-client experiment', start)
    text = text[:start] + POWER_PUBLIC + POWER_SETTER + text[end:]
    text = replace_one(text, '    bool vsyncWanted() const {\n        const void *head=',
                       '    bool vsyncWanted() const {\n        if(displayPowerSupported && !originalPoweredState())return false;\n        const void *head=')
    text = replace_one(text, 'OSDefineMetaClassAndStructors(NativeLegacyDisplay,IOMobileFramebufferLegacy)',
                       'OSDefineMetaClassAndStructors(NativeLegacyDisplay,IOMobileFramebufferLegacy)\n' + POWER_EXPORT)
    return text
