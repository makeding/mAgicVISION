#include "mb86H/ts_framer.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__); \
        return 1; \
    } \
} while (0)

struct packets {
    uint8_t bytes[8][MB86H_TS_PACKET_SIZE];
    size_t count;
    int fail_next;
};

static void record(uint8_t out[MB86H_TS_RECORD_SIZE], uint8_t prefix, uint8_t id)
{
    size_t i;
    out[0] = 0x47;
    for (i = 1; i < MB86H_TS_PACKET_SIZE; ++i)
        out[i] = (uint8_t)(id + i);
    memset(out + MB86H_TS_PACKET_SIZE, prefix, MB86H_TS_RECORD_SUFFIX_SIZE);
}

static int collect(void *opaque, const uint8_t packet[MB86H_TS_PACKET_SIZE])
{
    struct packets *packets = opaque;
    if (packets->fail_next) {
        packets->fail_next = 0;
        return -123;
    }
    if (packets->count >= 8)
        return -1;
    memcpy(packets->bytes[packets->count++], packet, MB86H_TS_PACKET_SIZE);
    return 0;
}

static int check_packet(const struct packets *packets, size_t index, uint8_t id)
{
    return packets->bytes[index][0] == 0x47 &&
           packets->bytes[index][1] == (uint8_t)(id + 1);
}

static int push_split(struct mb86H_ts_framer *framer, const uint8_t *bytes,
                      size_t size, const size_t *splits, size_t split_count,
                      struct packets *packets)
{
    size_t offset = 0;
    size_t i;
    for (i = 0; i < split_count; ++i) {
        size_t consumed = 0;
        if (splits[i] > size - offset ||
            mb86H_ts_framer_push(framer, bytes + offset, splits[i], collect, packets,
                                &consumed) != 0 || consumed != splits[i])
            return 1;
        offset += splits[i];
    }
    return offset == size ? 0 : 1;
}

int main(void)
{
    uint8_t input[MB86H_TS_RECORD_SIZE * 4];
    uint8_t damaged[MB86H_TS_RECORD_SIZE * 4 + 1];
    const size_t splits[] = { 1, 7, 190, 3, 212, 355 };
    struct mb86H_ts_framer a;
    struct mb86H_ts_framer b;
    struct packets out_a = {0};
    struct packets out_b = {0};
    size_t consumed = 0;
    size_t tail_offset;

    record(input, 0xa0, 0x10);
    record(input + MB86H_TS_RECORD_SIZE, 0xb0, 0x20);
    record(input + 2 * MB86H_TS_RECORD_SIZE, 0xc0, 0x30);
    record(input + 3 * MB86H_TS_RECORD_SIZE, 0xd0, 0x40);

    mb86H_ts_framer_init(&a);
    CHECK(push_split(&a, input, sizeof(input), splits, sizeof(splits) / sizeof(splits[0]), &out_a) == 0);
    CHECK(out_a.count == 4 && a.emitted_packets == 4 && a.desyncs == 0 && a.discarded_bytes == 0);
    CHECK(check_packet(&out_a, 0, 0x10));
    CHECK(check_packet(&out_a, 3, 0x40));

    mb86H_ts_framer_init(&a);
    mb86H_ts_framer_init(&b);
    CHECK(mb86H_ts_framer_push(&a, input, 2 * MB86H_TS_RECORD_SIZE, collect, &out_a, &consumed) == 0 &&
          consumed == 2 * MB86H_TS_RECORD_SIZE);
    CHECK(mb86H_ts_framer_push(&b, input + 2 * MB86H_TS_RECORD_SIZE,
                             2 * MB86H_TS_RECORD_SIZE, collect, &out_b, &consumed) == 0 &&
          consumed == 2 * MB86H_TS_RECORD_SIZE);
    CHECK(out_a.count == 6 && out_b.count == 2);
    CHECK(check_packet(&out_b, 0, 0x30));

    mb86H_ts_framer_init(&a);
    memset(&out_a, 0, sizeof(out_a));
    CHECK(mb86H_ts_framer_push(&a, input, 2 * MB86H_TS_RECORD_SIZE, collect, &out_a, &consumed) == 0);
    damaged[0] = 0x99;
    memcpy(damaged + 1, input + 2 * MB86H_TS_RECORD_SIZE, 2 * MB86H_TS_RECORD_SIZE);
    CHECK(mb86H_ts_framer_push(&a, damaged, 1 + 2 * MB86H_TS_RECORD_SIZE, collect, &out_a,
                             &consumed) == MB86H_TS_FRAMER_DESYNC);
    CHECK(a.desyncs == 1 && out_a.count == 4 && a.discarded_bytes == 1);
    CHECK(check_packet(&out_a, 0, 0x10));
    CHECK(check_packet(&out_a, 3, 0x40));

    mb86H_ts_framer_init(&a);
    memset(&out_a, 0, sizeof(out_a));
    damaged[0] = 0x99;
    damaged[1] = 0x47;
    memcpy(damaged + 2, input + MB86H_TS_RECORD_SIZE,
           3 * MB86H_TS_RECORD_SIZE);
    CHECK(mb86H_ts_framer_push(&a, damaged, 3 * MB86H_TS_RECORD_SIZE + 2, collect, &out_a,
                             &consumed) == 0);
    CHECK(out_a.count == 3 && a.discarded_bytes == 2);

    mb86H_ts_framer_init(&a);
    memset(&out_a, 0, sizeof(out_a));
    out_a.fail_next = 1;
    CHECK(mb86H_ts_framer_push(&a, input, sizeof(input), collect, &out_a, &consumed) == -123);
    CHECK(consumed < sizeof(input) && out_a.count == 0);
    tail_offset = consumed;
    CHECK(mb86H_ts_framer_push(&a, NULL, 0, collect, &out_a, &consumed) == 0 && consumed == 0);
    CHECK(mb86H_ts_framer_push(&a, input + tail_offset, sizeof(input) - tail_offset,
                             collect, &out_a, &consumed) == 0);
    CHECK(consumed == sizeof(input) - tail_offset && out_a.count == 4);

    CHECK(mb86H_ts_framer_push(NULL, input, 1, collect, &out_a, &consumed) == MB86H_TS_FRAMER_ARGUMENT);
    puts("ts framer tests passed");
    return 0;
}
