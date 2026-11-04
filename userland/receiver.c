/* SPDX-License-Identifier: MIT */
#include "mb86H/receiver.h"


#include <pthread.h>
#include <stdlib.h>

struct mb86H_ts_receiver {
    const struct mb86H_transport *transport;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    void *stream;
    struct mb86H_ts_mux mux;
    struct mb86H_ts_receiver_core core;
    uint8_t buffer[16384];
    int draining;
    unsigned int format;
};

static int receive(void *opaque, int result, const uint8_t *data, size_t size)
{
    struct mb86H_ts_receiver *r = opaque;
    int terminal = 0;
    pthread_mutex_lock(&r->mutex);
    if (!result) {
        if (r->format == MB86H_STREAM_FORMAT_XZM1)
            result = mb86H_ts_receiver_mux_receive(&r->mux, &r->core, data, size);
        else {
            mb86H_ts_receiver_core_receive(&r->core, 0, data, size);
            result = r->core.stats.error;
        }
    }
    if (result) {
        terminal = 1;
        mb86H_ts_receiver_core_receive(&r->core, result, NULL, 0);
    }
    pthread_cond_broadcast(&r->changed);
    pthread_mutex_unlock(&r->mutex);
    return terminal;
}

int mb86H_ts_receiver_create(const struct mb86H_transport *transport, size_t capacity,
                           struct mb86H_ts_receiver **out)
{
    struct mb86H_ts_receiver *r;
    if (!out)
        return MB86H_TS_RECEIVER_ARGUMENT;
    *out = NULL;
    if (!transport || !transport->ops || !transport->ops->receive_start ||
        !transport->ops->receive_stop || capacity < 2 || capacity > 32768)
        return MB86H_TS_RECEIVER_ARGUMENT;
    r = calloc(1, sizeof(*r));
    if (!r)
        return MB86H_TS_RECEIVER_NOMEM;
    r->transport = transport;
    if (pthread_mutex_init(&r->mutex, NULL)) { free(r); return MB86H_TS_RECEIVER_NOMEM; }
    if (pthread_cond_init(&r->changed, NULL)) {
        pthread_mutex_destroy(&r->mutex); free(r); return MB86H_TS_RECEIVER_NOMEM;
    }
    uint8_t *storage = malloc(capacity * MB86H_TS_PACKET_SIZE);
    if (storage) {
        mb86H_ts_receiver_core_init(&r->core, storage, capacity);
        *out = r;
        return 0;
    }
    pthread_cond_destroy(&r->changed);
    pthread_mutex_destroy(&r->mutex);
    free(r);
    return MB86H_TS_RECEIVER_NOMEM;
}

static int start(struct mb86H_ts_receiver *r, unsigned int format,
                 unsigned int source, size_t record_size)
{
    void *stream = NULL;
    int rc;
    if (!r || (format != MB86H_STREAM_FORMAT_XZM1 &&
               format != MB86H_STREAM_FORMAT_RAW_TS))
        return MB86H_TS_RECEIVER_ARGUMENT;
    pthread_mutex_lock(&r->mutex);
    if (r->stream || r->draining) {
        pthread_mutex_unlock(&r->mutex);
        return MB86H_TS_RECEIVER_BUSY;
    }
    rc = mb86H_ts_receiver_core_start(&r->core, record_size);
    if (rc) {
        pthread_mutex_unlock(&r->mutex);
        return rc;
    }
    r->format = format;
    r->core.expected_source = source;
    mb86H_ts_mux_reset(&r->mux);
    pthread_mutex_unlock(&r->mutex);
    /* Start/cancel may invoke callbacks synchronously. Never hold the lock. */
    rc = r->transport->ops->receive_start(r->transport->opaque,
                               r->buffer, sizeof(r->buffer),
                               receive, r, &stream);
    pthread_mutex_lock(&r->mutex);
    r->stream = stream;
    if (rc)
        mb86H_ts_receiver_core_start_failed(&r->core, rc);
    pthread_cond_broadcast(&r->changed);
    pthread_mutex_unlock(&r->mutex);
    return rc;
}

