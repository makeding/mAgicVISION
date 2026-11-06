/* SPDX-License-Identifier: MIT */
#include "mb86H/firmware_ping.h"
#include "mb86H/protocol/constants.h"

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

int mb86H_firmware_ping_validate(const uint8_t *data, size_t size)
{
    if (!data)
        return MB86H_FIRMWARE_PING_MALFORMED;
    if (size != MB86H_FIRMWARE_IDENTITY_BYTES)
        return MB86H_FIRMWARE_PING_WRONG_IDENTITY;
    return memcmp(data, MB86H_FIRMWARE_IDENTITY,
                  MB86H_FIRMWARE_IDENTITY_BYTES) == 0
               ? MB86H_FIRMWARE_PING_SUPPORTED
               : MB86H_FIRMWARE_PING_WRONG_IDENTITY;
}
