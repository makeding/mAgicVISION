#include "ipc.h"
#include <sddl.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <wchar.h>
#include <string.h>

static int token_user(TOKEN_USER **out)
{
    HANDLE token=NULL;
    DWORD bytes=0;
    *out=NULL;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)) return -EACCES;
    GetTokenInformation(token,TokenUser,NULL,0,&bytes);
    TOKEN_USER *user=malloc(bytes);
    BOOL ok=user && GetTokenInformation(token,TokenUser,user,bytes,&bytes);
    CloseHandle(token);
    if(!ok) { free(user); return -EACCES; }
    *out=user; return 0;
}
int xz_ipc_names(wchar_t control[128],wchar_t data[128],wchar_t mutex[128])
{
    TOKEN_USER *user;
    if(token_user(&user)) return -EACCES;
    uint32_t hash=2166136261u;
    const BYTE *sid=user->User.Sid;
    for(DWORD i=0;i<GetLengthSid(user->User.Sid);++i) hash=(hash^sid[i])*16777619u;
    free(user);
#ifdef XZ_IPC_TEST
    hash^=0x74657374u;
#endif
    DWORD session;
    if(!ProcessIdToSessionId(GetCurrentProcessId(),&session)) return -EIO;
    swprintf(control,128,L"\\\\.\\pipe\\mb86H.XZ.v3.%lu.%08x.control",session,hash);
    swprintf(data,128,L"\\\\.\\pipe\\mb86H.XZ.v3.%lu.%08x.data",session,hash);
    swprintf(mutex,128,L"Local\\mb86H.XZ.v3.%08x",hash);
    return 0;
}
int xz_ipc_security_init(struct xz_ipc_security *s)
{
    TOKEN_USER *user; wchar_t *sid=NULL;
    memset(s,0,sizeof(*s));
    if(token_user(&user)) return -EACCES;
    BOOL ok=ConvertSidToStringSidW(user->User.Sid,&sid);
    free(user);
    if(!ok) return -EACCES;
    wchar_t acl[256];
    swprintf(acl,256,L"D:P(A;;GA;;;%ls)",sid);
    LocalFree(sid);
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl,SDDL_REVISION_1,
          &s->descriptor,NULL)) return -EACCES;
    s->attributes=(SECURITY_ATTRIBUTES){sizeof(SECURITY_ATTRIBUTES),s->descriptor,FALSE};
    return 0;
}
void xz_ipc_security_free(struct xz_ipc_security *s)
{ if(s->descriptor) LocalFree(s->descriptor); s->descriptor=NULL; }

static DWORD wait_io(HANDLE event,ULONGLONG deadline,HANDLE cancel,HANDLE peer)
{
    HANDLE handles[3]={event}; DWORD count=1;
    if(cancel) handles[count++]=cancel;
    if(peer) handles[count++]=peer;
    ULONGLONG now=GetTickCount64();
    DWORD timeout=deadline?(now>=deadline?0:(DWORD)(deadline-now)):INFINITE;
    DWORD result=WaitForMultipleObjects(count,handles,FALSE,timeout);
    return result==WAIT_OBJECT_0?0:result==WAIT_TIMEOUT?ERROR_TIMEOUT:
        result==WAIT_FAILED?GetLastError():ERROR_OPERATION_ABORTED;
}
int xz_ipc_io(HANDLE pipe,BOOL writing,void *buffer,DWORD bytes,
              ULONGLONG deadline,HANDLE cancel,HANDLE peer)
{
    BYTE *cursor=buffer;
    while(bytes) {
        OVERLAPPED io={0}; DWORD actual=0,error=0;
        io.hEvent=CreateEventW(NULL,TRUE,FALSE,NULL);
        if(!io.hEvent) return -EIO;
        if((deadline && GetTickCount64()>=deadline) ||
           (cancel && WaitForSingleObject(cancel,0)==WAIT_OBJECT_0) ||
           (peer && WaitForSingleObject(peer,0)==WAIT_OBJECT_0)) {
            CloseHandle(io.hEvent); SetLastError(ERROR_OPERATION_ABORTED); return -ECANCELED;
        }
        BOOL ok=writing ? WriteFile(pipe,cursor,bytes,&actual,&io) :
                          ReadFile(pipe,cursor,bytes,&actual,&io);
        if(!ok) {
            error=GetLastError();
            if(error==ERROR_IO_PENDING) {
                DWORD waited=wait_io(io.hEvent,deadline,cancel,peer);
                if(waited) CancelIoEx(pipe,&io);
                ok=GetOverlappedResult(pipe,&io,&actual,TRUE);
                error=waited ? waited : (ok?0:GetLastError());
            }
        }
        CloseHandle(io.hEvent);
        if(error || !actual) {
            SetLastError(error?error:ERROR_BROKEN_PIPE);
            return error==ERROR_TIMEOUT ? -ETIMEDOUT :
                error==ERROR_OPERATION_ABORTED ? -ECANCELED : -EIO;
        }
        if(actual>bytes) return -EPROTO;
        bytes-=actual; cursor+=actual;
    }
    return 0;
}
HANDLE xz_ipc_listen(const wchar_t *name,struct xz_ipc_security *security,BOOL first)
{
    return CreateNamedPipeW(name,PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|
        (first?FILE_FLAG_FIRST_PIPE_INSTANCE:0),
        PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES,131072,131072,0,&security->attributes);
}
int xz_ipc_accept(HANDLE pipe,HANDLE cancel,DWORD timeout)
{
    OVERLAPPED io={0}; DWORD bytes=0;
    io.hEvent=CreateEventW(NULL,TRUE,FALSE,NULL);
    if(!io.hEvent) return -EIO;
    BOOL ok=ConnectNamedPipe(pipe,&io);
    DWORD error=ok?0:GetLastError();
    if(error==ERROR_PIPE_CONNECTED) error=0;
    else if(error==ERROR_IO_PENDING) {
        ULONGLONG deadline=timeout==INFINITE?0:GetTickCount64()+timeout;
        DWORD waited=wait_io(io.hEvent,deadline,cancel,NULL);
        if(waited) CancelIoEx(pipe,&io);
        ok=GetOverlappedResult(pipe,&io,&bytes,TRUE);
        error=ok?0:(waited?waited:GetLastError());
    }
    CloseHandle(io.hEvent);
    if(error==ERROR_TIMEOUT) DisconnectNamedPipe(pipe);
    return !error?0:error==ERROR_TIMEOUT?-ETIMEDOUT:-EIO;
}
