// Compiled for MSVC ABI independently of the MinGW C implementation.
// Interface declarations follow DBCTRADO/LibISDB Windows/Base/BonDriver.hpp.
using BOOL=int; using BYTE=unsigned char; using DWORD=unsigned long;
class IBonDriver {
public:
    virtual const BOOL OpenTuner()=0;
    virtual void CloseTuner()=0;
    virtual const BOOL SetChannel(const BYTE)=0;
    virtual const float GetSignalLevel()=0;
    virtual const DWORD WaitTsStream(const DWORD)=0;
    virtual const DWORD GetReadyCount()=0;
    virtual const BOOL GetTsStream(BYTE *,DWORD *,DWORD *)=0;
    virtual const BOOL GetTsStream(BYTE **,DWORD *,DWORD *)=0;
    virtual void PurgeTsStream()=0;
    virtual void Release()=0;
};
class IBonDriver2:public IBonDriver {
public:
    virtual const wchar_t *GetTunerName()=0;
    virtual const BOOL IsTunerOpening()=0;
    virtual const wchar_t *EnumTuningSpace(const DWORD)=0;
    virtual const wchar_t *EnumChannelName(const DWORD,const DWORD)=0;
    virtual const BOOL SetChannel(const DWORD,const DWORD)=0;
    virtual const DWORD GetCurSpace()=0;
    virtual const DWORD GetCurChannel()=0;
    virtual void Release() override=0;
};
// Type descriptor vftables are unused by dynamic_cast (there is no typeid()).
extern "C" { void *type_info_vtable[2] __asm__("??_7type_info@@6B@")={}; }
extern "C" int bon_msvc_consumer(void *opaque)
{
    auto base=static_cast<IBonDriver *>(opaque);
    auto driver=dynamic_cast<IBonDriver2 *>(base);
    if(!driver) return 1;
    if(driver->GetCurSpace()!=0xffffffff || driver->GetCurChannel()!=0xffffffff ||
       driver->IsTunerOpening() || !driver->GetTunerName() ||
       !driver->EnumTuningSpace(0) || !driver->EnumChannelName(0,39) ||
       driver->EnumChannelName(0,40)) return 2;
    BYTE *data=nullptr; DWORD size=99,remain=99;
    if(driver->GetTsStream(&data,&size,&remain) || !data || size || remain) return 3;
    static BYTE buffer[256*188]; size=99; remain=99;
    if(driver->GetTsStream(buffer,&size,&remain) || size || remain) return 4;
    if(driver->SetChannel(0,0) || base->SetChannel(13) || base->GetSignalLevel()!=0.0f ||
       base->GetReadyCount()!=0 || base->WaitTsStream(0)!=0x80) return 5;
    driver->PurgeTsStream(); driver->CloseTuner();
    return 0;
}
