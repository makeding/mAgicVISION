/* SPDX-License-Identifier: MIT */
/* Native bootstrap inspection over the same libusb owner as the receiver. */
#define _POSIX_C_SOURCE 200809L
#include "mb86H/device.h"
#include "mb86H/usb/libusb.h"
#include "mb86H/loader.h"
#include "mb86H/device_query.h"
#include "mb86H/protocol.h"
#include "mb86H/receiver.h"
#include "mb86H/card.h"
#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>
#include <limits.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define EVIDENCE_LIMIT (8U * 1024U * 1024U)
static volatile sig_atomic_t interrupted;
static void stop(int signal_number) { interrupted = signal_number; }
struct log_context { FILE *file; size_t bytes, limit; unsigned controls; int failed, failures_only; };

static double timestamp(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_REALTIME, &t)) return 0;
    return (double)t.tv_sec + (double)t.tv_nsec / 1000000000.0;
}

static int account(struct log_context *log, int written)
{
    if (written < 0) log->failed = 1;
    else log->bytes += (size_t)written;
    if (log->bytes > (log->limit ? log->limit : EVIDENCE_LIMIT) - 2U * 65536U) log->failed = 1;
    if (fflush(log->file)) log->failed = 1;
    return log->failed;
}

static uint64_t fingerprint(uint64_t hash, const uint8_t *data, size_t size)
{
    for (size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * UINT64_C(1099511628211);
    return hash;
}

static void json_string(struct log_context *log, const char *value)
{
    account(log, fputc('"', log->file) == EOF ? -1 : 1);
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (*p < 32) account(log, fprintf(log->file, "\\u%04x", *p));
        else if (*p == '"' || *p == '\\') account(log, fprintf(log->file, "\\%c", *p));
        else account(log, fputc(*p, log->file) == EOF ? -1 : 1);
    }
    account(log, fputc('"', log->file) == EOF ? -1 : 1);
}

static void command_record(struct log_context *log, const char *cwd,
                            int argc, char **argv, uint32_t address, size_t size)
{
    account(log, fprintf(log->file,
        "{\"event\":\"start\",\"time\":%.6f,\"schema\":1,\"cwd\":", timestamp()));
    json_string(log, cwd);
    account(log, fprintf(log->file, ",\"argv\":["));
    for (int i = 0; i < argc; ++i) {
        if (i) account(log, fputc(',', log->file) == EOF ? -1 : 1);
        json_string(log, argv[i]);
    }
    account(log, fprintf(log->file,
        "],\"budget_bytes\":%u,\"candidate_address\":%u,\"candidate_bytes\":%zu,"
        "\"candidate_verified\":false}\n", (unsigned)(log->limit ? log->limit : EVIDENCE_LIMIT), address, size));
}

static void observe(void *opaque, const struct usb_control_step *step, const uint8_t *data)
{
    struct log_context *log = opaque;
    ++log->controls;
    if (log->failures_only && !step->submit_result && step->transfer_status == 0 &&
        step->actual_length == step->length) return;
    int n = fprintf(log->file,
        "{\"time\":%.6f,\"event\":\"control\",\"type\":%u,\"request\":%u,"
        "\"value\":%u,\"index\":%u,\"requested\":%u,\"actual\":%u,"
        "\"submit_result\":%d,\"transfer_status\":%d,\"timeout_ms\":%u,\"hex\":\"",
        timestamp(), step->request_type, step->request, step->value, step->index,
        step->length, step->actual_length, step->submit_result, step->transfer_status,
        step->timeout_ms);
    if (data) for (unsigned i = 0; i < step->actual_length; ++i) {
        int x = fprintf(log->file, "%02x", data[i]);
        if (x < 0) { n = -1; break; }
        if (n >= 0) n += x;
    }
    int end = fprintf(log->file, "\"}\n");
    (void)account(log, n < 0 || end < 0 ? -1 : n + end);
}

static int parse_id(const char *text, uint16_t *value)
{
    char *end;
    errno = 0;
    unsigned long n = strtoul(text, &end, 16);
    if (errno || !*text || *end || n > 65535) return -1;
    *value = (uint16_t)n;
    return 0;
}

static FILE *new_file(const char *directory, const char *name)
{
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", directory, name) >= (int)sizeof(path)) {
        errno = ENAMETOOLONG; return NULL;
    }
    return fopen(path, "wbx");
}

static int load_image(const char *path, uint8_t **data, size_t *size)
{
    struct stat st;
    FILE *file = fopen(path, "rb");
    int rc = -1;
    if (!file) return -1;
    if (fstat(fileno(file), &st) || !S_ISREG(st.st_mode) ||
        st.st_size < 64 || st.st_size > MB86H_NATIVE_IMAGE_MAX) goto done;
    *size = (size_t)st.st_size;
    *data = malloc(*size);
    if (!*data) goto done;
    if (fread(*data, 1, *size, file) != *size || fgetc(file) != EOF || ferror(file))
        goto done;
    rc = 0;
done:
    if (fclose(file)) rc = -1;
    if (rc) { free(*data); *data = NULL; *size = 0; }
    return rc;
}

/* Shared observation options; hardware-specific register probes are explicit
 * debug capabilities, not alternate tune/stream/lock implementations. */
