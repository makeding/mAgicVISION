#include "mb86H/device.h"
#include "mb86H/usb/boot.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

struct call { struct usb_window_control control; uint8_t data[8]; };
struct mock {
    struct call records[16]; unsigned call_count, receive_count;
    int receive_rc, fail_at;
    uint16_t state, ready_first, ready_second, pending;
};

static void put16(uint8_t *p, uint16_t x) { p[0] = x >> 8; p[1] = x; }
static int control(void *opaque, const struct usb_window_control *control,
                   void *data, unsigned timeout_ms)
{
    struct mock *m = opaque; uint32_t address; uint16_t value;
    (void)timeout_ms;
    CHECK(m->call_count < 16);
    m->records[m->call_count].control = *control;
    if (control->request_type == 0x40u)
        memcpy(m->records[m->call_count].data, data, control->length);
    ++m->call_count;
    if (m->fail_at && m->call_count == (unsigned)m->fail_at) return -EIO;
    address = ((uint32_t)control->value << 8) | control->index;
    if (control->request_type != 0xc0u || control->length != 2u) return 0;
    value = address == 0x82008u ? m->state :
        address == 0x90074u ? (m->call_count <= 2u ? m->ready_first : m->ready_second) :
        address == 0x901b0u ? m->pending : 0;
    put16(data, value);
    return 0;
}
static int receive(void *opaque, uint8_t endpoint, void *data, size_t size,
                   unsigned timeout_ms)
{
    struct mock *m = opaque;
    CHECK(endpoint == 0x83u && size == 6u && timeout_ms == 500u);
    memset(data, 0, size); ++m->receive_count;
    return m->receive_rc;
}

int main(void)
{
    const struct mb86H_device_info *device;
    const struct usb_boot_io io = {.native = {control}, .receive = receive};
    struct mock m = {.state = 0xffffu, .ready_first = 4u,
                     .ready_second = 4u, .pending = 0x80u};
    struct usb_boot_result result;
    device = mb86H_device_find(0x06b8u, 0x103bu);
    CHECK(device && !strcmp(device->name, "PIXELA PIX-DT295 ISDB-T/S Tuner") &&
          device->frontend_count == 1 &&
          device->delivery_systems == ((1U << MB86H_DVB_GR) | (1U << MB86H_DVB_BSCS)) &&
          device->runtime_protocol == MB86H_RUNTIME_RESIDENT_V1 &&
          device->firmware_target == MB86H_FIRMWARE_DT295 && device->usb_boot);
    CHECK(device->usb_boot->candidate_address == 0x000e0000u &&
          device->usb_boot->candidate_size == 0x00010000u);
    CHECK(!device->usb_boot->observe(&io, &m, &result) &&
          result.stage == USB_BOOT_STAGE_READ_STATE && result.rawstate == 0xffffu &&
          result.control_count == 1 && m.call_count == 1 && !m.receive_count &&
          m.records[0].control.request_type == 0xc0u);
    memset(&m, 0, sizeof(m)); m.state = 0xffffu; m.ready_first = 4u;
    m.ready_second = 4u; m.pending = 0x80u;
    CHECK(!device->usb_boot->prepare(&io, &m, &result));
    CHECK(result.stage == USB_BOOT_STAGE_BOOT0_READY && result.control_count == 7 &&
          m.receive_count == 1 && m.call_count == 7);
    CHECK(m.records[2].control.request_type == 0x40u &&
          m.records[2].control.value == 0x0901u && m.records[2].control.index == 0x0074u &&
          m.records[2].data[1] == 4u && m.records[2].data[5] == 4u);
    memset(&m, 0, sizeof(m)); m.state = 1;
    CHECK(device->usb_boot->prepare(&io, &m, &result) == -EALREADY &&
          result.failed_stage == USB_BOOT_STAGE_READ_STATE && m.call_count == 1);
    memset(&m, 0, sizeof(m)); m.state = 0xffffu; m.ready_first = 4; m.fail_at = 2;
    CHECK(device->usb_boot->prepare(&io, &m, &result) == -EIO &&
          result.failed_stage == USB_BOOT_STAGE_READ_READY && m.call_count == 2);
    memset(&m, 0, sizeof(m)); m.state = 0;
    CHECK(!device->usb_boot->prepare(&io, &m, &result) && m.call_count == 1 &&
          !m.receive_count);
    CHECK(!mb86H_device_find(0x06b8u, 0x103au));
    /* XZ cold loading shares the downloader, but requires its own code and
     * image identity. A runtime or foreign/modified boot0 is never prepared. */
    device = mb86H_device_find(0x04bbu, 0x0530u);
    CHECK(device && device->usb_boot && !device->usb_boot->download &&
          device->firmware_target == MB86H_FIRMWARE_XZ);
    memset(&m, 0, sizeof(m)); m.state = 0x10;
    CHECK(!device->usb_boot->observe(&io, &m, &result) &&
          result.stage == USB_BOOT_STAGE_RUNNING && m.call_count == 1);
    memset(&m, 0, sizeof(m)); m.state = 0x10;
    CHECK(device->usb_boot->prepare(&io, &m, &result) == -EALREADY &&
          m.call_count == 1 && m.records[0].control.request_type == 0xc0);
    memset(&m, 0, sizeof(m));
    CHECK(device->usb_boot->prepare(&io, &m, &result) == -129 &&
          result.failed_stage == USB_BOOT_STAGE_VERIFY_BOOT0 &&
          !result.code_signature_verified && m.call_count == 2 &&
          m.records[0].control.request_type == 0xc0 && m.records[1].control.request_type == 0xc0);
    device = mb86H_device_find(0x054cu, 0x04b2u);
    CHECK(device && device->firmware_target == MB86H_FIRMWARE_TORNE &&
          device->runtime_protocol == MB86H_RUNTIME_RESIDENT_V1);
    device = mb86H_device_find(0x06b8u, 0x103bu);
    CHECK(device && device->firmware_target == MB86H_FIRMWARE_DT295);
    device = mb86H_device_find(0x06b8u, 0x103cu);
    CHECK(device && !strcmp(device->name, "PIXELA PIX-DT295W ISDB-T/S Tuner") &&
          device->firmware_target == MB86H_FIRMWARE_NONE);
    CHECK(!strcmp(mb86H_device_name(0x04c5u, 0x202fu), "PIXELA PIX-DTxxx ISDB-T/S Tuner"));
    CHECK(!strcmp(mb86H_device_name(0x06b8u, 0x1038u), "PIXELA PIX-DT420 ISDB-T/S Tuner"));
    CHECK(!strcmp(mb86H_device_name(0x06b8u, 0x1039u), "PIXELA PIX-DT410 ISDB-T/S Tuner"));
    return 0;
}
