#ifndef MB86H_WINDOWS_IPC_H
#define MB86H_WINDOWS_IPC_H
#include "host.h"
#include "ipc_protocol.h"
_Static_assert(sizeof(struct mb86H_dvb_request)==32,"fixed local tune request ABI");
struct xz_ipc_security { SECURITY_ATTRIBUTES attributes; void *descriptor; };
int xz_ipc_names(wchar_t control[128],wchar_t data[128],wchar_t mutex[128]);
int xz_ipc_security_init(struct xz_ipc_security *);
void xz_ipc_security_free(struct xz_ipc_security *);
/* One exact transfer; cancellation drains the original operation, never retries it. */
int xz_ipc_io(HANDLE pipe,BOOL writing,void *buffer,DWORD bytes,
              ULONGLONG deadline,HANDLE cancel,HANDLE peer);
HANDLE xz_ipc_listen(const wchar_t *,struct xz_ipc_security *,BOOL first);
int xz_ipc_accept(HANDLE pipe,HANDLE cancel,DWORD timeout);
#endif