enum capture_trace_flags {
    TRACE_ACQUISITION=1u, TRACE_LOCK=2u, TRACE_REGISTERS=4u,
    TRACE_SIGNAL=8u, TRACE_TUNE=16u, TRACE_STATE=32u, TRACE_RF=64u
};
/* Platform callbacks only: install/commit/identity/restore stay in core/loader. */
struct firmware_context {
    struct usb_device *device;
    struct log_context *log;
    const char *directory;
    unsigned exchanges;
    struct {
        uint64_t ns;
        unsigned controls, exchanges;
    } stages[MB86H_LOADER_STAGE_COUNT];
    int restoring;
    bool tracing_tune;
    unsigned trace_flags;
    struct mb86H_session *session;
    const struct mb86H_session_ops *session_ops;
};
static uint32_t monotonic_ms(void *opaque)
{
    struct timespec t; (void)opaque;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((uint64_t)t.tv_sec * 1000u + t.tv_nsec / 1000000u);
}
static void delay_ms(void *opaque, unsigned ms)
{
    struct timespec t = {ms / 1000u, (long)(ms % 1000u) * 1000000L};
    (void)opaque; (void)nanosleep(&t, NULL);
}
static void *allocate(void *opaque, size_t n) { (void)opaque; return malloc(n); }
static void release(void *opaque, void *p) { (void)opaque; free(p); }
static int stopped(struct firmware_context *ctx)
{ return !ctx->restoring && (interrupted || ctx->log->failed); }
static int read_window(void *opaque, uint32_t a, void *p, uint16_t n)
{
    struct firmware_context *ctx = opaque;
    return stopped(ctx) ? -ECANCELED : usb_libusb_boot0_read(ctx->device, a, p, n);
}
static int write_window(void *opaque, uint32_t a, const void *p, uint16_t n)
{
    struct firmware_context *ctx = opaque;
    return stopped(ctx) ? -ECANCELED : usb_libusb_boot0_write(ctx->device, a, p, n);
}
static int block_writes(void *opaque, bool enabled)
{
    struct firmware_context *ctx = opaque;
    return stopped(ctx) ? -ECANCELED : usb_libusb_set_block_writes(ctx->device, enabled);
}
static int exchange(void *opaque, const uint8_t tx[64], uint8_t rx[64])
{
    struct firmware_context *ctx = opaque;
    const struct mb86H_transport *transport = usb_libusb_transport(ctx->device);
    int rc;
    if (stopped(ctx)) return -ECANCELED;
    if (!transport) return -EOPNOTSUPP;
    rc = transport->ops->exchange(transport->opaque, tx, rx);
    ++ctx->exchanges;
    bool failed = rc || mb86H_debug_u32be_get(rx + 8) == MB86H_DEBUG_FATAL ||
        !mb86H_debug_response_matches(rx, MB86H_DEBUG_MAGIC, mb86H_debug_u32be_get(tx + 4));
    bool stream_state = ctx->session && ctx->session->streaming &&
        mb86H_debug_u32be_get(tx + 12) == MB86H_RESIDENT_COMMAND_DEVICE_QUERY &&
        tx[24] == MB86H_DEVICE_QUERY_STATE;
    if (failed || stream_state || ctx->exchanges <= 4u || !(ctx->exchanges % 256u)) {
        account(ctx->log, fprintf(ctx->log->file,
            "{\"time\":%.6f,\"event\":\"loader-progress\",\"restoring\":%s,"
            "\"exchanges\":%u,\"controls\":%u,\"result\":%d", timestamp(),
            ctx->restoring ? "true" : "false", ctx->exchanges, ctx->log->controls, rc));
        if (failed || stream_state || ctx->exchanges <= 4u) {
            account(ctx->log, fprintf(ctx->log->file, ",\"request\":\""));
            for (unsigned i = 0; i < 64; ++i) account(ctx->log, fprintf(ctx->log->file, "%02x", tx[i]));
            account(ctx->log, fprintf(ctx->log->file, "\""));
            if (!rc) {
                account(ctx->log, fprintf(ctx->log->file, ",\"response\":\""));
                for (unsigned i=0;i<64;++i) account(ctx->log, fprintf(ctx->log->file,"%02x",rx[i]));
                account(ctx->log, fprintf(ctx->log->file,"\""));
            }
        }
        account(ctx->log, fprintf(ctx->log->file, "}\n"));
    }
    return rc;
}
/* Optional DT295 cached observations use the same serialized session owner. */
static uint64_t monotonic_ns(void);
static void acquisition_observe(void *opaque, const uint8_t request[64],
                                const uint8_t *response, int result)
{
    struct firmware_context *ctx = opaque;
    uint8_t sample[64] = {0};
    if((ctx->trace_flags&TRACE_TUNE) && ctx->tracing_tune && !ctx->restoring && response &&
       mb86H_debug_u32be_get(request+8)==MB86H_DEBUG_RESIDENT_DISPATCH &&
       mb86H_debug_u32be_get(request+12)==MB86H_RESIDENT_COMMAND_SERVICE) {
        static const char digits[]="0123456789abcdef";
        char hex[129];
        for(unsigned i=0;i<64;++i) {
            hex[i*2]=digits[response[i]>>4];hex[i*2+1]=digits[response[i]&15];
        }
        hex[128]=0;
        account(ctx->log,fprintf(ctx->log->file,
            "{\"event\":\"tune-progress\",\"ns\":%" PRIu64 ",\"result\":%d,\"response\":\"%s\"}\n",
            monotonic_ns(),result,hex));
    }
    if (!(ctx->trace_flags&TRACE_ACQUISITION) || !ctx->tracing_tune || ctx->restoring ||
        result || !response || ctx->log->failed ||
        mb86H_debug_u32be_get(request + 8) != MB86H_DEBUG_RESIDENT_DISPATCH ||
        mb86H_debug_u32be_get(request + 12) != MB86H_RESIDENT_COMMAND_SERVICE ||
        mb86H_debug_u32be_get(response + 8) != MB86H_DEBUG_OK)
        return;
    int rc = mb86H_session_command(ctx->session, ctx->session_ops, ctx,
                                  MB86H_RESIDENT_COMMAND_STATUS, 1, NULL, sample);
    if (!rc && (mb86H_debug_u32be_get(sample + 8) != MB86H_DEBUG_OK ||
                sample[24] != 1 || sample[25] != 1)) rc = -EPROTO;
    account(ctx->log, fprintf(ctx->log->file,
        "{\"event\":\"acquisition\",\"ns\":%" PRIu64 ",\"result\":%d,\"response\":\"",
        monotonic_ns(), rc));
    for (unsigned i = 0; i < sizeof(sample); ++i)
        account(ctx->log, fprintf(ctx->log->file, "%02x", sample[i]));
    account(ctx->log, fprintf(ctx->log->file, "\"}\n"));
    if (rc) ctx->log->failed = 1;
}
static int snapshot_write(void *opaque, const void *data, size_t size)
{
    return fwrite(data, 1, size, opaque) == size ? 0 : -EIO;
}
static int snapshot(void *opaque, const struct mb86H_loader *loader)
{
    struct firmware_context *ctx = opaque;
    FILE *f = new_file(ctx->directory, "preimage.bin");
    int rc = 0;
    if (!f) return -errno;
    if (loader->adopted) {
        uint8_t header[20] = {0};
        if (!loader->preserve_all_ram || !loader->resident_saved ||
            !loader->adopted_original_resident) rc = -EINVAL;
        if (!rc) {
            memcpy(header, "MBADOPT1", 8);
            mb86H_debug_u32be_put(header + 8, loader->target);
            memcpy(header + 12, loader->resident_saved +
                   MB86H_RESIDENT_BUILD_ID - MB86H_RESIDENT_BASE, 8);
            rc = snapshot_write(f, header, sizeof(header));
        }
        if (!rc) rc = snapshot_write(f, loader->resident_saved, MB86H_RESIDENT_BYTES);
    } else {
        if (loader->preserve_all_ram && loader->target == MB86H_BOOTSTRAP_DT295) rc = snapshot_write(f, "MBBOOT04", 8);
        else if (loader->preserve_all_ram && loader->is_torne) rc = snapshot_write(f, "MBBOOT03", 8);
        if (!rc) rc = mb86H_loader_snapshot_write(loader, snapshot_write, f);
    }
    long bytes = ftell(f);
    if (bytes < 0) rc = -EIO;
    if (fflush(f) || fsync(fileno(f))) rc = -EIO;
    if (fclose(f)) rc = -EIO;
    if (loader->adopted) account(ctx->log, fprintf(ctx->log->file,
        "{\"event\":\"adopted-preimage\",\"file\":\"preimage.bin\","
        "\"bytes\":%ld,\"result\":%d}\n", bytes, rc));
    return rc;
}
struct capture_options {
    const char *filename;
    size_t max_bytes;
    bool pipe_output;
    unsigned source;
    uint32_t duration_ms;
    struct mb86H_dvb_request request;
};
static uint64_t monotonic_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}
static void loader_observe(void *opaque, const struct mb86H_loader_observation *event)
{
    static const char *const names[MB86H_LOADER_STAGE_COUNT] = {
        "adopt-validate", "hook-snapshot", "resident-snapshot", "write", "readback",
        "commit", "capabilities", "prepare", "initialize", "restore",
    };
    struct firmware_context *ctx = opaque;
    unsigned stage = (unsigned)event->stage;
    uint64_t ns = monotonic_ns();
    if (stage >= MB86H_LOADER_STAGE_COUNT) return;
    if (event->began) {
        ctx->stages[stage].ns = ns;
        ctx->stages[stage].controls = ctx->log->controls;
        ctx->stages[stage].exchanges = ctx->exchanges;
    }
    account(ctx->log, fprintf(ctx->log->file,
        "{\"event\":\"loader-stage\",\"stage\":\"%s\",\"edge\":\"%s\","
        "\"ns\":%" PRIu64 ",\"duration_ms\":%.3f,\"controls\":%u,\"exchanges\":%u,\"status\":%d}\n",
        names[stage], event->began ? "begin" : "end", ns,
        (double)(ns - ctx->stages[stage].ns) / 1000000.0,
        ctx->log->controls - ctx->stages[stage].controls,
        ctx->exchanges - ctx->stages[stage].exchanges, event->status));
}
struct capture_pat {
    uint8_t section[1024];
    size_t used, wanted;
    unsigned count;
    int cc_valid;
    uint8_t cc;
    struct { uint16_t tsid; uint64_t ns; size_t offset; } seen[16];
};
struct capture_writer {
    struct mb86H_ts_receiver *receiver;
    FILE *file;
    size_t bytes, max_bytes;
    uint64_t hash, generation;
    atomic_int error;
    atomic_size_t written_bytes;
    atomic_uint_fast64_t tune_ns;
    uint64_t first_ts_ns;
    struct capture_pat pat;
};
static void capture_pat_bytes(struct capture_pat *pat, const uint8_t *p, size_t n,
                              uint64_t ns, size_t offset)
{
    while (n) {
        if (!pat->used && *p == 0xff) return;
        pat->section[pat->used++] = *p++; --n;
        if (pat->used == 3) {
            pat->wanted = 3 + (((size_t)pat->section[1] & 15) << 8) + pat->section[2];
            if (pat->section[0] || !(pat->section[1] & 0x80) ||
                pat->wanted < 12 || pat->wanted > sizeof(pat->section)) {
                pat->used = pat->wanted = 0; return;
            }
        }
        if (pat->wanted && pat->used == pat->wanted) {
            uint32_t crc = UINT32_MAX;
            for (size_t i = 0; i < pat->wanted; ++i) {
                crc ^= (uint32_t)pat->section[i] << 24;
                for (unsigned bit = 0; bit < 8; ++bit)
                    crc = (crc << 1) ^ ((crc & 0x80000000u) ? 0x04c11db7u : 0);
            }
            if (!crc && (pat->section[5] & 1)) {
                uint16_t tsid = ((uint16_t)pat->section[3] << 8) | pat->section[4];
                unsigned i;
                for (i = 0; i < pat->count; ++i) if (pat->seen[i].tsid == tsid) break;
                if (i == pat->count && i < 16) {
                    pat->seen[i].tsid = tsid; pat->seen[i].ns = ns;
                    pat->seen[i].offset = offset; ++pat->count;
                }
            }
            pat->used = pat->wanted = 0;
        }
    }
}
static void capture_pat_packet(struct capture_pat *pat, const uint8_t *p,
                               uint64_t ns, size_t offset)
{
    unsigned control = (p[3] >> 4) & 3, pos = 4;
    if ((p[1] & 0x80) || (p[1] & 31) || p[2] || (p[3] & 0xc0)) return;
    if (control & 2) {
        if (p[4] && (p[5] & 0x80)) { pat->used = pat->wanted = 0; pat->cc_valid = 0; }
        pos += 1u + p[4];
    }
    if (!(control & 1) || pos >= 188) return;
    unsigned cc = p[3] & 15;
    if (pat->cc_valid && cc == pat->cc) return; /* Duplicate PSI payload. */
    if (pat->cc_valid && cc != ((pat->cc + 1u) & 15)) pat->used = pat->wanted = 0;
    pat->cc = (uint8_t)cc; pat->cc_valid = 1;
    if (p[1] & 0x40) {
        unsigned pointer = p[pos++];
        if (pointer > 188 - pos) { pat->used = pat->wanted = 0; return; }
        if (pat->used) capture_pat_bytes(pat, p + pos, pointer, ns, offset);
        pos += pointer;
        pat->used = pat->wanted = 0;
    } else if (!pat->used) return;
    capture_pat_bytes(pat, p + pos, 188 - pos, ns, offset);
}
static void *capture_write(void *opaque)
{
    struct capture_writer *writer = opaque;
    uint8_t bytes[188 * 256];
    for (;;) {
        size_t size;
        uint64_t generation;
        int rc = mb86H_ts_receiver_read(writer->receiver, bytes, sizeof(bytes),
                                        &size, &generation);
        uint64_t observed_ns = monotonic_ns();
        if (rc == MB86H_TS_RECEIVER_STOPPED) break;
        if (rc) { atomic_store(&writer->error, rc); break; }
        if (writer->generation && writer->generation != generation) {
            atomic_store(&writer->error, -ESTALE); break;
        }
        writer->generation = generation;
        if (size > writer->max_bytes - writer->bytes) {
            atomic_store(&writer->error, -EFBIG); break;
        }
        uint64_t tune_ns = atomic_load_explicit(&writer->tune_ns, memory_order_relaxed);
        if (tune_ns && observed_ns >= tune_ns) {
            if (!writer->first_ts_ns && size) writer->first_ts_ns = observed_ns;
            for (size_t i = 0; i + 188 <= size; i += 188)
                capture_pat_packet(&writer->pat, bytes + i, observed_ns, writer->bytes + i);
        }
        size_t actual = fwrite(bytes, 1, size, writer->file);
        writer->hash = fingerprint(writer->hash, bytes, actual);
        writer->bytes += actual;
        atomic_store_explicit(&writer->written_bytes, writer->bytes, memory_order_relaxed);
        if (actual != size) { atomic_store(&writer->error, -EIO); break; }
    }
    return NULL;
}
/* Private debug-image observation, serialized by the existing USB owner. */
static int register_observe(struct firmware_context *ctx,struct mb86H_session *session)
{
    static const uint8_t ranges[][2]={{0x80,8},{0x8b,3},{0x96,1},{0xb0,1},{0xb2,8},{0xac,1},{0xda,1}};
    for(unsigned range=0;range<sizeof(ranges)/sizeof(ranges[0]);++range) {
        uint8_t request[64]={0},reply[64]={0};
        if(session->sequence==UINT32_MAX)return -EOVERFLOW;
        mb86H_debug_u32be_put(request,MB86H_DEBUG_MAGIC);
        mb86H_debug_u32be_put(request+4,++session->sequence);
        mb86H_debug_u32be_put(request+8,MB86H_DEBUG_I2C);
        mb86H_debug_u32be_put(request+12,0x18);
        mb86H_debug_u32be_put(request+16,1u|((uint32_t)ranges[range][1]<<8));
        mb86H_debug_u32be_put(request+20,100000u);
        request[24]=ranges[range][0];
        int rc=exchange(ctx,request,reply);
        if(!rc && (!mb86H_debug_response_matches(reply,MB86H_DEBUG_MAGIC,session->sequence) ||
            mb86H_debug_u32be_get(reply+8)!=MB86H_DEBUG_OK ||
            mb86H_debug_u32be_get(reply+12)!=(uint32_t)ranges[range][1]+1u))rc=-EPROTO;
        account(ctx->log,fprintf(ctx->log->file,
            "{\"event\":\"xz-rf-observation\",\"ns\":%" PRIu64 ",\"generation\":%u,\"register\":%u,\"count\":%u,\"result\":%d,\"response\":\"",
            monotonic_ns(),session->generation,ranges[range][0],ranges[range][1],rc));
        for(unsigned i=0;i<64;++i)account(ctx->log,fprintf(ctx->log->file,"%02x",reply[i]));
        account(ctx->log,fprintf(ctx->log->file,"\"}\n"));
        if(rc)return rc;
    }
    return 0;
}
/* One RF observation path for every device, using the public query contract. */
static int source_observe(struct firmware_context *ctx,struct mb86H_session *session,
    const struct mb86H_session_ops *ops,unsigned source)
{
    int result=0;
    if(ctx->trace_flags&(TRACE_STATE|TRACE_RF)) {
        struct mb86H_source_state state={0};
        int measured=mb86H_device_query_state(session,ops,ctx,source,&state);
        account(ctx->log,fprintf(ctx->log->file,
            "{\"event\":\"lock-observation\",\"source\":%u,\"result\":%d,"
            "\"generation\":%u,\"phase\":%u,\"flags\":%u,\"error\":%d,\"owner_error\":%d}\n",
            source,measured,state.generation,state.phase,state.flags,state.error,state.owner_error));
        if(measured)result=measured;
    }
    if(ctx->trace_flags&(TRACE_SIGNAL|TRACE_RF)) {
        struct mb86H_source_signal sample={0};
        int measured=mb86H_device_query_signal(session,ops,ctx,source,&sample);
        account(ctx->log,fprintf(ctx->log->file,
            "{\"event\":\"signal-observation\",\"source\":%u,\"result\":%d,\"generation\":%u,\"kind\":%u,\"raw\":%u,\"aux\":%u}\n",
            source,measured,sample.generation,sample.kind,sample.raw,sample.aux));
        if(measured && measured!=MB86H_FE_UNSUPPORTED && !result)result=measured;
    }
    return result;
}
static int capture_firmware(struct firmware_context *ctx,
    struct mb86H_session *session, const struct mb86H_session_ops *ops,
    const struct mb86H_device_overview *overview,
    const struct capture_options *options)
{
    struct mb86H_source_descriptor descriptor;
    struct mb86H_ts_receiver *receiver = NULL;
    struct capture_writer writer = {.hash = UINT64_C(14695981039346656037),
                                    .max_bytes = options->max_bytes};
    pthread_t thread;
    int rc, drain = 0, stopped_rc = 0;
    bool writer_started = false;
    unsigned source = options->source;
    if (!(overview->stream_mask & (1u << source))) return -EOPNOTSUPP;
    rc = mb86H_device_query_source(session, ops, ctx, source, &descriptor);
    if (rc) return rc;
    struct mb86H_dvb_caps caps = {
        .frequency_min = descriptor.frequency_min, .frequency_max = descriptor.frequency_max,
        .present = 1, .record_size = descriptor.record_size, .mux_channel = source,
        .auto_tsid = !!(descriptor.flags & MB86H_DEVICE_QUERY_AUTO_TSID),
    };
    if (!mb86H_dvb_request_valid(&caps, &options->request)) return -EINVAL;
    /* Window-grant capture has a separate owner; this path receives autonomous USB output. */
    if (descriptor.window_records) return -EOPNOTSUPP;
    if (options->pipe_output) {
        int fd = dup(3);
        if (fd < 0) return -errno;
        writer.file = fdopen(fd, "wb");
        if (!writer.file) { int error = errno; close(fd); return -error; }
    } else writer.file = new_file(ctx->directory, options->filename);
    if (!writer.file) return -errno;
    rc = mb86H_ts_receiver_create(usb_libusb_transport(ctx->device), 32768, &receiver);
    if (!rc) rc = mb86H_ts_receiver_start_source(receiver, source, &descriptor);
    writer.receiver = receiver;
    if (!rc) {
        int err = pthread_create(&thread, NULL, capture_write, &writer);
        rc = err ? -err : 0;
        writer_started = !rc;
    }
    /* Submit reads before the shared TUNE can start producing, including ACQUIRE. */
    uint64_t tune_begin_ns = 0;
    if (!rc) {
        tune_begin_ns = monotonic_ns();
        atomic_store_explicit(&writer.tune_ns, tune_begin_ns, memory_order_relaxed);
        ctx->tracing_tune = true;
        rc = mb86H_session_tune(session, ops, ctx, source, &options->request, true);
        ctx->tracing_tune = false;
    }
    uint64_t tune_return_ns = monotonic_ns();
    account(ctx->log, fprintf(ctx->log->file,
        "{\"time\":%.6f,\"event\":\"capture-tune\",\"file\":\"%s\",\"source\":%u,"
        "\"result\":%d,\"session_generation\":%u,\"written_bytes\":%zu}\n",
        timestamp(), options->filename, source, rc, session->generation,
        atomic_load_explicit(&writer.written_bytes, memory_order_relaxed)));
    uint32_t started = monotonic_ms(ctx);
    unsigned rf_sample=0;
    static const uint32_t rf_due[]={200,800,2000,3500};
    while (!rc && !stopped(ctx) &&
           (uint32_t)(monotonic_ms(ctx) - started) < options->duration_ms) {
        rc = atomic_load(&writer.error);
        if(!rc && (ctx->trace_flags&(TRACE_RF|TRACE_REGISTERS)) && rf_sample<4 &&
           (uint32_t)(monotonic_ms(ctx)-started)>=rf_due[rf_sample]) {
            rc=source_observe(ctx,session,ops,source);
            if(!rc && (ctx->trace_flags&TRACE_REGISTERS) && source==0)
                rc=register_observe(ctx,session);
            ++rf_sample;
        }
        if (!rc) {
            int health=mb86H_session_stream_health(session, ops, ctx);
            if (health<0) rc=health;
            else delay_ms(ctx, health==MB86H_FE_WAIT ? 1 : 100);
        }
    }
    if(!stopped(ctx) && !ctx->log->failed) {
        int measured=source_observe(ctx,session,ops,source);
        if(measured && !rc)rc=measured;
    }
    if ((ctx->trace_flags&TRACE_LOCK) && !stopped(ctx) && !ctx->log->failed) {
        uint8_t data[40]={MB86H_RESIDENT_SOURCE_LOCK_OBSERVE},reply[64]={0};
        int observed=mb86H_session_command(session,ops,ctx,
            MB86H_RESIDENT_COMMAND_SOURCE_STATUS,source,data,reply);
        account(ctx->log,fprintf(ctx->log->file,
            "{\"event\":\"xz-lock-observation\",\"result\":%d,\"response\":\"",observed));
        for(unsigned i=0;i<64;++i)account(ctx->log,fprintf(ctx->log->file,"%02x",reply[i]));
        account(ctx->log,fprintf(ctx->log->file,"\"}\n"));
        if(observed && !rc)rc=observed;
    }
    if (!rc && stopped(ctx)) rc = -ECANCELED;
    uint64_t drain_begin_ns = monotonic_ns();
    if (receiver) drain = mb86H_ts_receiver_stop(receiver);
    if (writer_started) pthread_join(thread, NULL);
    uint64_t drain_end_ns = monotonic_ns();
    if (!rc) rc = atomic_load(&writer.error);
    account(ctx->log, fprintf(ctx->log->file,
        "{\"event\":\"capture-timing\",\"file\":\"%s\",\"boundary\":\"host-writer\","
        "\"tune_begin_ns\":%" PRIu64 ",\"tune_return_ns\":%" PRIu64 ","
        "\"tune_return_ms\":%.6f,\"first_ts_ns\":%" PRIu64 ",\"first_ts_ms\":%.6f,\"pat\":[",
        options->filename, tune_begin_ns, tune_return_ns,
        tune_begin_ns ? (double)(tune_return_ns - tune_begin_ns) / 1e6 : -1.,
        writer.first_ts_ns, writer.first_ts_ns ? (double)(writer.first_ts_ns - tune_begin_ns) / 1e6 : -1.));
    for (unsigned i = 0; i < writer.pat.count; ++i)
        account(ctx->log, fprintf(ctx->log->file,
            "%s{\"tsid\":%u,\"first_ns\":%" PRIu64 ",\"first_ms\":%.6f,\"offset\":%zu}",
            i ? "," : "", writer.pat.seen[i].tsid, writer.pat.seen[i].ns,
            (double)(writer.pat.seen[i].ns - tune_begin_ns) / 1e6, writer.pat.seen[i].offset));
    account(ctx->log, fprintf(ctx->log->file, "]}\n"));
    /* Cleanup ignores cancellation, but never retries a failed semantic request. */
    ctx->restoring = 1;
    uint64_t stop_begin_ns = monotonic_ns(), stop_ack_ns = 0, stop_end_ns = 0;
    uint32_t stop_sequence = session->sequence;
    if (session->request_valid) {
        stopped_rc = mb86H_session_stop(session, ops, ctx, source);
        stop_ack_ns = monotonic_ns();
        if (!stopped_rc) stopped_rc = mb86H_session_wait_stopped(session, ops, ctx);
        stop_end_ns = monotonic_ns();
    }
    account(ctx->log, fprintf(ctx->log->file,
        "{\"event\":\"capture-stop\",\"file\":\"%s\",\"source\":%u,"
        "\"drain_ms\":%.6f,\"stop_ack_ms\":%.6f,\"stop_settle_ms\":%.6f,"
        "\"stop_total_ms\":%.6f,\"command_count\":%u,\"result\":%d}\n",
        options->filename, source, (double)(drain_end_ns - drain_begin_ns) / 1e6,
        stop_ack_ns ? (double)(stop_ack_ns - stop_begin_ns) / 1e6 : -1.,
        stop_end_ns ? (double)(stop_end_ns - stop_ack_ns) / 1e6 : -1.,
        stop_end_ns ? (double)(stop_end_ns - stop_begin_ns) / 1e6 : -1.,
        session->sequence - stop_sequence, stopped_rc));
    if (!drain && receiver) drain = mb86H_ts_receiver_destroy(receiver);
    /* A failed cancel retains the receiver until process exit; never free callback state. */
    if (drain) {
        fprintf(stderr, "RX drain failed=%d; receiver ownership retained\n", drain);
    }
    if (fflush(writer.file) || ferror(writer.file)) { if (!rc) rc = -EIO; }
    if (fclose(writer.file) && !rc) rc = -EIO;
    if (!rc) rc = drain ? drain : stopped_rc ? stopped_rc : writer.bytes ? 0 : -ENODATA;
    account(ctx->log, fprintf(ctx->log->file,
        "{\"event\":\"capture\",\"file\":\"%s\",\"source\":%u,\"format\":%u,\"record_size\":%u,"
        "\"duration_ms\":%u,\"bytes\":%zu,\"generation\":%" PRIu64 ","
        "\"fingerprint_algorithm\":\"fnv1a64\",\"fingerprint\":\"%016" PRIx64 "\","
        "\"result\":%d,\"drain_result\":%d,\"stop_result\":%d}\n",
        options->filename, source, descriptor.stream_format, descriptor.record_size, options->duration_ms,
        writer.bytes, writer.generation, writer.hash, rc, drain, stopped_rc));
    printf("capture_result=%d bytes=%zu generation=%" PRIu64 " drain_result=%d stop_result=%d\n",
           rc, writer.bytes, writer.generation, drain, stopped_rc);
    return rc;
}

