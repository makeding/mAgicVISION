#include "ipc.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

struct server {
    SRWLOCK state,io;
    CRITICAL_SECTION connections_lock;
    struct xz_host *host;
    HANDLE stop,wake,cancel,all_done,peer,lifetime;
    LONG owner,connections;
    DWORD owner_pid,lifetime_pid,session,generation,completed,serial,phase;
    BOOL selected,prepared,closing,preparing;
    int error;
    struct mb86H_dvb_request request;
    ULONGLONG idle_since;
    wchar_t firmware[32768],directory[32768];
    struct xz_ipc_security security;
    wchar_t control_name[128],data_name[128],mutex_name[128];
    FILE *log;
};
struct connection { struct server *server; HANDLE pipe; };
static void log_state(struct server *s,const char *action,int rc)
{
    SYSTEMTIME t; GetSystemTime(&t);
    if(s->log) {
        fprintf(s->log,"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ owner %s phase=%lu session=%lu generation=%lu selected=%u system=%u frequency=%u tsid=%u rc=%d\n",
            t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds,
            action,s->phase,s->session,s->generation,s->selected,
            s->request.system,s->request.frequency,s->request.stream_id,rc);
        fflush(s->log);
    }
}
#ifndef XZ_IPC_TEST
static DWORD WINAPI failure_notice(void *opaque)
{
    wchar_t *message=opaque;
    MessageBoxW(NULL,message,L"mb86H tuner error",MB_OK|MB_ICONERROR|MB_SETFOREGROUND);
    free(message); return 0;
}
#endif
static void notify_failure(struct server *s,const wchar_t *operation,int rc)
{
#ifndef XZ_IPC_TEST
    wchar_t *message=calloc(768,sizeof(wchar_t));
    if(!message) return;
    if(rc==-129) {
        wchar_t stage[96];
        MultiByteToWideChar(CP_UTF8,0,xz_failure_stage(s->host),-1,stage,96);
        swprintf(message,768,L"Resident image validation failed during %ls (code %d).\n"
            L"The device image is incomplete or incompatible. No channel was tuned.\n\n"
            L"Close TVTest and keep the existing snapshot and DriverHost_mb86H.log for recovery.",stage,rc);
    }
    else {
        wchar_t target[96]=L"No channel selected",stage[96];
        if(s->selected && s->request.system==MB86H_DVB_GR) {
            if(s->request.frequency>=473142857u && s->request.frequency<=707142857u &&
               (s->request.frequency-473142857u)%6000000u==0) {
                DWORD channel=(s->request.frequency-473142857u)/6000000u+13u;
                swprintf(target,96,L"UHF %lu (%.3f MHz)",channel,s->request.frequency/1000000.0);
            } else swprintf(target,96,L"ISDB-T (%.3f MHz)",s->request.frequency/1000000.0);
        } else if(s->selected) swprintf(target,96,L"BS/CS (IF %.3f MHz)",s->request.frequency/1000.0);
        const char *part=xz_failure_stage(s->host);
        MultiByteToWideChar(CP_UTF8,0,part,-1,stage,96);
        if(rc==-ETIMEDOUT)
            swprintf(message,768,L"%ls timed out during %ls (code %d).\n"
                L"The selected operation did not complete before its deadline.\n\n"
                L"You can select another channel in TVTest. Keep DriverHost_mb86H.log if it repeats.",
                target,stage,rc);
        else
            swprintf(message,768,L"%ls failed for %ls during %ls (code %d).\n\n"
                L"You can select another channel in TVTest. Keep DriverHost_mb86H.log for diagnosis.",
                operation,target,stage,rc);
    }
    HANDLE thread=CreateThread(NULL,0,failure_notice,message,0,NULL);
    if(thread) CloseHandle(thread); else free(message);
#else
    (void)s; (void)operation; (void)rc;
#endif
}
static void prepare_failed(void *opaque,int rc)
{
    struct server *s=opaque;
    AcquireSRWLockExclusive(&s->state);
    s->error=rc;
    s->phase=s->closing?XZ_IPC_CLOSING:XZ_IPC_FAILED;
    log_state(s,"prepare failed; safe restoration pending",rc);
    if(!s->closing) notify_failure(s,L"Device preparation",rc);
    ReleaseSRWLockExclusive(&s->state);
}
static DWORD WINAPI owner_thread(void *opaque)
{
    struct server *s=opaque;
    for(;;) {
        HANDLE events[2]={s->stop,s->wake};
        DWORD wait=WaitForMultipleObjects(2,events,FALSE,INFINITE);
        if(wait==WAIT_OBJECT_0) break;
        AcquireSRWLockExclusive(&s->state);
        BOOL close=s->closing;
        if(close) {
            s->phase=XZ_IPC_CLOSING;
            ReleaseSRWLockExclusive(&s->state);
            struct xz_operation op={.deadline=GetTickCount64()+XZ_IPC_TUNE_MS};
            AcquireSRWLockExclusive(&s->io);
            int rc=s->host?xz_detach(s->host,&op):0;
            ReleaseSRWLockExclusive(&s->io);
            AcquireSRWLockExclusive(&s->state);
            if(rc) s->error=rc;
            s->closing=FALSE; s->selected=FALSE;
            s->owner_pid=0; s->session=0; s->owner=0;
            s->phase=s->error?XZ_IPC_FAILED:XZ_IPC_IDLE;
            s->idle_since=GetTickCount64();
            if(s->peer) { CloseHandle(s->peer); s->peer=NULL; }
            log_state(s,"lease retired",rc);
            ReleaseSRWLockExclusive(&s->state);
            continue;
        }
        if(!s->prepared && s->host && !s->error) {
            s->phase=XZ_IPC_PREPARING; s->preparing=TRUE;
            struct xz_operation op={.peer=s->peer,.cancel=s->cancel,
                .deadline=GetTickCount64()+XZ_IPC_OPEN_MS,
                .failed=prepare_failed,.opaque=s};
            ReleaseSRWLockExclusive(&s->state);
            AcquireSRWLockExclusive(&s->io);
            int rc=xz_prepare(s->host,&op);
            ReleaseSRWLockExclusive(&s->io);
            AcquireSRWLockExclusive(&s->state);
            s->preparing=FALSE;
            if(rc) { s->error=rc; s->phase=XZ_IPC_FAILED; }
            else { s->prepared=TRUE; s->phase=s->selected?XZ_IPC_QUEUED:XZ_IPC_IDLE; }
            log_state(s,"prepare complete",rc);
            if(s->closing) SetEvent(s->wake);
        }
        if(s->prepared && s->selected && !s->closing && !s->error &&
           s->completed!=s->generation) {
            DWORD generation=s->generation;
            struct mb86H_dvb_request request=s->request;
            struct xz_operation op={.peer=s->peer,.cancel=s->cancel,
                .deadline=GetTickCount64()+XZ_IPC_TUNE_MS};
            s->phase=XZ_IPC_TUNING;
            log_state(s,"tune begin",0);
            ReleaseSRWLockExclusive(&s->state);
            AcquireSRWLockExclusive(&s->io);
            int rc=xz_tune(s->host,&request,&op);
            ReleaseSRWLockExclusive(&s->io);
            AcquireSRWLockExclusive(&s->state);
            if(rc) { s->error=rc; s->phase=XZ_IPC_FAILED; }
            else if(generation==s->generation && !s->closing) {
                s->completed=generation; s->phase=XZ_IPC_STREAMING;
            } else { s->phase=XZ_IPC_QUEUED; SetEvent(s->wake); }
            log_state(s,"tune complete",rc);
            if(rc && !s->closing) notify_failure(s,L"Selected channel tuning",rc);
        }
        ReleaseSRWLockExclusive(&s->state);
    }
    return 0;
}
static void done_connection(struct server *s)
{
    EnterCriticalSection(&s->connections_lock);
    if(!InterlockedDecrement(&s->connections)) SetEvent(s->all_done);
    LeaveCriticalSection(&s->connections_lock);
}
/* Byte-pipe disconnect discards unread data. Wait for acknowledgement before
 * closing a terminal response, including BUSY and identity-validation errors. */
