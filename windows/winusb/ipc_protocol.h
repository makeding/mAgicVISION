#ifndef MB86H_WINDOWS_IPC_PROTOCOL_H
#define MB86H_WINDOWS_IPC_PROTOCOL_H
#include <stdint.h>
#define XZ_IPC_MAGIC 0x485a5849u
#define XZ_IPC_VERSION 3u
#define XZ_IPC_TS_BYTES (256u*188u)
#define XZ_IPC_PATH_BYTES 65536u
#define XZ_IPC_OPEN_MS 180000u
#define XZ_IPC_TUNE_MS 95000u
#define XZ_IPC_ATTACH_MS 5000u
enum xz_ipc_command { XZ_IPC_OPEN=1, XZ_IPC_TUNE, XZ_IPC_CLOSE,
    XZ_IPC_READ, XZ_IPC_READY, XZ_IPC_WAIT, XZ_IPC_PURGE, XZ_IPC_STATUS, XZ_IPC_ACK };
enum xz_ipc_phase { XZ_IPC_PREPARING=1, XZ_IPC_IDLE, XZ_IPC_QUEUED,
    XZ_IPC_TUNING, XZ_IPC_STREAMING, XZ_IPC_FAILED, XZ_IPC_CLOSING };
struct xz_ipc_state {
    uint32_t phase,generation,completed,selected;
    int32_t error;
    uint32_t reserved;
};
_Static_assert(sizeof(struct xz_ipc_state)==24,"fixed status ABI");
struct xz_ipc_packet {
    uint32_t magic,version,command,sequence,session,generation,length,value;
    int32_t status;
    uint32_t reserved;
};
_Static_assert(sizeof(struct xz_ipc_packet)==40,"fixed local IPC ABI");
static inline int xz_ipc_valid(const struct xz_ipc_packet *p)
{
    return p && p->magic==XZ_IPC_MAGIC && p->version==XZ_IPC_VERSION &&
        p->command>=XZ_IPC_OPEN && p->command<=XZ_IPC_ACK &&
        p->sequence && !p->reserved && p->length<=XZ_IPC_PATH_BYTES;
}
#endif