static bool card_probe_enabled;
static int card_command(void *opaque,unsigned operation,const uint8_t data[40],
                        uint8_t response[64])
{
    struct firmware_context *ctx=opaque;
    return mb86H_session_command(ctx->session,ctx->session_ops,ctx,
        MB86H_RESIDENT_COMMAND_CARD,operation,data,response);
}
static uint32_t card_now(void *opaque)
{return monotonic_ms(opaque);}
static void card_delay(void *opaque,unsigned milliseconds)
{delay_ms(opaque,milliseconds);}

static int download_firmware(struct usb_device *device, struct mb86H_loader *loader,
                              struct log_context *log, const char *directory,
                              const struct mb86H_loader *alternative,
                              bool initialize, const struct capture_options *capture_options, unsigned capture_count)
{
    const struct mb86H_loader original = *loader;
    struct firmware_context ctx = {.device = device, .log = log, .directory = directory};
    struct mb86H_session session = {0};
    const struct mb86H_session_ops session_ops = {
        .exchange = exchange, .observe = acquisition_observe,
        .now_ms = monotonic_ms, .delay_ms = delay_ms,
    };
    const struct mb86H_loader_ops ops = {
        .window_read = read_window, .window_write = write_window, .session = &session,
        .session_ops = &session_ops, .now_ms = monotonic_ms, .delay_ms = delay_ms,
        .allocate = allocate, .release = release, .snapshot = snapshot, .block_writes = block_writes,
        .observe = loader_observe,
        .preserve_all_ram = !initialize,
    };
    struct mb86H_device_overview overview = {0};
    ctx.session = &session; ctx.session_ops = &session_ops;
    static const struct { const char *environment; unsigned flags, target; } trace_options[]={
        {"MB86H_DT295_ACQUISITION_TRACE",TRACE_ACQUISITION,MB86H_BOOTSTRAP_DT295},
        {"MB86H_XZ_LOCK_TRACE",TRACE_LOCK,MB86H_BOOTSTRAP_XZ},
        {"MB86H_XZ_RF_TRACE",TRACE_REGISTERS|TRACE_LOCK,MB86H_BOOTSTRAP_XZ},
        {"MB86H_SIGNAL_TRACE",TRACE_SIGNAL,0},
        {"MB86H_TUNE_TRACE",TRACE_TUNE,0},
        {"MB86H_LOCK_TRACE",TRACE_STATE,0},
        {"MB86H_RF_TRACE",TRACE_RF,0},
    };
    for(unsigned i=0;i<sizeof(trace_options)/sizeof(trace_options[0]);++i) {
        const char *value=getenv(trace_options[i].environment);
        if(value && !strcmp(value,"1") &&
           (!trace_options[i].target || trace_options[i].target==loader->target))
            ctx.trace_flags|=trace_options[i].flags;
    }
    account(log, fprintf(log->file, "{\"event\":\"acquisition-trace\",\"enabled\":%s}\n",
                         (ctx.trace_flags&TRACE_ACQUISITION) ? "true" : "false"));
    int rc = 0;
    for (unsigned round=0;round<(alternative?(capture_count?2u:3u):1u);++round) {
        const struct mb86H_loader *selected=round==1?alternative:&original;
        uint8_t reply[64]={0};
        rc=round?mb86H_loader_update(loader,&ops,&ctx,selected):
            initialize?mb86H_loader_start(loader,&ops,&ctx):
            mb86H_loader_install(loader,&ops,&ctx);
        if (!rc) {
            loader_observe(&ctx, &(struct mb86H_loader_observation){MB86H_LOADER_PREPARE, 1, 0});
            rc=mb86H_session_prepare(&session,&session_ops,&ctx,&overview);
            loader_observe(&ctx, &(struct mb86H_loader_observation){MB86H_LOADER_PREPARE, 0, rc});
        }
        if (alternative) {
            if (!rc) rc=mb86H_session_command(&session,&session_ops,&ctx,
                MB86H_RESIDENT_COMMAND_CAPABILITIES,0,NULL,reply);
            const uint8_t *expected=selected->resident+MB86H_RESIDENT_BUILD_ID-MB86H_RESIDENT_BASE;
            if (!rc && (mb86H_debug_u32be_get(reply+8)!=MB86H_DEBUG_OK ||
                memcmp(reply+MB86H_DEBUG_PACKET_DATA_OFFSET+16,expected,8) ||
                memcmp(reply+MB86H_DEBUG_PACKET_DATA_OFFSET+24,expected,8))) rc=-EPROTO;
            account(log,fprintf(log->file,"{\"event\":\"ota-round\",\"round\":%u,\"result\":%d,\"expected_build_id\":\"",round,rc));
            for (unsigned i=0;i<8;++i)account(log,fprintf(log->file,"%02x",expected[i]));
            account(log,fprintf(log->file,"\",\"observed_build_id\":\""));
            for (unsigned i=0;i<8;++i)account(log,fprintf(log->file,"%02x",reply[40+i]));
            account(log,fprintf(log->file,"\",\"compiled_build_id\":\""));
            for (unsigned i=0;i<8;++i)account(log,fprintf(log->file,"%02x",reply[48+i]));
            account(log,fprintf(log->file,"\",\"owner_adopted\":%s}\n",
                loader->adopted ? "true" : "false"));
            printf("ota_round=%u result=%d build_id=",round,rc);
            for(unsigned i=0;i<8;++i)printf("%02x",reply[48+i]);
            putchar('\n');
            if(!rc && card_probe_enabled && round==1) {
                struct mb86H_card card;
                uint8_t icc=0xff, atr[MB86H_CARD_MAX_ATR];
                size_t atr_length=sizeof atr;
                const struct mb86H_card_ops card_ops={card_command,card_now,card_delay};
                mb86H_card_init(&card,&card_ops,&ctx);
                int status_rc=mb86H_card_status(&card,&icc);
                int reset_rc=status_rc?status_rc:mb86H_card_reset(&card,atr,&atr_length);
                int off_rc=0;
                if(!reset_rc || card.active) off_rc=mb86H_card_power_off(&card);
                account(log,fprintf(log->file,
                    "{\"event\":\"card-probe\",\"round\":1,\"status_result\":%d,\"icc\":%u,"
                    "\"reset_result\":%d,\"firmware_result\":%d,\"atr_length\":%zu,"
                    "\"power_off_result\":%d}\n",
                    status_rc,(unsigned)icc,reset_rc,card.firmware_result,
                    reset_rc?0:atr_length,off_rc));
                printf("card_status=%d icc=%u card_reset_result=%d firmware_result=%d atr_len=%zu card_power_off=%d\n",
                    status_rc,(unsigned)icc,reset_rc,card.firmware_result,reset_rc?0:atr_length,off_rc);
            }
        }
        if(rc)break;
    }
    if (!rc && alternative && capture_count) {
        loader_observe(&ctx, &(struct mb86H_loader_observation){MB86H_LOADER_INITIALIZE, 1, 0});
        rc=mb86H_session_initialize(&session,&session_ops,&ctx);
        loader_observe(&ctx, &(struct mb86H_loader_observation){MB86H_LOADER_INITIALIZE, 0, rc});
    }
    account(log, fprintf(log->file,
        "{\"time\":%.6f,\"event\":\"firmware\",\"result\":%d,\"identity\":\"%s\","
        "\"rf_mask\":%u,\"stream_mask\":%u}\n",
        timestamp(), rc, rc ? "" : MB86H_FIRMWARE_IDENTITY,
        overview.rf_mask, overview.stream_mask));
    int firmware_rc = rc;
    for (unsigned segment = 0; !rc && segment < capture_count; ++segment) {
        ctx.restoring = 0;
        account(log, fprintf(log->file,
            "{\"event\":\"capture-segment\",\"index\":%u,\"count\":%u}\n",
            segment + 1, capture_count));
        rc = capture_firmware(&ctx, &session, &session_ops, &overview,
                              capture_options + segment);
    }
    ctx.restoring = 1;
    int restored = initialize ? mb86H_loader_close(loader, &ops, &ctx) :
                                    mb86H_loader_restore(loader, &ops, &ctx);
    account(log, fprintf(log->file,
        "{\"time\":%.6f,\"event\":\"%s\",\"result\":%d,\"exchanges\":%u,\"controls\":%u}\n",
        timestamp(), initialize ? "firmware-close" : "restore", restored, ctx.exchanges, log->controls));
    printf("firmware_result=%d identity=%s %s_result=%d\n",
           firmware_rc, firmware_rc ? "unverified" : MB86H_FIRMWARE_IDENTITY,
           initialize ? "firmware_close" : "restore", restored);
    return rc ? rc : restored;
}