static int reply(HANDLE pipe,struct xz_ipc_packet *p,const void *body,HANDLE stop,HANDLE peer,BOOL terminal)
{
    ULONGLONG deadline=GetTickCount64()+1000;
    int rc=xz_ipc_io(pipe,TRUE,p,sizeof(*p),deadline,stop,peer);
    if(!rc && p->length) rc=xz_ipc_io(pipe,TRUE,(void *)body,p->length,deadline,stop,peer);
    if(!rc && terminal) {
        struct xz_ipc_packet ack;
        rc=xz_ipc_io(pipe,FALSE,&ack,sizeof(ack),deadline,stop,peer);
        if(!rc && (!xz_ipc_valid(&ack) || ack.command!=XZ_IPC_ACK ||
            ack.sequence!=p->sequence || ack.session!=p->session || ack.length)) rc=-EPROTO;
    }
    return rc;
}
static void release_lease(struct server *s,DWORD session,const char *cause,int rc,DWORD win32)
{
    AcquireSRWLockExclusive(&s->state);
    if(session && s->session==session && s->owner && !s->closing) {
        s->closing=TRUE; s->phase=XZ_IPC_CLOSING;
        char action[192];
        snprintf(action,sizeof(action),"lease cancellation cause=%s owner_pid=%lu win32=%lu",
                 cause,s->owner_pid,win32);
        log_state(s,action,rc);
        SetEvent(s->cancel); SetEvent(s->wake);
    }
    ReleaseSRWLockExclusive(&s->state);
}
static DWORD WINAPI control_thread(void *opaque)
{
    struct connection *c=opaque;
    struct server *s=c->server;
    HANDLE pipe=c->pipe,peer=NULL;
    DWORD pid=0,owned=0,last_win32=0; uint32_t sequence=0;
    int ended=0;
    const char *cause="control-connection-ended";
    free(c);
    if(!GetNamedPipeClientProcessId(pipe,&pid) || !(peer=OpenProcess(SYNCHRONIZE,FALSE,pid))) goto done;
    for(;;) {
        struct xz_ipc_packet p;
        ended=xz_ipc_io(pipe,FALSE,&p,sizeof(p),owned?0:GetTickCount64()+1000,s->stop,peer);
        if(ended) {
            last_win32=GetLastError();
            cause=WaitForSingleObject(peer,0)==WAIT_OBJECT_0?"client-process-exited":"control-read-ended";
            break;
        }
        if(!xz_ipc_valid(&p) || p.status || p.sequence!=++sequence ||
           (p.command!=XZ_IPC_OPEN && p.command!=XZ_IPC_TUNE && p.command!=XZ_IPC_CLOSE)) {
            cause="invalid-control-header"; ended=-EPROTO; break;
        }
        DWORD length=p.length; p.length=0;
        BOOL terminal=FALSE;
        if(!owned) {
            if(p.command!=XZ_IPC_OPEN || p.session || length<2 || length%sizeof(wchar_t)) break;
            wchar_t *firmware=malloc(length);
            if(!firmware) break;
            int rc=xz_ipc_io(pipe,FALSE,firmware,length,GetTickCount64()+1000,s->stop,peer);
            if(rc || firmware[length/sizeof(wchar_t)-1] ||
                (wcslen(firmware)+1)*sizeof(wchar_t)!=length) { free(firmware); break; }
            AcquireSRWLockExclusive(&s->state);
            if(s->lifetime && WaitForSingleObject(s->lifetime,0)==WAIT_OBJECT_0) {
                p.status=-EBUSY;
                log_state(s,"previous caller exited; shutdown requested",0);
                /* The main loop observes the process handle. Preserve this
                 * terminal response until the client has acknowledged it. */
            } else if(s->owner) p.status=-EBUSY;
            else if(s->error) p.status=s->error;
            else if(s->host && _wcsicmp(s->firmware,firmware)) p.status=-EINVAL;
            else {
                s->owner=1; s->owner_pid=pid;
                s->session=++s->serial; if(!s->session) s->session=++s->serial;
                owned=s->session; s->generation=1; s->completed=0; s->selected=FALSE;
                ResetEvent(s->cancel);
                DuplicateHandle(GetCurrentProcess(),peer,GetCurrentProcess(),&s->peer,0,FALSE,DUPLICATE_SAME_ACCESS);
                HANDLE lifetime=NULL;
                if(s->peer) DuplicateHandle(GetCurrentProcess(),peer,GetCurrentProcess(),&lifetime,0,FALSE,DUPLICATE_SAME_ACCESS);
                if(!s->peer || !lifetime) p.status=-EIO;
                else {
                    if(s->lifetime) CloseHandle(s->lifetime);
                    s->lifetime=lifetime; s->lifetime_pid=pid;
                }
                if(!p.status && !s->host) {
                    wchar_t snapshot[32768],log[32768];
                    swprintf(snapshot,32768,L"%lsmb86H-host-%lu-%llu.snapshot",s->directory,GetCurrentProcessId(),GetTickCount64());
                    swprintf(log,32768,L"%lsDriverHost_mb86H.log",s->directory);
                    ReleaseSRWLockExclusive(&s->state);
                    struct xz_host *host=NULL;
                    rc=xz_connect(&host,firmware,snapshot,log);
                    AcquireSRWLockExclusive(&s->state);
                    s->host=host; p.status=rc;
                    if(!rc) wcscpy(s->firmware,firmware);
                }
                s->phase=p.status?XZ_IPC_FAILED:(s->prepared?XZ_IPC_IDLE:XZ_IPC_PREPARING);
                if(p.status) s->error=p.status;
                p.session=s->session; p.generation=s->generation;
                log_state(s,"transport lease (no channel selected)",p.status);
                if(!p.status) SetEvent(s->wake);
            }
            ReleaseSRWLockExclusive(&s->state);
            free(firmware);
            terminal=p.status!=0;
        } else {
            if(p.session!=owned) break;
            if(p.command==XZ_IPC_TUNE && length==sizeof(struct mb86H_dvb_request)) {
                struct mb86H_dvb_request request;
                if(xz_ipc_io(pipe,FALSE,&request,length,GetTickCount64()+1000,s->stop,peer)) break;
                AcquireSRWLockExclusive(&s->state);
                if((unsigned)request.system>=MB86H_CHANNEL_COUNT ||
                   !mb86H_dvb_request_valid(&mb86H_frontend_caps[request.system],&request)) p.status=-EINVAL;
                else if(s->error && !s->prepared) p.status=s->error;
                else if(s->closing) p.status=-EBUSY;
                else {
                    ++s->generation; if(!s->generation) ++s->generation;
                    s->request=request; s->selected=TRUE; s->error=0;
                    if(s->prepared && !s->preparing) s->phase=XZ_IPC_QUEUED;
                    SetEvent(s->wake);
                    log_state(s,"selected channel queued",0);
                }
                p.generation=s->generation;
                ReleaseSRWLockExclusive(&s->state);
            } else if(p.command==XZ_IPC_CLOSE && !length) {
                release_lease(s,owned,"explicit-CloseTuner",0,0);
                cause="CloseTuner-reply-completed";
                terminal=TRUE;
            } else break;
        }
        int rc=reply(pipe,&p,NULL,s->stop,peer,terminal);
        if(rc || terminal) {
            ended=rc; last_win32=rc?GetLastError():0;
            if(rc) cause="control-reply-or-ack-failed";
            break;
        }
    }
done:
    release_lease(s,owned,cause,ended,last_win32);
    if(peer) CloseHandle(peer);
    DisconnectNamedPipe(pipe); CloseHandle(pipe);
    done_connection(s); return 0;
}
static DWORD WINAPI data_thread(void *opaque)
{
    struct server *s=opaque; BOOL first=TRUE;
    while(WaitForSingleObject(s->stop,0)==WAIT_TIMEOUT) {
        HANDLE pipe=xz_ipc_listen(s->data_name,&s->security,first);
        if(pipe==INVALID_HANDLE_VALUE) { SetEvent(s->stop); break; }
        first=FALSE;
        if(xz_ipc_accept(pipe,s->stop,INFINITE)) { CloseHandle(pipe); break; }
        DWORD pid=0; HANDLE peer=NULL;
        if(GetNamedPipeClientProcessId(pipe,&pid)) peer=OpenProcess(SYNCHRONIZE,FALSE,pid);
        uint32_t sequence=0;
        while(peer) {
            struct xz_ipc_packet p; BYTE data[XZ_IPC_TS_BYTES];
            if(xz_ipc_io(pipe,FALSE,&p,sizeof(p),0,s->stop,peer)) break;
            if(!xz_ipc_valid(&p) || p.status || p.length || p.sequence!=++sequence ||
               p.command<XZ_IPC_READ || p.command>XZ_IPC_STATUS) break;
            AcquireSRWLockShared(&s->state);
            if(!s->session || p.session!=s->session || pid!=s->owner_pid) p.status=-EACCES;
            else if(p.command==XZ_IPC_STATUS) {
                struct xz_ipc_state state={s->phase,s->generation,s->completed,s->selected,s->error,0};
                memcpy(data,&state,sizeof(state)); p.length=sizeof(state);
            } else if(s->error) p.status=s->error;
            else if(p.generation!=s->generation) p.status=-EPROTO;
            else if(s->phase!=XZ_IPC_STREAMING || s->completed!=s->generation || s->closing) {
                if(p.command==XZ_IPC_WAIT) p.value=WAIT_TIMEOUT;
                else p.value=0;
            } else if(TryAcquireSRWLockShared(&s->io)) {
                if(p.command==XZ_IPC_READ) {
                    DWORD bytes=0,remain=0;
                    p.status=xz_read(s->host,data,sizeof(data),&bytes,&remain);
                    p.length=bytes; p.value=remain;
                } else if(p.command==XZ_IPC_READY) p.value=xz_ready(s->host);
                else if(p.command==XZ_IPC_WAIT) p.value=xz_wait(s->host,p.value>100?100:p.value);
                else if(p.command==XZ_IPC_PURGE) xz_purge(s->host);
                ReleaseSRWLockShared(&s->io);
            } else if(p.command==XZ_IPC_WAIT) p.value=WAIT_TIMEOUT;
            p.generation=s->generation;
            ReleaseSRWLockShared(&s->state);
            if(reply(pipe,&p,data,s->stop,peer,FALSE)) break;
        }
        if(peer) CloseHandle(peer);
        DisconnectNamedPipe(pipe); CloseHandle(pipe);
    }
    return 0;
}
int xz_driver_host_main(void)
{
    struct server *s=calloc(1,sizeof(*s));
    if(!s) return 1;
    InitializeSRWLock(&s->state); InitializeSRWLock(&s->io);
    InitializeCriticalSection(&s->connections_lock);
    DWORD size=GetModuleFileNameW(NULL,s->directory,32768);
    if(!size || size>=32600) { free(s); return 1; }
    wchar_t *slash=wcsrchr(s->directory,L'\\');
    if(!slash) { free(s); return 1; } slash[1]=0;
    if(xz_ipc_names(s->control_name,s->data_name,s->mutex_name) || xz_ipc_security_init(&s->security)) { free(s); return 1; }
    wchar_t log[32768]; swprintf(log,32768,L"%lsDriverHost_mb86H.owner.log",s->directory);
    s->log=_wfopen(log,L"a");
    HANDLE mutex=CreateMutexW(&s->security.attributes,TRUE,s->mutex_name);
    if(!mutex || GetLastError()==ERROR_ALREADY_EXISTS) {
        if(mutex) CloseHandle(mutex);
        if(s->log) fclose(s->log);
        xz_ipc_security_free(&s->security); free(s); return 0;
    }
    s->stop=CreateEventW(NULL,TRUE,FALSE,NULL); s->wake=CreateEventW(NULL,FALSE,FALSE,NULL);
    s->cancel=CreateEventW(NULL,TRUE,FALSE,NULL); s->all_done=CreateEventW(NULL,TRUE,TRUE,NULL);
    HANDLE data_worker=NULL,owner_worker=NULL,listener=INVALID_HANDLE_VALUE;
    int rc=1;
    if(!s->stop || !s->wake || !s->cancel || !s->all_done) goto done;
    data_worker=CreateThread(NULL,0,data_thread,s,0,NULL);
    owner_worker=CreateThread(NULL,0,owner_thread,s,0,NULL);
    if(!data_worker || !owner_worker) goto done;
    s->idle_since=GetTickCount64(); BOOL first=TRUE;
    while(WaitForSingleObject(s->stop,0)==WAIT_TIMEOUT) {
        AcquireSRWLockExclusive(&s->state);
        BOOL caller_exited=s->lifetime && WaitForSingleObject(s->lifetime,0)==WAIT_OBJECT_0;
        if(caller_exited) {
            char action[128];
            snprintf(action,sizeof(action),"caller exited pid=%lu; shutdown and restoration begin",s->lifetime_pid);
            log_state(s,action,0);
            s->closing=TRUE; s->phase=XZ_IPC_CLOSING;
        }
        ReleaseSRWLockExclusive(&s->state);
        if(caller_exited) { rc=0; break; }
        if(listener==INVALID_HANDLE_VALUE) {
            listener=xz_ipc_listen(s->control_name,&s->security,first);
            if(listener==INVALID_HANDLE_VALUE) break;
            first=FALSE;
        }
        int accepted=xz_ipc_accept(listener,s->stop,1000);
        if(accepted==-ETIMEDOUT) {
            AcquireSRWLockShared(&s->state);
            BOOL idle=!s->lifetime && !s->owner &&
                !InterlockedCompareExchange(&s->connections,0,0) &&
                GetTickCount64()-s->idle_since>=XZ_IPC_ATTACH_MS;
            ReleaseSRWLockShared(&s->state);
            if(idle) { rc=0; break; }
            continue;
        }
        if(accepted) break;
        struct connection *c=malloc(sizeof(*c)); if(!c) break;
        *c=(struct connection){s,listener};
        EnterCriticalSection(&s->connections_lock);
        ResetEvent(s->all_done); InterlockedIncrement(&s->connections);
        LeaveCriticalSection(&s->connections_lock);
        HANDLE worker=CreateThread(NULL,0,control_thread,c,0,NULL);
        if(!worker) { free(c); done_connection(s); break; }
        CloseHandle(worker); listener=INVALID_HANDLE_VALUE;
    }
done:
    if(s->stop) SetEvent(s->stop);
    if(s->cancel) SetEvent(s->cancel);
    if(listener!=INVALID_HANDLE_VALUE) CloseHandle(listener);
    if(data_worker) { WaitForSingleObject(data_worker,INFINITE); CloseHandle(data_worker); }
    if(s->all_done) WaitForSingleObject(s->all_done,INFINITE);
    if(owner_worker) { WaitForSingleObject(owner_worker,INFINITE); CloseHandle(owner_worker); }
    if(s->host) { int closed=xz_close(&s->host); if(closed) rc=1; }
    if(s->peer) CloseHandle(s->peer);
    if(s->lifetime) CloseHandle(s->lifetime);
    log_state(s,rc?"DriverHost exiting; restoration or shutdown failed; keep snapshot":
                   "DriverHost exiting; restoration complete",rc);
    if(s->stop) CloseHandle(s->stop);
    if(s->wake) CloseHandle(s->wake);
    if(s->cancel) CloseHandle(s->cancel);
    if(s->all_done) CloseHandle(s->all_done);
    ReleaseMutex(mutex); CloseHandle(mutex);
    if(s->log) fclose(s->log);
    xz_ipc_security_free(&s->security);
    DeleteCriticalSection(&s->connections_lock); free(s); return rc;
}
#ifndef XZ_IPC_TEST
int wmain(void) { return xz_driver_host_main(); }
#endif
