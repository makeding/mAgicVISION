/* SPDX-License-Identifier: MIT */
#include "mb86H/receiver_core.h"

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

static int enqueue(void *opaque, const uint8_t packet[MB86H_TS_PACKET_SIZE])
{
    struct mb86H_ts_receiver_core *core = opaque;
    size_t tail;

    if (core->used == core->capacity)
        return MB86H_TS_RECEIVER_OVERFLOW;
    tail = (core->head + core->used) % core->capacity;
    memcpy(core->packets + tail * MB86H_TS_PACKET_SIZE, packet, MB86H_TS_PACKET_SIZE);
    ++core->used;
    return 0;
}
static int mux_receive(void *opaque,unsigned channel,const uint8_t *data,size_t size)
{
    struct mb86H_ts_receiver_core *core = opaque;
    int discard = core->discard_mux_frame;
    if (core->expected_source < MB86H_CHANNEL_COUNT &&
        channel != core->expected_source)
        return MB86H_MUX_INVALID;
    core->discard_mux_frame = 0;
    if (!discard)
        mb86H_ts_receiver_core_receive(core, 0, data, size);
    return core->stats.error;
}
int mb86H_ts_receiver_mux_receive(struct mb86H_ts_mux *mux,
                                struct mb86H_ts_receiver_core *core,
                                const uint8_t *data, size_t size)
{
    int rc;
    if (!core) return MB86H_TS_RECEIVER_ARGUMENT;
    rc = mb86H_ts_mux_feed(mux, data, size, mux_receive, core);
    if (rc) mb86H_ts_receiver_core_receive(core, rc, NULL, 0);
    return rc;
}

int mb86H_ts_receiver_core_init(struct mb86H_ts_receiver_core *core,
                              uint8_t *storage, size_t capacity)
{
    if (!core || !storage || capacity < 2 || capacity > 32768)
        return MB86H_TS_RECEIVER_ARGUMENT;
    memset(core, 0, sizeof(*core));
    core->packets = storage;
    core->capacity = capacity;
    return 0;
}

int mb86H_ts_receiver_core_start(struct mb86H_ts_receiver_core *core, size_t record_size)
{
    uint64_t generation;

    if (!core || !core->packets || !core->capacity ||
        (record_size != MB86H_TS_PACKET_SIZE && record_size != MB86H_TS_RECORD_SIZE))
        return MB86H_TS_RECEIVER_ARGUMENT;
    if (core->stats.active || core->stats.generation == ~(uint64_t)0)
        return MB86H_TS_RECEIVER_BUSY;
    generation = core->stats.generation + 1;
    memset(&core->stats, 0, sizeof(core->stats));
    core->stats.generation = generation;
    core->stats.active = 1;
    core->head = 0;
    core->used = 0;
    core->stopping = 0;
    core->discard_mux_frame = 0;
    core->expected_source = MB86H_CHANNEL_COUNT;
    (void)mb86H_ts_framer_init_layout(&core->framer, record_size);
    return 0;
}

void mb86H_ts_receiver_core_start_failed(struct mb86H_ts_receiver_core *core, int result)
{
    if (!core)
        return;
    core->stats.active = 0;
    core->stats.error = result;
}

void mb86H_ts_receiver_core_receive(struct mb86H_ts_receiver_core *core, int result,
                                  const uint8_t *data, size_t size)
{
    size_t consumed;

    if (!core || !core->stats.active || core->stopping || core->stats.error)
        return;
    if (!result) {
        core->stats.input_bytes += size;
        result = mb86H_ts_framer_push(&core->framer, data, size, enqueue, core, &consumed);
        /* Loss of RF sync is recoverable framing, not a transport shutdown.
         * The framer keeps its resynchronization state and error counters. */
        if (result == MB86H_TS_FRAMER_DESYNC && consumed == size)
            result = 0;
    }
    if (result)
        core->stats.error = result;
}

int mb86H_ts_receiver_core_waiting(const struct mb86H_ts_receiver_core *core,
                                 uint64_t generation)
{
    return core && !core->used && core->stats.active && !core->stopping &&
           !core->stats.error && generation == core->stats.generation;
}

uint64_t mb86H_ts_receiver_core_generation(const struct mb86H_ts_receiver_core *core)
{
    return core ? core->stats.generation : 0;
}

int mb86H_ts_receiver_core_read(struct mb86H_ts_receiver_core *core,
                              uint64_t generation, uint8_t *output,
                              size_t capacity, size_t *size)
{
    size_t count;

    if (size)
        *size = 0;
    if (!core || !output || capacity < MB86H_TS_PACKET_SIZE || !size)
        return MB86H_TS_RECEIVER_ARGUMENT;
    if (generation != core->stats.generation || core->stopping)
        return MB86H_TS_RECEIVER_STOPPED;
    if (!core->used)
        return core->stats.error ? core->stats.error : MB86H_TS_RECEIVER_STOPPED;
    count = capacity / MB86H_TS_PACKET_SIZE;
    if (count > core->used)
        count = core->used;
    for (size_t i = 0; i < count; ++i) {
        memcpy(output + i * MB86H_TS_PACKET_SIZE,
               core->packets + core->head * MB86H_TS_PACKET_SIZE, MB86H_TS_PACKET_SIZE);
        core->head = (core->head + 1) % core->capacity;
    }
    core->used -= count;
    core->stats.read_packets += count;
    *size = count * MB86H_TS_PACKET_SIZE;
    return 0;
}

void mb86H_ts_receiver_core_stop_begin(struct mb86H_ts_receiver_core *core)
{
    if (!core)
        return;
    core->stopping = 1;
    core->head = 0;
    core->used = 0;
}

void mb86H_ts_receiver_core_stop_finish(struct mb86H_ts_receiver_core *core, int result)
{
    if (!core)
        return;
    if (result)
        core->stats.error = result;
    else
        core->stats.active = 0;
}

void mb86H_ts_receiver_core_discard_buffered(struct mb86H_ts_receiver_core *core)
{
    size_t record_size;

    if (!core)
        return;
    core->head = 0;
    core->used = 0;
    record_size = core->framer.record_size;
    if (record_size != MB86H_TS_PACKET_SIZE && record_size != MB86H_TS_RECORD_SIZE)
        record_size = MB86H_TS_RECORD_SIZE;
    (void)mb86H_ts_framer_init_layout(&core->framer, record_size);
}

void mb86H_ts_receiver_core_stats(const struct mb86H_ts_receiver_core *core,
                                struct mb86H_ts_receiver_stats *stats)
{
    if (!core || !stats)
        return;
    *stats = core->stats;
    stats->packets = core->framer.emitted_packets;
    stats->discarded_bytes = core->framer.discarded_bytes;
    stats->desyncs = core->framer.desyncs;
    stats->queued_packets = core->used;
}