int main(int argc, char **argv)
{
    uint16_t vendor, product;
    struct usb_device *device = NULL;
    struct usb_boot_result result = {0};
    struct mb86H_loader loader = {0}, alternative = {0};
    struct usb_libusb_identity identity;
    struct log_context log = {.file = stderr};
    const struct mb86H_device_info *info;
    int rc = 0, close_rc = 0, capture, prepare, download, check_image, ota, record, sequence, start, ota_record, ota_sequence;
    struct capture_options recording[4] = {{0}};
    unsigned capture_count = 0;
    const char *directory = NULL;
    FILE *raw[2] = {NULL, NULL};
    uint8_t *first = NULL;
    uint8_t *image = NULL, *image_b = NULL;
    size_t image_size = 0, image_b_size = 0;
    size_t written[2] = {0}, different = 0, first_difference = SIZE_MAX;
    uint64_t hashes[2] = {UINT64_C(14695981039346656037), UINT64_C(14695981039346656037)};
    if ((argc != 4 && argc != 5 && argc != 6 && argc != 7 && argc != 10 && argc != 11 && argc != 18 && argc != 19 && argc != 22 && argc != 23) || parse_id(argv[2], &vendor) ||
        parse_id(argv[3], &product)) goto usage;
    capture = !strcmp(argv[1], "read-boot0");
    prepare = !strcmp(argv[1], "prepare");
    card_probe_enabled = !strcmp(argv[1], "ota-card");
    ota_sequence = !strcmp(argv[1], "ota-capture-sequence");
    ota_record = !strcmp(argv[1], "ota-capture-pipe") || ota_sequence;
    ota = !strcmp(argv[1], "ota") || card_probe_enabled || ota_record;
    sequence = !strcmp(argv[1], "capture-sequence") || ota_sequence;
    record = sequence || !strcmp(argv[1], "capture") || !strcmp(argv[1], "capture-pipe") || !strcmp(argv[1], "ota-capture-pipe");
    start = !strcmp(argv[1], "start");
    capture_count = record ? (sequence ? (unsigned)(argc-(ota_sequence?7:6))/4u : 1u) : 0u;
    download = start || record || ota || !strcmp(argv[1], "download");
    check_image = !strcmp(argv[1], "check-image");
    if ((download && (record ? (sequence ? (ota_sequence ? (argc != 19 && argc != 23) : (argc != 18 && argc != 22)) : argc != (ota_record ? 11 : 10)) : argc != (ota ? 7 : 6))) || ((capture || check_image) && argc != 5) ||
        (!(capture || download || check_image) && argc != 4) ||
        (!capture && !prepare && !download && !check_image && strcmp(argv[1], "identify") &&
         strcmp(argv[1], "plan") && strcmp(argv[1], "observe"))) goto usage;
    for (unsigned segment = 0; segment < capture_count; ++segment) {
        unsigned base = sequence ? (ota_sequence?7:6) + segment * 4 : ota_record ? 6 : 5;
        char *end;
        unsigned long values[4];
        for (unsigned i = 0; i < 4; ++i) {
            if (i == 2 && !strcmp(argv[base + 2], "auto")) { values[i] = UINT32_MAX; continue; }
            errno = 0;
            values[i] = strtoul(argv[base + i], &end, i == 2 ? 16 : 10);
            if (errno || !*argv[base + i] || *end || values[i] > UINT32_MAX) goto usage;
        }
        if (values[0] >= MB86H_CHANNEL_COUNT || !values[1] ||
            !values[3] || values[3] > INT32_MAX ||
            (values[2] != UINT32_MAX && values[2] > 65535)) goto usage;
        static const char *names[] = {"capture-01.ts", "capture-02.ts", "capture-03.ts", "capture-04.ts"};
        recording[segment].filename = sequence ? names[segment] : "capture.ts";
        recording[segment].pipe_output = !strcmp(argv[1], "capture-pipe") || !strcmp(argv[1], "ota-capture-pipe");
        recording[segment].max_bytes = recording[segment].pipe_output ? SIZE_MAX :
            512U * 1024U * 1024U / capture_count;
        if (recording[segment].pipe_output) {
            log.limit = 64U * 1024U * 1024U;
            signal(SIGPIPE, SIG_IGN);
        }
        recording[segment].source = (unsigned)values[0];
        recording[segment].duration_ms = (uint32_t)values[3];
        recording[segment].request = (struct mb86H_dvb_request){
            .system = values[0] ? MB86H_DVB_BSCS : MB86H_DVB_GR,
            .frequency = (uint32_t)values[1], .stream_id = (uint32_t)values[2],
            .layers = values[0] ? 0 : 7,
        };
    }
    info = mb86H_device_find(vendor, product);
    if (!info) { fprintf(stderr, "Unknown device %04x:%04x\n", vendor, product); return 3; }
    printf("%s %04x:%04x runtime_protocol=%u\n", info->name, vendor, product,
           (unsigned)info->runtime_protocol);
    if (!strcmp(argv[1], "identify")) return 0;
    if (download || check_image) {
        if (info->runtime_protocol != MB86H_RUNTIME_RESIDENT ||
            info->firmware_target == MB86H_FIRMWARE_NONE) {
            fprintf(stderr, "Shared resident loader binding unavailable for this target\n"); return 3;
        }
        if (load_image(argv[4], &image, &image_size)) {
            fprintf(stderr, "Cannot read a bounded firmware bundle: %s\n", argv[4]); return 1;
        }
        rc = mb86H_device_firmware_parse(info, &loader, image, image_size);
        if (rc) {
            fprintf(stderr, "Invalid resident firmware bundle: status=%d\n", rc); free(image); return 1;
        }
        if (ota) {
            if (load_image(argv[5],&image_b,&image_b_size) ||
                (rc=mb86H_device_firmware_parse(info,&alternative,image_b,image_b_size))) {
                fprintf(stderr,"Invalid alternate firmware bundle: status=%d\n",rc);
                free(image_b);free(image);return 1;
            }
            rc=mb86H_loader_update_validate(&loader,&alternative);
            if(rc) {
                fprintf(stderr,"Incompatible OTA gateway: status=%d\n",rc);
                free(image_b);free(image);return 1;
            }
            if (!memcmp(loader.resident+MB86H_RESIDENT_BUILD_ID-MB86H_RESIDENT_BASE,
                alternative.resident+MB86H_RESIDENT_BUILD_ID-MB86H_RESIDENT_BASE,8)) {
                fprintf(stderr,"OTA requires distinct compiled build identities\n");
                free(image_b);free(image);return 1;
            }
        }
        if (check_image) {
            printf("firmware_valid=true bytes=%zu magic=%.4s fnv1a64=%016" PRIx64 "\n",
                   image_size, image, fingerprint(UINT64_C(14695981039346656037), image, image_size));
            free(image); return 0;
        }
    }
    if (!download && !info->usb_boot) { fprintf(stderr, "Bootstrap observation unavailable\n"); free(image); return 3; }
    if (!strcmp(argv[1], "plan")) {
        printf("candidate_address=%#x candidate_bytes=%zu candidate_verified=false\n",
               info->usb_boot->candidate_address, info->usb_boot->candidate_size);
        printf("firmware_formats=SBC2,XZFB resident_loader=%s\n",
               info->runtime_protocol == MB86H_RUNTIME_RESIDENT ? "available" : "unavailable");
        return 0;
    }
    umask(0077);
    if (download) {
        directory=argv[ota_sequence?6:sequence?5:ota_record?10:record?9:ota?6:5];
        if (mkdir(directory, 0700)) { perror("create firmware evidence directory"); free(image); return 1; }
        log.file = new_file(directory, "control.jsonl");
        if (!log.file) { perror("create firmware log"); free(image); return 1; }
        log.failures_only = 1;
    } else if (capture) {
        if (!info->usb_boot->candidate_size || info->usb_boot->candidate_size > 65536U) return 3;
        if (mkdir(argv[4], 0700)) { perror("create evidence directory"); return 1; }
        log.file = new_file(argv[4], "control.jsonl");
        if (!log.file) { perror("create evidence log"); return 1; }
        raw[0] = new_file(argv[4], "candidate-a.bin");
        raw[1] = new_file(argv[4], "candidate-b.bin");
        if (!raw[0] || !raw[1]) { perror("create candidate files"); rc = 1; goto done; }
        first = malloc(info->usb_boot->candidate_size);
        if (!first) { perror("allocate candidate comparison"); rc = 1; goto done; }
    }
    signal(SIGINT, stop); signal(SIGTERM, stop);
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) { perror("getcwd"); rc = 1; goto done; }
    command_record(&log, cwd, argc, argv,
                    info->usb_boot ? info->usb_boot->candidate_address : 0u,
                    info->usb_boot ? info->usb_boot->candidate_size : 0u);
    if (download) account(&log, fprintf(log.file,
        "{\"event\":\"image\",\"bytes\":%zu,\"magic\":\"%.4s\","
        "\"fingerprint_algorithm\":\"fnv1a64\",\"fingerprint\":\"%016" PRIx64 "\"}\n",
        image_size, image, fingerprint(UINT64_C(14695981039346656037), image, image_size)));
    if (log.failed) { rc = 1; goto done; }
    rc = usb_libusb_open_target(&device, vendor, product);
    account(&log, fprintf(log.file, "{\"time\":%.6f,\"event\":\"open\",\"result\":%d}\n",
                         timestamp(), rc));
    if (rc) goto done;
    usb_libusb_get_identity(device, &identity);
    account(&log, fprintf(log.file,
        "{\"event\":\"identity\",\"vendor\":%u,\"product\":%u,\"bcd_device\":%u,"
        "\"bus\":%u,\"address\":%u}\n", identity.vendor, identity.product,
        identity.bcd_device, identity.bus, identity.address));
    rc = usb_libusb_observe_control(device, observe, &log);
    if (rc) goto done;
    if (download) {
        rc = download_firmware(device, &loader, &log, directory, ota?&alternative:NULL, !ota_record && (start || record), record?recording:NULL, capture_count);
        goto done;
    } else rc = prepare ? usb_libusb_prepare_boot0(device, &result) :
                   usb_libusb_observe_boot0(device, &result);
    account(&log, fprintf(log.file,
        "{\"time\":%.6f,\"event\":\"boot\",\"result\":%d,\"stage\":%u,"
        "\"failed_stage\":%u,\"raw_state\":%u,\"ready_status\":%u,\"pending_status\":%u,"
        "\"code_signature_verified\":%s}\n",
        timestamp(), rc, (unsigned)result.stage, (unsigned)result.failed_stage,
        result.rawstate, result.ready_status, result.pending_status,
        result.code_signature_verified ? "true" : "false"));
    struct usb_libusb_receive_status received;
    usb_libusb_get_receive_status(device, &received);
    if (received.submitted || received.descriptor_result) {
        account(&log, fprintf(log.file,
            "{\"event\":\"receive\",\"endpoint\":%u,\"type\":%u,\"submit_result\":%d,"
            "\"transfer_status\":%d,\"actual\":%d,\"descriptor_result\":%d,"
            "\"requested\":%u,\"timeout_ms\":%u,\"hex\":\"", received.endpoint,
            received.transfer_type, received.submit_result, received.transfer_status,
            received.actual_length, received.descriptor_result, received.requested_length,
            received.timeout_ms));
        for (int i = 0; i < received.actual_length && i < (int)sizeof(received.bytes); ++i)
            account(&log, fprintf(log.file, "%02x", received.bytes[i]));
        account(&log, fprintf(log.file, "\"}\n"));
    }
    if (rc || log.failed || interrupted || !capture) goto done;
    for (unsigned pass = 0; pass < 2 && !rc; ++pass) {
        for (size_t offset = 0; offset < info->usb_boot->candidate_size; offset += 64) {
            uint8_t bytes[64];
            size_t n = info->usb_boot->candidate_size - offset;
            if (n > sizeof(bytes)) n = sizeof(bytes);
            if (interrupted || log.failed) { rc = 1; break; }
            rc = usb_libusb_boot0_read(device,
                info->usb_boot->candidate_address + (uint32_t)offset, bytes, n);
            if (rc) break;
            size_t actual = fwrite(bytes, 1, n, raw[pass]);
            hashes[pass] = fingerprint(hashes[pass], bytes, actual);
            written[pass] += actual;
            if (!pass) memcpy(first + offset, bytes, actual);
            else for (size_t i = 0; i < actual; ++i) if (first[offset + i] != bytes[i]) {
                if (first_difference == SIZE_MAX) first_difference = offset + i;
                different++;
            }
            if (actual != n) { rc = 1; break; }
        }
    }
