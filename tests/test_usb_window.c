#include "mb86H/usb/window.h"
#include "mb86H/protocol/constants.h"
#include "mb86H/protocol.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

struct xz_mock {
    uint8_t packet[64];
    unsigned controls, fail, corrupt, last_word;
    struct usb_xz_read_cache *cache;
};
static int xz_control(void *opaque, const struct usb_window_control *control,
                       void *data, unsigned timeout)
{
    struct xz_mock *m = opaque;
    m->cache->valid = false; /* Other native control operations invalidate it. */
    assert(timeout == 20000);
    if (++m->controls == m->fail) return -777;
    if (control->request_type == 0x40) {
        unsigned word = control->index - (uint16_t)MB86H_BOOT0_STAGING_ADDRESS;
        assert(control->length == 4 && word < m->last_word);
        m->last_word = word;
        memcpy(m->packet, data, 4); /* XZ's native OUT scratch aliases staging. */
        memcpy(m->packet + word, data, 4);
    } else {
        uint32_t offset = mb86H_debug_u32be_get(m->packet + 12);
        uint32_t n = mb86H_debug_u32be_get(m->packet + 20);
        assert(control->length == 64);
        for (unsigned i = 0; i < n; ++i) m->packet[24 + i] = (uint8_t)(offset + i + 0x51);
        mb86H_debug_u32be_put(m->packet + 8, MB86H_DEBUG_OK);
        mb86H_debug_u32be_put(m->packet + 12, n);
        mb86H_debug_u32be_put(m->packet + 16, offset);
        mb86H_debug_u32be_put(m->packet + 20, 0);
        memcpy(data, m->packet, 64);
        if (m->corrupt) ((uint8_t *)data)[m->corrupt - 1] ^= 1;
        m->last_word = 64;
    }
    return 0;
}
static void xz_cached_reads(void)
{
    const struct usb_native_ops ops = {.control = xz_control};
    uint8_t tx[64], rx[64], zeros[40] = {0};
    struct usb_xz_read_cache cache = {0};
    struct xz_mock m = {.cache = &cache, .last_word = 64};
    mb86H_debug_packet_pack(tx, MB86H_DEBUG_MAGIC, 1, MB86H_DEBUG_RESIDENT_BLOCK, 0, 0, 40, zeros);
    assert(!usb_xz_exchange_cached(&ops, &m, &cache, tx, rx));
    assert(cache.valid && m.controls == 17);
    mb86H_debug_packet_pack(tx, MB86H_DEBUG_MAGIC, 2, MB86H_DEBUG_RESIDENT_BLOCK, 80, 0, 40, zeros);
    m.controls = 0;
    assert(!usb_xz_exchange_cached(&ops, &m, &cache, tx, rx));
    assert(cache.valid && m.controls == 6 && !memcmp(tx + 24, zeros, 40));
    for (unsigned i = 0; i < 40; ++i) assert(rx[24 + i] == (uint8_t)(80 + i + 0x51));
    for (unsigned fail = 1; fail <= 7; ++fail) {
        struct usb_xz_read_cache attempt = cache;
        struct xz_mock broken = {.cache = &attempt, .last_word = 64, .fail = fail};
        memcpy(broken.packet, cache.packet, 64);
        mb86H_debug_packet_pack(tx, MB86H_DEBUG_MAGIC, 3, MB86H_DEBUG_RESIDENT_BLOCK, 120, 0, 40, zeros);
        assert(usb_xz_exchange_cached(&ops, &broken, &attempt, tx, rx) == -777);
        assert(!attempt.valid && broken.controls == fail);
    }
    for (unsigned field = 0; field < 6; ++field) {
        struct usb_xz_read_cache attempt = {0};
        struct xz_mock corrupt = {.cache = &attempt, .last_word = 64, .corrupt = 4 * field + 1};
        assert(!usb_xz_exchange_cached(&ops, &corrupt, &attempt, tx, rx));
        assert(!attempt.valid);
    }
}

