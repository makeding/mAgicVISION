#include "bon_abi.h"
#include "client.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define CHANNEL_LIMIT 256
#define TS_CHUNK (256*188)
struct channel { wchar_t name[64]; struct mb86H_dvb_request request; };
struct bon {
    const struct bon_vtable *vtable;
    CRITICAL_SECTION api;
    struct xz_client *host;
    HANDLE cancel;
    wchar_t ini[32768],directory[32768];
    struct channel channels[2][CHANNEL_LIMIT];
    DWORD counts[2],space,channel;
    BYTE ts[TS_CHUNK];
};
static HMODULE module;
static struct bon *instance;
static SRWLOCK instance_lock=SRWLOCK_INIT;
static BOOL config(struct bon *b)
{
    DWORD n=GetModuleFileNameW(module,b->directory,32768);
    if(!n || n>=32760) return FALSE;
    wchar_t *slash=wcsrchr(b->directory,L'\\');
    if(!slash) return FALSE;
    wcscpy(b->ini,b->directory);
    wchar_t *dot=wcsrchr(b->ini,L'.');
    if(!dot) return FALSE;
    wcscpy(dot,L".ini"); slash[1]=0;
    b->counts[0]=40;
    for(DWORD i=0;i<40;++i) {
        struct channel *c=&b->channels[0][i];
        swprintf(c->name,64,L"UHF %lu",i+13);
        c->request=(struct mb86H_dvb_request){.system=MB86H_DVB_GR,
            .frequency=473142857+i*6000000,.bandwidth_hz=6000000,
            .stream_id=UINT32_MAX,.layers=7};
    }
    DWORD count=GetPrivateProfileIntW(L"Satellite",L"Count",0,b->ini);
    if(count>CHANNEL_LIMIT) return FALSE;
    for(DWORD i=0;i<count;++i) {
        wchar_t section[32],tsid[32],*end;
        swprintf(section,32,L"Satellite%lu",i);
        struct channel *c=&b->channels[1][i];
        GetPrivateProfileStringW(section,L"Name",L"",c->name,64,b->ini);
        GetPrivateProfileStringW(section,L"TSID",L"auto",tsid,32,b->ini);
        unsigned long id=UINT32_MAX;
        if(_wcsicmp(tsid,L"auto")) {
            id=wcstoul(tsid,&end,0);
            if(!*tsid || *end || id>0xffff) return FALSE;
        }
        c->request=(struct mb86H_dvb_request){.system=MB86H_DVB_BSCS,
            .frequency=GetPrivateProfileIntW(section,L"FrequencyKHz",0,b->ini),
            .stream_id=(uint32_t)id};
        if(!*c->name || !mb86H_dvb_request_valid(&mb86H_frontend_caps[1],&c->request)) return FALSE;
    }
    b->counts[1]=count;
    return TRUE;
}
static BOOL open_tuner(struct bon *b)
{
    EnterCriticalSection(&b->api);
    if(b->host) { LeaveCriticalSection(&b->api); return TRUE; }
    wchar_t name[32768],firmware[32768],exe[32768],log[32768];
    GetPrivateProfileStringW(L"Device",L"Firmware",L"firmware.bin",name,32768,b->ini);
    if(wcslen(b->directory)+wcslen(name)>32600) { LeaveCriticalSection(&b->api); return FALSE; }
    if(name[0]==L'\\' || (wcslen(name)>1 && name[1]==L':')) wcscpy(firmware,name);
    else swprintf(firmware,32768,L"%ls%ls",b->directory,name);
    DWORD full=GetFullPathNameW(firmware,32768,name,NULL);
    if(!full || full>=32768) { LeaveCriticalSection(&b->api); return FALSE; }
    wcscpy(firmware,name);
    swprintf(exe,32768,L"%lsDriverHost_mb86H.exe",b->directory);
    swprintf(log,32768,L"%lsBonDriver_mb86H.log",b->directory);
    ResetEvent(b->cancel);
    int rc=xz_client_open(&b->host,exe,firmware,log,b->cancel);
    if(rc) xz_client_close(&b->host);
    LeaveCriticalSection(&b->api); return rc==0;
}
static void lifecycle_log(struct bon *b,const char *action)
{
    wchar_t path[32768];
    swprintf(path,32768,L"%lsBonDriver_mb86H.log",b->directory);
    FILE *f=_wfopen(path,L"a");
    if(!f) return;
    SYSTEMTIME t; GetSystemTime(&t);
    fprintf(f,"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ TVTest %s space=%lu channel=%lu thread=%lu\n",
        t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds,
        action,b->space,b->channel,GetCurrentThreadId());
    fclose(f);
}
static void close_tuner(struct bon *b)
{
    EnterCriticalSection(&b->api);
    if(b->host) lifecycle_log(b,"CloseTuner invoked");
    xz_client_close(&b->host);
    b->space=b->channel=UINT32_MAX;
    LeaveCriticalSection(&b->api);
}
static BOOL set_channel(struct bon *b,DWORD space,DWORD channel)
{
    if(space>1 || channel>=b->counts[space]) return FALSE;
    EnterCriticalSection(&b->api);
    if(!b->host) { LeaveCriticalSection(&b->api); return FALSE; }
    wchar_t log[32768];
    swprintf(log,32768,L"%lsBonDriver_mb86H.log",b->directory);
    FILE *f=_wfopen(log,L"a");
    if(f) {
        char title[256];
        WideCharToMultiByte(CP_UTF8,0,b->channels[space][channel].name,-1,title,sizeof(title),NULL,NULL);
        SYSTEMTIME t; GetSystemTime(&t);
        fprintf(f,"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ SetChannel selected=%s space=%lu channel=%lu system=%u frequency=%u tsid=%u\n",
            t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds,
            title,space,channel,
            b->channels[space][channel].request.system,
            b->channels[space][channel].request.frequency,
            b->channels[space][channel].request.stream_id);
        fclose(f);
    }
    int rc=xz_client_tune(b->host,&b->channels[space][channel].request);
    if(!rc) { b->space=space; b->channel=channel; }
    LeaveCriticalSection(&b->api); return rc==0;
}
static BOOL channel_byte(struct bon *b,BYTE channel)
{ return channel>=13 ? set_channel(b,0,channel-13) : FALSE; }
static float signal_level(struct bon *b)
{ (void)b; return 0.0f; /* No calibrated C/N API in this first adapter. */ }
static DWORD wait_stream(struct bon *b,DWORD timeout)
{
    EnterCriticalSection(&b->api);
    /* Do not hold the API lock indefinitely: Close/SetChannel must retire RX. */
    DWORD result=xz_client_wait(b->host,timeout>100 ? 100 : timeout);
    LeaveCriticalSection(&b->api); return result;
}
static DWORD ready(struct bon *b)
{
    EnterCriticalSection(&b->api); DWORD count=xz_client_ready(b->host);
    LeaveCriticalSection(&b->api); return count;
}
static BOOL stream_copy(struct bon *b,BYTE *data,DWORD *size,DWORD *remain)
{
    if(!size || !data) return FALSE;
    EnterCriticalSection(&b->api);
    *size=0; if(remain) *remain=0;
    int rc=b->host ? xz_client_read(b->host,data,TS_CHUNK,size,remain) : -1;
    LeaveCriticalSection(&b->api); return rc==0;
}
static BOOL stream_pointer(struct bon *b,BYTE **data,DWORD *size,DWORD *remain)
{ if(!data) return FALSE; *data=b->ts; return stream_copy(b,b->ts,size,remain); }
static void purge(struct bon *b)
{ EnterCriticalSection(&b->api); xz_client_purge(b->host); LeaveCriticalSection(&b->api); }
static void release_bon(struct bon *b)
{
    AcquireSRWLockExclusive(&instance_lock);
    lifecycle_log(b,"Release invoked");
    close_tuner(b);
    if(instance==b) instance=NULL;
    CloseHandle(b->cancel); DeleteCriticalSection(&b->api); free(b);
    ReleaseSRWLockExclusive(&instance_lock);
}
static const wchar_t *name(struct bon *b) { (void)b; return L"I-O DATA GV-MVP/XZ ISDB-T/S Tuner"; }
static BOOL opening(struct bon *b) {
    EnterCriticalSection(&b->api);
    BOOL result=b->host!=NULL;
    LeaveCriticalSection(&b->api); return result;
}
static const wchar_t *space_name(struct bon *b,DWORD space)
{ return space<2 && b->counts[space] ? (space ? L"BS/CS" : L"ISDB-T") : NULL; }
static const wchar_t *channel_name(struct bon *b,DWORD space,DWORD channel)
{ return space<2 && channel<b->counts[space] ? b->channels[space][channel].name : NULL; }
static DWORD current_space(struct bon *b) { return b->space; }
static DWORD current_channel(struct bon *b) { return b->channel; }
static const struct bon_vtable methods={open_tuner,close_tuner,channel_byte,signal_level,
    wait_stream,ready,stream_pointer,stream_copy,purge,release_bon,name,opening,
    space_name,channel_name,set_channel,current_space,current_channel};
__declspec(dllexport) struct bon *CreateBonDriver(void)
{
    AcquireSRWLockExclusive(&instance_lock);
    if(!instance) {
        struct bon *b=calloc(1,sizeof(*b));
        if(b) {
            InitializeCriticalSection(&b->api); b->space=b->channel=UINT32_MAX;
            b->cancel=CreateEventW(NULL,TRUE,FALSE,NULL);
            b->vtable=bon_msvc_table(&methods);
            if(b->cancel && b->vtable && config(b)) instance=b;
            else { CloseHandle(b->cancel); DeleteCriticalSection(&b->api); free(b); }
        }
    }
    struct bon *result=instance;
    ReleaseSRWLockExclusive(&instance_lock); return result;
}
BOOL WINAPI DllMain(HINSTANCE dll,DWORD reason,void *reserved)
{
    (void)reserved;
    if(reason==DLL_PROCESS_ATTACH) { module=dll; DisableThreadLibraryCalls(dll); }
    /* The host calls Release before FreeLibrary. Never join under loader lock. */
    return TRUE;
}