done:
    if (device) close_rc = usb_libusb_close(device);
    for (unsigned i = 0; i < 2; ++i) if (raw[i] && fclose(raw[i]) && !rc) rc = 1;
    if (!rc && close_rc) rc = close_rc;
    if (!rc && (interrupted || log.failed)) rc = 1;
    if (capture) {
        for (unsigned i = 0; i < 2; ++i)
            account(&log, fprintf(log.file,
                "{\"event\":\"artifact\",\"file\":\"candidate-%c.bin\",\"bytes\":%zu,"
                "\"expected_bytes\":%zu,\"fingerprint_algorithm\":\"fnv1a64\","
                "\"fingerprint\":\"%016" PRIx64 "\",\"complete\":%s}\n", 'a' + i,
                written[i], info->usb_boot->candidate_size, hashes[i],
                written[i] == info->usb_boot->candidate_size ? "true" : "false"));
        if (written[0] == info->usb_boot->candidate_size && written[1] == written[0]) {
            account(&log, fprintf(log.file,
                "{\"event\":\"comparison\",\"different_bytes\":%zu,\"first_difference\":",
                different));
            if (first_difference == SIZE_MAX) account(&log, fprintf(log.file, "null"));
            else account(&log, fprintf(log.file, "%zu", first_difference));
            account(&log, fprintf(log.file, ",\"candidate_verified\":false}\n"));
        }
    }
    free(first);
    free(image);
    free(image_b);
    account(&log, fprintf(log.file,
        "{\"time\":%.6f,\"event\":\"exit\",\"result\":%d,\"close_result\":%d,"
        "\"signal\":%d,\"candidate_verified\":false}\n", timestamp(), rc, close_rc, interrupted));
    if (log.failed) rc = 1;
    if (log.file != stderr && fclose(log.file)) rc = 1;
    return rc ? 1 : 0;