struct native_mock {
    uint8_t mapped[4];
    uint8_t saw_ring_map[4];
    unsigned controls;
    unsigned block_writes;
};

static int native_control(void *opaque, const struct usb_window_control *control,
                          void *data, unsigned timeout)
{
    struct native_mock *mock = opaque;

    assert(control->request == USB_WINDOW_VENDOR_REQUEST);
    assert(timeout == 1000);
    mock->controls++;
    if (control->length == 4 && control->request_type == 0x40) {
        memcpy(mock->mapped, data, sizeof(mock->mapped));
        if (!memcmp(data, "\x82\0\0\0", 4))
            memcpy(mock->saw_ring_map, data, sizeof(mock->saw_ring_map));
    } else if (control->length == 4 && control->request_type == 0xc0) {
        memcpy(data, mock->mapped, sizeof(mock->mapped));
    } else if (control->request_type == 0xc0) {
        memset(data, 0xa5, control->length);
    } else if (control->request_type == 0x40 && control->length == 64) {
        mock->block_writes++;
    }
    return 0;
}

int main(void)
{
    xz_cached_reads();
    struct usb_window_control c;
    uint32_t address = 0;
    assert(usb_window_control_from_address(0x8219Cu, 0x110u,
                                           USB_WINDOW_OUT, &c) == 0);
    assert(c.request_type == 0x40u && c.request == 0xBCu);
    assert(c.value == 0x0800u && c.index == 0x219Cu && c.length == 0x110u);
    assert(usb_window_address_from_control(&c, &address) == 0);
    assert(address == 0x8219Cu);
    assert(usb_window_control_from_address(0x9FFF0u, 0x20u,
                                           USB_WINDOW_IN, &c) != 0);
    assert(usb_window_control_from_address(0x80000u, 0x200u,
                                           USB_WINDOW_OUT, &c) == 0);
    {
        struct native_mock mock = {0};
        struct usb_native_ops ops = { .control = native_control };
        uint8_t ring[64];

        assert(usb_torne_window(&ops, &mock, MB86H_TORNE_TS_RING_BASE,
                                ring, sizeof(ring), false, false) == 0);
        assert(!memcmp(mock.saw_ring_map, "\x82\0\0\0", 4));
        for (unsigned i = 0; i < sizeof(ring); ++i)
            assert(ring[i] == 0xa5);
        assert(usb_torne_window(&ops, &mock,
                                MB86H_TORNE_TS_RING_BASE + MB86H_TORNE_TS_RING_BYTES - 4,
                                ring, 8, false, false) == -22);
    }
    {
        struct native_mock mock = {.mapped = {0, 0xf0, 0, 0}};
        struct usb_native_ops ops = {.control = native_control};
        uint8_t ram[64];
        assert(!usb_native_window(&ops, &mock, &usb_dt295_window_profile,
            MB86H_DT295_AUX_RAM_ADDRESS, ram, sizeof(ram), false, false));
        for (unsigned i = 0; i < sizeof(ram); ++i) assert(ram[i] == 0xa5);
        assert(!memcmp(mock.mapped, "\0\xf0\0\0", 4));
        assert(!usb_native_window(&ops, &mock, &usb_dt295_window_profile,
            MB86H_DT295_AUX_RAM_ADDRESS, ram, sizeof(ram), true, true));
        assert(mock.block_writes == 1);
        unsigned before = mock.controls;
        assert(usb_native_window(&ops, &mock, &usb_dt295_window_profile,
            MB86H_DT295_AUX_RAM_ADDRESS + MB86H_DT295_AUX_RAM_BYTES - 4,
            ram, 8, false, false) == -22);
        assert(usb_native_window(&ops, &mock, &usb_dt295_window_profile,
            MB86H_DT295_AUX_RAM_ADDRESS - 4, ram, 4, false, false) == -22);
        assert(mock.controls == before);
    }
    return 0;
}