int mb86H_ts_receiver_start(struct mb86H_ts_receiver *r,
                          enum mb86H_ts_layout layout)
{
    switch (layout) {
    case MB86H_TS_LAYOUT_OBSERVED_188_PLUS_4:
        return start(r, MB86H_STREAM_FORMAT_XZM1, MB86H_CHANNEL_COUNT,
                     MB86H_TS_RECORD_SIZE);
    case MB86H_TS_LAYOUT_XZM1_188:
        return start(r, MB86H_STREAM_FORMAT_XZM1, MB86H_CHANNEL_COUNT,
                     MB86H_TS_PACKET_SIZE);
    case MB86H_TS_LAYOUT_RAW_188:
        return start(r, MB86H_STREAM_FORMAT_RAW_TS, MB86H_CHANNEL_COUNT,
                     MB86H_TS_PACKET_SIZE);
    default:
        return MB86H_TS_RECEIVER_ARGUMENT;
    }
}

int mb86H_ts_receiver_start_source(struct mb86H_ts_receiver *r,
    unsigned int source, const struct mb86H_source_descriptor *descriptor)
{
    if (!descriptor || source >= MB86H_CHANNEL_COUNT ||
        (descriptor->flags & (MB86H_DEVICE_QUERY_PRESENT | MB86H_DEVICE_QUERY_STREAM)) !=
        (MB86H_DEVICE_QUERY_PRESENT | MB86H_DEVICE_QUERY_STREAM))
        return MB86H_TS_RECEIVER_ARGUMENT;
    return start(r, descriptor->stream_format, source, descriptor->record_size);
}

int mb86H_ts_receiver_read(struct mb86H_ts_receiver *r, uint8_t *output,
                         size_t capacity, size_t *size, uint64_t *generation)
{
    uint64_t session;
    int result;
    if (size) *size = 0;
    if (!r || !output || capacity < MB86H_TS_PACKET_SIZE ||
        !size || !generation)
        return MB86H_TS_RECEIVER_ARGUMENT;
    pthread_mutex_lock(&r->mutex);
    session = mb86H_ts_receiver_core_generation(&r->core);
    while (mb86H_ts_receiver_core_waiting(&r->core, session))
        pthread_cond_wait(&r->changed, &r->mutex);
    *generation = session;
    result = mb86H_ts_receiver_core_read(&r->core, session, output, capacity, size);
    pthread_mutex_unlock(&r->mutex);
    return result;
}

int mb86H_ts_receiver_stats(struct mb86H_ts_receiver *r,
                          struct mb86H_ts_receiver_stats *stats)
{
    if (!r || !stats) return MB86H_TS_RECEIVER_ARGUMENT;
    pthread_mutex_lock(&r->mutex);
    mb86H_ts_receiver_core_stats(&r->core, stats);
    pthread_mutex_unlock(&r->mutex);
    return 0;
}

int mb86H_ts_receiver_discard_buffered(struct mb86H_ts_receiver *r)
{
    if (!r) return MB86H_TS_RECEIVER_ARGUMENT;
    pthread_mutex_lock(&r->mutex);
    mb86H_ts_receiver_core_discard_buffered(&r->core);
    r->core.discard_mux_frame = r->mux.used != 0;
    pthread_mutex_unlock(&r->mutex);
    return 0;
}

int mb86H_ts_receiver_stop(struct mb86H_ts_receiver *r)
{
    void *stream;
    int rc;
    if (!r) return MB86H_TS_RECEIVER_ARGUMENT;
    pthread_mutex_lock(&r->mutex);
    mb86H_ts_receiver_core_stop_begin(&r->core);
    pthread_cond_broadcast(&r->changed);
    stream = r->stream;
    r->draining = stream != NULL;
    pthread_mutex_unlock(&r->mutex);
    rc = stream ? r->transport->ops->receive_stop(r->transport->opaque, stream) : 0;
    pthread_mutex_lock(&r->mutex);
    if (!rc) { r->stream = NULL; r->draining = 0; }
    mb86H_ts_receiver_core_stop_finish(&r->core, rc);
    pthread_mutex_unlock(&r->mutex);
    return rc;
}

int mb86H_ts_receiver_destroy(struct mb86H_ts_receiver *r)
{
    int rc;
    if (!r) return MB86H_TS_RECEIVER_ARGUMENT;
    rc = mb86H_ts_receiver_stop(r);
    if (rc) return rc;
    free(r->core.packets);
    pthread_cond_destroy(&r->changed);
    pthread_mutex_destroy(&r->mutex);
    free(r);
    return 0;
}