usage:
    fprintf(stderr, "usage: usbboot identify|plan|observe|prepare VID PID\n"
                    "       usbboot read-boot0 VID PID NEW_EVIDENCE_DIRECTORY\n"
                    "       usbboot check-image VID PID FIRMWARE_BUNDLE\n"
                    "       usbboot download VID PID FIRMWARE_BUNDLE NEW_EVIDENCE_DIRECTORY\n"
                    "       usbboot start VID PID FIRMWARE_BUNDLE NEW_EVIDENCE_DIRECTORY\n"
                    "       usbboot ota VID PID FIRMWARE_A FIRMWARE_B NEW_EVIDENCE_DIRECTORY\n"
                    "       usbboot ota-card VID PID FIRMWARE_A FIRMWARE_B NEW_EVIDENCE_DIRECTORY\n"
                    "       usbboot capture VID PID BUNDLE SOURCE FREQUENCY TSID|auto DURATION_MS NEW_DIRECTORY\n"
                    "       usbboot ota-capture-pipe VID PID A B SOURCE FREQUENCY TSID|auto DURATION_MS NEW_DIRECTORY (TS to fd 3)\n"
                    "       usbboot capture-pipe VID PID BUNDLE SOURCE FREQUENCY TSID|auto DURATION_MS NEW_DIRECTORY (TS to fd 3)\n"
                    "       usbboot ota-capture-sequence VID PID A B NEWDIR S F TSID MS S F TSID MS S F TSID MS [S F TSID MS]\n"
                    "       usbboot capture-sequence VID PID BUNDLE NEWDIR S F TSID MS S F TSID MS S F TSID MS [S F TSID MS]\n"
                    "start initializes and restores patches; download/ota preserve and restore all RAM.\n");
    return 2;
}
