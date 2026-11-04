#include "mb86H/firmware_ping.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__); \
        return 1; \
    } \
} while (0)

int main(void)
{
    static const unsigned char supported[] = "mAgicVISION_1";
    static const unsigned char wrong_device[] = "otherDevice_v1";
    static const unsigned char v2[] = "mAgicVISION_2";
    unsigned char nul_terminated[sizeof(supported)];

    memcpy(nul_terminated, supported, sizeof(supported));
    CHECK(mb86H_firmware_ping_validate(supported, sizeof(supported) - 1) ==
          MB86H_FIRMWARE_PING_SUPPORTED);
    CHECK(mb86H_firmware_ping_validate(supported, sizeof(supported)) ==
          MB86H_FIRMWARE_PING_UNSUPPORTED_VERSION);
    CHECK(mb86H_firmware_ping_validate(supported, sizeof(supported) - 2) ==
          MB86H_FIRMWARE_PING_UNSUPPORTED_VERSION);
    CHECK(mb86H_firmware_ping_validate(wrong_device, sizeof(wrong_device) - 1) ==
          MB86H_FIRMWARE_PING_WRONG_DEVICE);
    CHECK(mb86H_firmware_ping_validate(v2, sizeof(v2) - 1) ==
          MB86H_FIRMWARE_PING_UNSUPPORTED_VERSION);
    CHECK(mb86H_firmware_ping_validate(nul_terminated, sizeof(nul_terminated)) ==
          MB86H_FIRMWARE_PING_UNSUPPORTED_VERSION);
    CHECK(mb86H_firmware_ping_validate(NULL, 0) == MB86H_FIRMWARE_PING_MALFORMED);
    puts("firmware ping tests passed");
    return 0;
}
