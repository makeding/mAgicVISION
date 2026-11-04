#include "client.h"
#include "ipc.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
struct xz_client {
    HANDLE control,data,cancel;
    FILE *log;
    CRITICAL_SECTION control_lock,data_lock;
    uint32_t control_sequence,data_sequence,session,generation;
    BOOL active,owns_cancel;
    volatile LONG broken;
};
static void report(struct xz_client *c,const char *where,int rc)
{
    DWORD error=rc?GetLastError():ERROR_SUCCESS; SYSTEMTIME t; GetSystemTime(&t);
    if(c->log) {
        fprintf(c->log,"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ DriverHost %s rc=%d win32=%lu\n",
            t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds,where,rc,error);
        fflush(c->log);
    }
}
static int call(struct xz_client *c,BOOL data,uint32_t command,const void *input,
               DWORD length,DWORD value,void *output,DWORD capacity,
               struct xz_ipc_packet *result,DWORD timeout,BOOL closing)
{
    if(!c || InterlockedCompareExchange(&c->broken,0,0)) return -EIO;
    CRITICAL_SECTION *lock=data?&c->data_lock:&c->control_lock;
    EnterCriticalSection(lock);
    struct xz_ipc_packet request={.magic=XZ_IPC_MAGIC,.version=XZ_IPC_VERSION,
        .command=command,.sequence=data?++c->data_sequence:++c->control_sequence,
        .session=c->session,.generation=c->generation,.length=length,.value=value};
    HANDLE pipe=data?c->data:c->control;
    HANDLE cancel=closing?NULL:c->cancel;
    ULONGLONG deadline=GetTickCount64()+timeout;
    int rc=xz_ipc_io(pipe,TRUE,&request,sizeof(request),deadline,cancel,NULL);
    if(!rc && length) rc=xz_ipc_io(pipe,TRUE,(void *)input,length,deadline,cancel,NULL);
    if(!rc) rc=xz_ipc_io(pipe,FALSE,result,sizeof(*result),deadline,cancel,NULL);
    if(!rc && (!xz_ipc_valid(result) || result->command!=command ||
       result->sequence!=request.sequence || result->length>capacity ||
       (command!=XZ_IPC_OPEN && result->session!=c->session))) rc=-EPROTO;
    if(!rc && result->length) rc=xz_ipc_io(pipe,FALSE,output,result->length,
                                          deadline,cancel,NULL);
    if(!rc && !data && ((command==XZ_IPC_OPEN && result->status) || command==XZ_IPC_CLOSE)) {
        struct xz_ipc_packet ack={.magic=XZ_IPC_MAGIC,.version=XZ_IPC_VERSION,
            .command=XZ_IPC_ACK,.sequence=result->sequence,.session=result->session};
        rc=xz_ipc_io(pipe,TRUE,&ack,sizeof(ack),deadline,NULL,NULL);
    }
    if(rc) {
        report(c,"IPC failed; connection fenced",rc);
        InterlockedExchange(&c->broken,1);
    } else { rc=result->status; SetLastError(ERROR_SUCCESS); }
    LeaveCriticalSection(lock);
    return rc;
}
static HANDLE connect_pipe(const wchar_t *name)
{
    return CreateFileW(name,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,
                        FILE_FLAG_OVERLAPPED,NULL);
}
int xz_client_open(struct xz_client **out,const wchar_t *exe,
                    const wchar_t *firmware,const wchar_t *log,HANDLE cancel)
{
    if(!out || !exe || !firmware || !log || wcslen(firmware)>=32768) return -EINVAL;
    struct xz_client *c=calloc(1,sizeof(*c));
    if(!c) return -ENOMEM;
    c->control=c->data=INVALID_HANDLE_VALUE;
    InitializeCriticalSection(&c->control_lock); InitializeCriticalSection(&c->data_lock);
    c->cancel=cancel?cancel:CreateEventW(NULL,TRUE,FALSE,NULL);
    c->owns_cancel=!cancel;
    c->log=_wfopen(log,L"a");
    *out=c;
    if(!c->cancel || !c->log) return -EIO;
    wchar_t control[128],data[128],mutex[128];
    int rc=xz_ipc_names(control,data,mutex);
    if(rc) return rc;
    c->control=connect_pipe(control);
    if(c->control==INVALID_HANDLE_VALUE) {
        if(GetLastError()!=ERROR_FILE_NOT_FOUND) {
            report(c,"connect control", -EBUSY); return -EBUSY;
        }
        STARTUPINFOW startup={.cb=sizeof(startup)};
        PROCESS_INFORMATION process={0};
        /* Explicit executable path: no command line parsing or PATH search. */
        if(!CreateProcessW(exe,NULL,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&startup,&process)) {
            report(c,"start DriverHost",-EIO); return -EIO;
        }
        CloseHandle(process.hThread);
        ULONGLONG deadline=GetTickCount64()+5000;
        while(GetTickCount64()<deadline) {
            c->control=connect_pipe(control);
            if(c->control!=INVALID_HANDLE_VALUE) break;
            DWORD error=GetLastError();
            if(error!=ERROR_FILE_NOT_FOUND && error!=ERROR_PIPE_BUSY) break;
            HANDLE waits[2]={process.hProcess,c->cancel};
            if(WaitForMultipleObjects(2,waits,FALSE,20)!=WAIT_TIMEOUT) break;
        }
        CloseHandle(process.hProcess);
        if(c->control==INVALID_HANDLE_VALUE) { report(c,"connect started host",-EIO); return -EIO; }
    }
    struct xz_ipc_packet reply;
    rc=call(c,FALSE,XZ_IPC_OPEN,firmware,(DWORD)((wcslen(firmware)+1)*sizeof(wchar_t)),
            0,NULL,0,&reply,1500,FALSE);
    if(rc) { report(c,"transport open failed",rc); return rc; }
    if(!reply.session || !reply.generation) return -EPROTO;
    c->session=reply.session; c->generation=reply.generation;
    ULONGLONG data_deadline=GetTickCount64()+2000;
    do {
        c->data=connect_pipe(data);
        if(c->data!=INVALID_HANDLE_VALUE) break;
        DWORD error=GetLastError();
        if(error!=ERROR_PIPE_BUSY && error!=ERROR_FILE_NOT_FOUND) break;
        if(WaitForSingleObject(c->cancel,10)!=WAIT_TIMEOUT) break;
    } while(GetTickCount64()<data_deadline);
    if(c->data==INVALID_HANDLE_VALUE) { report(c,"connect TS",-EIO); return -EIO; }
    c->active=TRUE;
    report(c,"transport lease acquired; firmware preparation pending; no channel selected",0);
    return 0;
}
void xz_client_cancel(struct xz_client *c) { if(c) SetEvent(c->cancel); }
int xz_client_close(struct xz_client **owner)
{
    if(!owner || !*owner) return 0;
    struct xz_client *c=*owner;
    int rc=0;
    if(c->session && !InterlockedCompareExchange(&c->broken,0,0)) {
        struct xz_ipc_packet reply;
        rc=call(c,FALSE,XZ_IPC_CLOSE,NULL,0,0,NULL,0,&reply,1000,TRUE);
    }
    if(c->data!=INVALID_HANDLE_VALUE) CloseHandle(c->data);
    if(c->control!=INVALID_HANDLE_VALUE) CloseHandle(c->control);
    if(c->cancel && c->owns_cancel) CloseHandle(c->cancel);
    if(c->log) fclose(c->log);
    DeleteCriticalSection(&c->control_lock); DeleteCriticalSection(&c->data_lock);
    free(c); *owner=NULL; return rc;
}
int xz_client_tune(struct xz_client *c,const struct mb86H_dvb_request *request)
{
    if(!c || !c->active) return -EINVAL;
    struct xz_ipc_packet reply={0};
    int rc=call(c,FALSE,XZ_IPC_TUNE,request,sizeof(*request),0,NULL,0,&reply,1000,FALSE);
    if(reply.generation) c->generation=reply.generation;
    report(c,"selected channel request",rc);
    return rc;
}
DWORD xz_client_wait(struct xz_client *c,DWORD timeout)
{
    if(!c || !c->active) return WAIT_ABANDONED;
    struct xz_ipc_packet reply;
    int rc=call(c,TRUE,XZ_IPC_WAIT,NULL,0,timeout>100?100:timeout,
                NULL,0,&reply,2000,FALSE);
    return rc?WAIT_ABANDONED:reply.value;
}
DWORD xz_client_ready(struct xz_client *c)
{
    if(!c || !c->active) return 0;
    struct xz_ipc_packet reply;
    return call(c,TRUE,XZ_IPC_READY,NULL,0,0,NULL,0,&reply,2000,FALSE)?0:reply.value;
}
int xz_client_read(struct xz_client *c,BYTE *data,DWORD capacity,DWORD *size,DWORD *remain)
{
    if(size) *size=0;
    if(remain) *remain=0;
    if(!c || !c->active || !data || !size || capacity<XZ_IPC_TS_BYTES) return -EINVAL;
    struct xz_ipc_packet reply;
    int rc=call(c,TRUE,XZ_IPC_READ,NULL,0,0,data,capacity,&reply,2000,FALSE);
    if(!rc && (reply.generation!=c->generation || reply.length%188)) {
        InterlockedExchange(&c->broken,1); return -EPROTO;
    }
    if(!rc) { *size=reply.length; if(remain) *remain=reply.value; }
    return rc;
}
int xz_client_state(struct xz_client *c,struct xz_ipc_state *state)
{
    if(!c || !c->active || !state) return -EINVAL;
    struct xz_ipc_packet reply;
    int rc=call(c,TRUE,XZ_IPC_STATUS,NULL,0,0,state,sizeof(*state),&reply,1000,FALSE);
    if(!rc && (reply.length!=sizeof(*state) || state->reserved)) return -EPROTO;
    return rc;
}
void xz_client_purge(struct xz_client *c)
{
    if(!c || !c->active) return;
    struct xz_ipc_packet reply;
    (void)call(c,TRUE,XZ_IPC_PURGE,NULL,0,0,NULL,0,&reply,2000,FALSE);
}
