#ifndef MB86H_WINDOWS_CLIENT_H
#define MB86H_WINDOWS_CLIENT_H
#include "host.h"
#include "ipc_protocol.h"
struct xz_client;
/* Published immediately for cancellation; close is required even after a failed open. */
int xz_client_open(struct xz_client **,const wchar_t *host_exe,
                    const wchar_t *firmware,const wchar_t *log,HANDLE cancel);
void xz_client_cancel(struct xz_client *);
int xz_client_close(struct xz_client **);
int xz_client_tune(struct xz_client *,const struct mb86H_dvb_request *);
DWORD xz_client_wait(struct xz_client *,DWORD);
DWORD xz_client_ready(struct xz_client *);
int xz_client_read(struct xz_client *,BYTE *,DWORD,DWORD *,DWORD *);
int xz_client_state(struct xz_client *,struct xz_ipc_state *);
void xz_client_purge(struct xz_client *);
#endif
