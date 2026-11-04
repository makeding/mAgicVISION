#ifndef MB86H_WINDOWS_HOST_H
#define MB86H_WINDOWS_HOST_H
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <stdint.h>
#include "mb86H/frontend.h"
struct xz_host;
/* A caller owns these handles for the duration of its synchronous request.
 * peer is the DriverHost client's process handle; pipe is optional and is
 * checked for caller disconnection at safe boundaries.  deadline is an absolute GetTickCount64()
 * value, or zero for no caller deadline. */
struct xz_operation {
    HANDLE peer;
    HANDLE cancel;
    HANDLE pipe;
    ULONGLONG deadline;
    void (*failed)(void *opaque,int result);
    void *opaque;
};
/* All returned pointers remain owned until a successful close. */
/* Connect validates transport only; preparation runs in the physical owner. */
int xz_connect(struct xz_host **out,const wchar_t *bundle,const wchar_t *snapshot,const wchar_t *log);
int xz_prepare(struct xz_host *,const struct xz_operation *operation);
int xz_tune(struct xz_host *, const struct mb86H_dvb_request *,
            const struct xz_operation *operation);
/* Retire a disconnected client's stream while keeping its resident image and
 * durable preimage in this physical owner for a later warm client. */
int xz_detach(struct xz_host *, const struct xz_operation *operation);
const char *xz_failure_stage(struct xz_host *);
int xz_close(struct xz_host **);
DWORD xz_wait(struct xz_host *, DWORD timeout);
DWORD xz_ready(struct xz_host *);
int xz_read(struct xz_host *, BYTE *, DWORD capacity, DWORD *size, DWORD *remain);
void xz_purge(struct xz_host *);
#endif
