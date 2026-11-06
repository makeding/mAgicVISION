#include "mb86H/firmware_ping.h"
#include "mb86H/protocol/constants.h"

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
    static const unsigned char supported[] = MB86H_FIRMWARE_IDENTITY;
    static const unsigned char wrong_device[] = "otherDevice_v1";
    static const unsigned char previous[] = "mAgicVISION_1";
    unsigned char nul_terminated[sizeof(supported)];

    memcpy(nul_terminated, supported, sizeof(supported));
    CHECK(mb86H_firmware_ping_validate(supported, sizeof(supported) - 1) ==
          MB86H_FIRMWARE_PING_SUPPORTED);
    CHECK(mb86H_firmware_ping_validate(supported, sizeof(supported)) ==
          MB86H_FIRMWARE_PING_WRONG_IDENTITY);
    CHECK(mb86H_firmware_ping_validate(supported, sizeof(supported) - 2) ==
          MB86H_FIRMWARE_PING_WRONG_IDENTITY);
    CHECK(mb86H_firmware_ping_validate(wrong_device, sizeof(wrong_device) - 1) ==
          MB86H_FIRMWARE_PING_WRONG_IDENTITY);
    CHECK(mb86H_firmware_ping_validate(previous, sizeof(previous) - 1) ==
          MB86H_FIRMWARE_PING_WRONG_IDENTITY);
    CHECK(mb86H_firmware_ping_validate(nul_terminated, sizeof(nul_terminated)) ==
          MB86H_FIRMWARE_PING_WRONG_IDENTITY);
    CHECK(mb86H_firmware_ping_validate(NULL, 0) == MB86H_FIRMWARE_PING_MALFORMED);
    puts("firmware ping tests passed");
    return 0;
}
