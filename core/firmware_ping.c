#include "mb86H/firmware_ping.h"

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

static const uint8_t supported[] = "mAgicVISION_1";
/* Deployed wire-family prefix; independent of the C/project namespace. */
static const uint8_t identity[] = "mAgicVISION_";

int mb86H_firmware_ping_validate(const uint8_t *data, size_t size)
{
    if (!data)
        return MB86H_FIRMWARE_PING_MALFORMED;
    if (size < sizeof(identity) - 1 || memcmp(data, identity, sizeof(identity) - 1) != 0)
        return MB86H_FIRMWARE_PING_WRONG_DEVICE;
    if (size != sizeof(supported) - 1)
        return MB86H_FIRMWARE_PING_UNSUPPORTED_VERSION;
    return memcmp(data, supported, sizeof(supported) - 1) == 0
               ? MB86H_FIRMWARE_PING_SUPPORTED
               : MB86H_FIRMWARE_PING_UNSUPPORTED_VERSION;
}
