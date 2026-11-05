#include "mb86H/loader.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct sink { size_t bytes; unsigned calls, fail_at; uint64_t hash; };
static int write_part(void *opaque, const void *data, size_t size)
{
    struct sink *sink = opaque;
    const uint8_t *p = data;
    size_t i;
    if (++sink->calls == sink->fail_at) return -77;
    for (i = 0; i < size; ++i) sink->hash = (sink->hash ^ p[i]) * UINT64_C(1099511628211);
    sink->bytes += size;
    return 0;
}
static struct sink empty_sink(void)
{
    return (struct sink){.hash = UINT64_C(14695981039346656037)};
}
int main(void)
{
    struct mb86H_loader loader = {.preserve_all_ram = true};
    struct sink sink = empty_sink();
    loader.resident_saved = malloc(MB86H_RESIDENT_BYTES);
    loader.torne_boot0_saved = malloc(MB86H_TORNE_BOOT0_CODE_BYTES);
    loader.torne_ts_ring_saved = malloc(MB86H_TORNE_TS_RING_BYTES);
    assert(loader.resident_saved && loader.torne_boot0_saved && loader.torne_ts_ring_saved);
    memset(loader.hook, 0x11, sizeof(loader.hook));
    memset(loader.body, 0x22, sizeof(loader.body));
    memset(loader.ep0_hook, 0x33, sizeof(loader.ep0_hook));
    memset(loader.staging, 0x44, sizeof(loader.staging));
    memset(loader.resident_saved, 0x55, MB86H_RESIDENT_BYTES);
    assert(mb86H_loader_snapshot_size(&loader) == 264548u);
    assert(!mb86H_loader_snapshot_write(&loader, write_part, &sink));
    assert(sink.bytes == 264548u && sink.calls == 5u);
    /* Golden byte layout of the released XZ raw preimage / MBSCREC2 payload. */
    assert(sink.hash == UINT64_C(0x7336bafebfd66a0d));
    sink = empty_sink(); sink.fail_at = 2;
    assert(mb86H_loader_snapshot_write(&loader, write_part, &sink) == -77 && sink.calls == 2);
    loader.adopted = true; sink = empty_sink();
    assert(!mb86H_loader_snapshot_size(&loader));
    assert(mb86H_loader_snapshot_write(&loader, write_part, &sink) == -EINVAL && !sink.calls);
    loader.adopted = false; loader.is_torne = true;
    assert(!mb86H_loader_snapshot_size(&loader));
    assert(mb86H_loader_snapshot_write(&loader, write_part, &sink) == -EINVAL && !sink.calls);
    loader.torne_ts_ring_snapshot = true;
    memset(loader.torne_hook, 0x11, sizeof(loader.torne_hook));
    memset(loader.torne_body, 0x22, sizeof(loader.torne_body));
    memset(loader.torne_out_original, 0x33, sizeof(loader.torne_out_original));
    memset(loader.torne_state, 0x44, sizeof(loader.torne_state));
    memset(loader.torne_execution, 0x55, sizeof(loader.torne_execution));
    memset(loader.torne_boot0_saved, 0x66, MB86H_TORNE_BOOT0_CODE_BYTES);
    memset(loader.staging, 0x77, sizeof(loader.staging));
    memset(loader.resident_saved, 0x88, MB86H_RESIDENT_BYTES);
    memset(loader.torne_ts_ring_saved, 0x99, MB86H_TORNE_TS_RING_BYTES);
    assert(mb86H_loader_snapshot_size(&loader) == 665460u);
    assert(!mb86H_loader_snapshot_write(&loader, write_part, &sink));
    assert(sink.bytes == 665460u && sink.calls == 9u);
    /* MBBOOT03 / MBSCREC4 payload, including all 393216 bytes of TS ring. */
    assert(sink.hash == UINT64_C(0x5b867cfa002b125d));
    loader.preserve_all_ram = false;
    assert(!mb86H_loader_snapshot_size(&loader));
    loader.patches_saved = true;
    sink = empty_sink();
    assert(mb86H_loader_snapshot_size(&loader) == 2140u);
    assert(!mb86H_loader_snapshot_write(&loader, write_part, &sink));
    assert(sink.bytes == 2140u);
    loader.is_torne = false;
    sink = empty_sink();
    assert(mb86H_loader_snapshot_size(&loader) == 2412u);
    assert(!mb86H_loader_snapshot_write(&loader, write_part, &sink));
    assert(sink.bytes == 2412u);
    free(loader.torne_ts_ring_saved);
    free(loader.torne_boot0_saved);
    free(loader.resident_saved);
    return 0;
}
