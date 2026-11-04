#include "ipc_protocol.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    struct xz_ipc_packet p={.magic=XZ_IPC_MAGIC,.version=XZ_IPC_VERSION,
        .command=XZ_IPC_OPEN,.sequence=1,.length=2};
    assert(xz_ipc_valid(&p));
    p.version++; assert(!xz_ipc_valid(&p)); p.version--;
    p.magic^=1; assert(!xz_ipc_valid(&p)); p.magic^=1;
    p.sequence=0; assert(!xz_ipc_valid(&p)); p.sequence=1;
    p.length=XZ_IPC_PATH_BYTES+1; assert(!xz_ipc_valid(&p));
    p.length=XZ_IPC_PATH_BYTES; assert(xz_ipc_valid(&p));
    p.reserved=1; assert(!xz_ipc_valid(&p)); p.reserved=0;
    p.command=XZ_IPC_ACK+1; assert(!xz_ipc_valid(&p));
    puts("PASS IPC version, exact header ABI, sequence and payload bounds");
    return 0;
}
