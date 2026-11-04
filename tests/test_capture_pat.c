#define main usbboot_program_main
#include "../userland/cli/usbboot.c"
#undef main
#include <assert.h>

static void pat_crc(uint8_t section[16])
{
    uint32_t crc = UINT32_MAX;
    for (unsigned i = 0; i < 12; ++i) {
        crc ^= (uint32_t)section[i] << 24;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc << 1) ^ ((crc & 0x80000000u) ? 0x04c11db7u : 0);
    }
    for (unsigned i = 0; i < 4; ++i) section[12 + i] = crc >> (24 - 8 * i);
}
int main(void)
{
    uint8_t section[16] = {0, 0xb0, 13, 0x7d, 0x70, 0xc1, 0, 0, 0, 1, 0xe1, 0};
    uint8_t packet[188];
    struct capture_pat pat = {0};
    pat_crc(section);
    memset(packet, 0xff, sizeof(packet));
    packet[0] = 0x47; packet[1] = 0x40; packet[2] = 0; packet[3] = 0x10;
    packet[4] = 0; memcpy(packet + 5, section, sizeof(section));
    capture_pat_packet(&pat, packet, 1000, 0);
    assert(pat.count == 1 && pat.seen[0].tsid == 0x7d70 && pat.seen[0].ns == 1000);
    capture_pat_packet(&pat, packet, 2000, 188);
    assert(pat.count == 1 && pat.seen[0].ns == 1000);
    section[3] = 0x40; section[4] = 0xf1; pat_crc(section);
    packet[3] = 0x11; memcpy(packet + 5, section, sizeof(section)); packet[20] ^= 1;
    capture_pat_packet(&pat, packet, 3000, 376);
    assert(pat.count == 1); /* Invalid CRC never supplies a latency endpoint. */
    /* A PAT section split over payloads with adaptation and consecutive CC. */
    memset(packet, 0xff, sizeof(packet));
    packet[0] = 0x47; packet[1] = 0x40; packet[2] = 0; packet[3] = 0x32;
    packet[4] = 177; packet[5] = 0; packet[182] = 0;
    memcpy(packet + 183, section, 5);
    capture_pat_packet(&pat, packet, 4000, 564);
    assert(pat.count == 1 && pat.used == 5);
    memset(packet, 0xff, sizeof(packet));
    packet[0] = 0x47; packet[1] = 0; packet[2] = 0; packet[3] = 0x13;
    memcpy(packet + 4, section + 5, 11);
    capture_pat_packet(&pat, packet, 5000, 752);
    assert(pat.count == 2 && pat.seen[1].tsid == 0x40f1 &&
           pat.seen[1].ns == 5000 && pat.seen[1].offset == 752);
    puts("Capture PAT timing: CRC, TSID, duplicate and split-section checks passed");
    return 0;
}
