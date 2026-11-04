#include "mb86H/device.h"
#include "mb86H/usb/boot.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct record { struct usb_window_control setup; uint8_t bytes[8]; };
struct mock {
    struct record controls[128];
    unsigned count, receives, writes, state_reads, fail_control;
    unsigned fail_write, short_write, fail_receive, cancel_after;
    size_t uploaded;
    uint16_t completion_state;
    const uint8_t *image;
};
static void put32(uint8_t *p, uint32_t n)
{ p[0] = n >> 24; p[1] = n >> 16; p[2] = n >> 8; p[3] = n; }
static uint32_t get32(const uint8_t *p)
{ return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void image_header(uint8_t *p, size_t size, const char magic[8])
{
    uint32_t sum = 0;
    unsigned i;
    memset(p, 0, 32); memcpy(p, magic, 8);
    put32(p + 24, (uint32_t)(size - 544));
    for (i = 0; i < 28; i += 4) sum += get32(p + i);
    put32(p + 28, 0u - sum);
}
static int control(void *opaque, const struct usb_window_control *s, void *data,
                    unsigned timeout)
{
    struct mock *m = opaque;
    struct record *r;
    uint32_t address = (uint32_t)(s->value & 0xff00u) << 8 | s->index;
    uint16_t value;
    assert(m->count < 128 && timeout == 1000 && s->request == 0xbc);
    r = &m->controls[m->count++]; r->setup = *s;
    if (m->fail_control == m->count) return -EPIPE;
    if (s->request_type == 0x40) {
        assert(s->length <= sizeof(r->bytes)); memcpy(r->bytes, data, s->length);
        return 0;
    }
    assert(s->request_type == 0xc0 && s->length == 2);
    value = address == 0x82008 ? (m->state_reads++ ? m->completion_state : 0) : 4;
    ((uint8_t *)data)[0] = value >> 8; ((uint8_t *)data)[1] = value;
    return 0;
}
static int receive(void *opaque, uint8_t ep, void *data, size_t size, unsigned timeout)
{
    struct mock *m = opaque;
    assert(ep == 0x83 && size == 6);
    ++m->receives;
    assert(timeout == (m->receives == 1 ? 100u : 2000u));
    if (m->receives == 1) assert(!m->writes);
    else assert(m->uploaded);
    if (m->fail_receive == m->receives) return -ETIMEDOUT;
    memset(data, 0, size); return 0;
}
static int transmit(void *opaque, uint8_t ep, const void *data, size_t size,
                     size_t *actual, unsigned timeout)
{
    struct mock *m = opaque;
    assert(ep == 2 && size <= 65536 && timeout == 300 && m->receives == 1);
    assert(!memcmp(data, m->image + m->uploaded, size));
    ++m->writes;
    *actual = m->short_write == m->writes ? size - 1 : size;
    if (m->fail_write == m->writes) { *actual = 7; return -EPIPE; }
    m->uploaded += *actual; return 0;
}
static int cancelled(void *opaque)
{
    struct mock *m = opaque;
    return m->cancel_after && m->writes >= m->cancel_after;
}
static void expect_mask(const struct record *r, uint32_t address,
                         unsigned shift, uint16_t mask, uint16_t value)
{
    uint8_t expected[8] = {0};
    size_t lane = address & 2u;
    assert(r->setup.request_type == 0x40 && r->setup.request == 0xbc &&
           r->setup.value == (((address >> 8) & 0xff00u) | 1u) &&
           r->setup.index == (uint16_t)(address & ~3u) && r->setup.length == 8);
    value <<= shift; mask <<= shift;
    expected[lane] = value >> 8; expected[lane + 1] = value;
    expected[lane + 4] = mask >> 8; expected[lane + 5] = mask;
    assert(!memcmp(r->bytes, expected, 8));
}
static void expect_transcript(const struct mock *m)
{
    /* SYS 17ABC / 126EA, including the repeated bit 1 clear. */
    static const unsigned clear[] = {13,12,11,10,9,8,7,6,5,4,3,2,1,1,0};
    unsigned n = 1, i;
    expect_mask(&m->controls[n++], 0x90194, 12, 3, 1);
    assert(m->controls[n].setup.length == 2 && m->controls[n].setup.index == 0x198 &&
           m->controls[n].setup.value == 0x900 && m->controls[n].bytes[0] == 0 &&
           m->controls[n].bytes[1] == 0x40); ++n;
    for (i = 0; i < sizeof(clear) / sizeof(clear[0]); ++i)
        expect_mask(&m->controls[n++], 0x90078, clear[i], 1, 0);
    for (i = 12; i; --i) expect_mask(&m->controls[n++], 0x901b4, i - 1, 1, 0);
    assert(m->controls[n++].setup.index == 0x74);
    expect_mask(&m->controls[n++], 0x90074, 2, 1, 1);
    /* SYS 17B72 / 124EE: enable before trigger, bit 4 stays clear. */
    for (i = 14; i; --i)
        expect_mask(&m->controls[n++], 0x90078, i - 1, 1, i == 5 ? 0 : 1);
    for (i = 12; i; --i) expect_mask(&m->controls[n++], 0x901b4, i - 1, 1, 1);
    expect_mask(&m->controls[n++], 0x90070, 2, 1, 1);
    assert(m->controls[n++].setup.index == 0x74);
    assert(m->controls[n++].setup.index == 0x2008);
    expect_mask(&m->controls[n++], 0x90074, 2, 1, 1);
    assert(n == m->count);
}
int main(void)
{
    const struct mb86H_device_info *device = mb86H_device_find(0x06b8, 0x103b);
    const struct usb_boot_io io = {
        .native = {control}, .receive = receive, .transmit = transmit,
    };
    struct usb_boot_download_result result;
    struct mock m;
    const size_t size = 2u * 65536u + 17u;
    uint8_t *image = malloc(size);
    unsigned i;
    assert(image && device && device->usb_boot->download);
    for (i = 0; i < size; ++i) image[i] = (uint8_t)i;
    image_header(image, size, "MB8AC031");
#define RESET() do { memset(&m, 0, sizeof(m)); m.image = image; m.completion_state = 0x10; } while (0)
#define DOWNLOAD(options) usb_boot_download(device->usb_boot->download, &io, &m, image, size, options, &result)
    RESET();
    assert(!DOWNLOAD(NULL));
    assert(result.download.stage == MB86H_NATIVE_DONE &&
           result.download.confirmed_bytes == size && result.download.completed_chunks == 3 &&
           result.download.last_actual == 17 && !result.download.recovery_required &&
           result.boot.stage == USB_BOOT_STAGE_RUNNING && m.receives == 2 && m.writes == 3);
    expect_transcript(&m);
    RESET(); image[0] ^= 1;
    assert(DOWNLOAD(NULL) == -ENOEXEC && !m.count && !m.writes &&
           !result.download.recovery_required && result.download.failed_stage == MB86H_NATIVE_VALIDATE);
    image[0] ^= 1; image[31] ^= 1;
    assert(DOWNLOAD(NULL) == -EBADMSG && !m.count);
    image[31] ^= 1;
    assert(mb86H_native_image_validate(image, size - 1, "MB8AC031") == -EBADMSG);
    RESET(); m.fail_control = 4;
    assert(DOWNLOAD(NULL) == -EPIPE && m.count == 4 && !m.writes &&
           result.download.failed_stage == MB86H_NATIVE_PREPARE && result.download.recovery_required);
    RESET(); m.fail_receive = 1;
    assert(DOWNLOAD(NULL) == -ETIMEDOUT && !m.writes && m.receives == 1);
    RESET(); m.short_write = 2;
    assert(DOWNLOAD(NULL) == -EIO && m.writes == 2 && m.receives == 1 &&
           result.download.confirmed_bytes == 65536 && result.download.last_actual == 65535 &&
           result.download.failed_stage == MB86H_NATIVE_TRANSFER && result.download.recovery_required);
    RESET(); m.fail_write = 1;
    assert(DOWNLOAD(NULL) == -EPIPE && m.writes == 1 && m.receives == 1 &&
           !result.download.confirmed_bytes && result.download.last_actual == 7);
    RESET(); m.fail_receive = 2;
    assert(DOWNLOAD(NULL) == -ETIMEDOUT && m.writes == 3 && m.receives == 2 &&
           result.download.failed_stage == MB86H_NATIVE_ACTIVATE &&
           result.boot.failed_stage == USB_BOOT_STAGE_WAIT_COMPLETE);
    RESET(); m.completion_state = 0x12;
    assert(DOWNLOAD(NULL) == -EPROTO && result.boot.failed_stage == USB_BOOT_STAGE_READ_COMPLETE);
    expect_mask(&m.controls[m.count - 1], 0x90074, 2, 1, 1);
    RESET(); m.cancel_after = 1;
    {
        const struct mb86H_native_download_options options = {.cancelled = cancelled, .opaque = &m};
        assert(DOWNLOAD(&options) == -ECANCELED && m.writes == 1 && m.receives == 1 &&
               result.download.confirmed_bytes == 65536 && result.download.recovery_required);
    }
    /* XZ production loading uses the shared resident loader, not this factory
     * container transport. It has no factory-image download binding. */
    device = mb86H_device_find(0x04bb, 0x0530);
    RESET(); image_header(image, size, "MB8AC018");
    assert(!mb86H_native_image_validate(image, size, "MB8AC018"));
    assert(!device->usb_boot->download);
    assert(usb_boot_validate_image(device->usb_boot->download, image, size) == -EINVAL);
    assert(DOWNLOAD(NULL) == -EINVAL && !m.count && !m.writes &&
           !result.download.recovery_required && result.download.failed_stage == MB86H_NATIVE_VALIDATE);
    free(image);
    puts("native download transcript and failure tests passed");
    return 0;
}
