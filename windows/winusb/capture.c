#include "client.h"
#include "binary_file.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
static volatile LONG cancelled;
static HANDLE cancel_event;
static BOOL WINAPI console(DWORD event)
{
    if(event==CTRL_C_EVENT || event==CTRL_BREAK_EVENT) {
        InterlockedExchange(&cancelled,1);
        if(cancel_event) SetEvent(cancel_event);
        return TRUE;
    }
    return FALSE;
}
int wmain(int argc,wchar_t **argv)
{
    if(argc!=7) {
        fwprintf(stderr,L"usage: xz-capture.exe firmware.bin gr|bscs frequency seconds tsid|auto output.ts\n"
                         L"frequency: GR Hz, BS/CS LNB IF kHz; Ctrl+C stops; idle DriverHost restores\n");
        return 2;
    }
    wchar_t *end;
    struct mb86H_dvb_request request={.stream_id=UINT32_MAX,.layers=7};
    if(!wcscmp(argv[2],L"gr")) { request.system=MB86H_DVB_GR; request.bandwidth_hz=6000000; }
    else if(!wcscmp(argv[2],L"bscs")) request.system=MB86H_DVB_BSCS;
    else return 2;
    request.frequency=wcstoul(argv[3],&end,10); if(!*argv[3] || *end) return 2;
    DWORD seconds=wcstoul(argv[4],&end,10); if(!*argv[4] || *end || !seconds || seconds>86400) return 2;
    if(_wcsicmp(argv[5],L"auto")) {
        request.stream_id=wcstoul(argv[5],&end,0);
        if(!*argv[5] || *end || request.stream_id>0xffff) return 2;
    }
    if(!mb86H_dvb_request_valid(&mb86H_frontend_caps[request.system],&request)) return 2;
    if(wcslen(argv[6])>32000) return 2;
    wchar_t exe[32768],firmware[32768],log[32768];
    DWORD n=GetModuleFileNameW(NULL,exe,32768);
    wchar_t *slash=wcsrchr(exe,L'\\');
    if(!n || n>=32600 || !slash) return 2;
    wcscpy(slash+1,L"DriverHost_mb86H.exe");
    if(!GetFullPathNameW(argv[1],32768,firmware,NULL)) return 2;
    swprintf(log,32768,L"%ls.log",argv[6]);
    FILE *output=xz_create_binary_file(argv[6]);
    if(!output) { fwprintf(stderr,L"cannot create %ls errno=%d (output must not exist)\n",argv[6],errno); return 1; }
    cancel_event=CreateEventW(NULL,TRUE,FALSE,NULL);
    if(!cancel_event) { fclose(output); return 1; }
    SetConsoleCtrlHandler(console,TRUE);
    struct xz_client *host=NULL;
    int rc=xz_client_open(&host,exe,firmware,log,cancel_event);
    if(!rc && !InterlockedCompareExchange(&cancelled,0,0)) rc=xz_client_tune(host,&request);
    ULONGLONG start=0,total=0,deadline=GetTickCount64()+XZ_IPC_OPEN_MS+XZ_IPC_TUNE_MS;
    BYTE data[256*188];
    while(!rc && !InterlockedCompareExchange(&cancelled,0,0) &&
          (!start || GetTickCount64()-start<(ULONGLONG)seconds*1000)) {
        struct xz_ipc_state state;
        rc=xz_client_state(host,&state);
        if(!rc && state.error) rc=state.error;
        if(rc) break;
        if(!start && state.phase==XZ_IPC_STREAMING) start=GetTickCount64();
        if(!start && GetTickCount64()>=deadline) { rc=-ETIMEDOUT; break; }
        DWORD result=xz_client_wait(host,100);
        if(result==WAIT_TIMEOUT) { Sleep(10); continue; }
        if(result!=WAIT_OBJECT_0) { rc=-1; break; }
        DWORD bytes=0,remain=0;
        rc=xz_client_read(host,data,sizeof(data),&bytes,&remain);
        if(!rc && fwrite(data,1,bytes,output)!=bytes) rc=-1;
        total+=bytes;
    }
    if(fflush(output)) rc=-1;
    if(fclose(output)) rc=-1;
    int closed=xz_client_close(&host);
    SetConsoleCtrlHandler(console,FALSE);
    CloseHandle(cancel_event); cancel_event=NULL;
    fwprintf(stderr,L"capture rc=%d close=%d TS bytes=%llu log=%ls (device progress/snapshot: DriverHost directory)\n",rc,closed,total,log);
    return rc || closed || !total ? 1 : 0;
}
