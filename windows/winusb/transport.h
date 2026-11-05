#ifndef MB86H_WINDOWS_TRANSPORT_H
#define MB86H_WINDOWS_TRANSPORT_H
#include "host.h"
#include <winusb.h>
#include <stdio.h>
#include "mb86H/loader.h"
#include "mb86H/receiver_core.h"
#include "mb86H/ts_mux.h"
#include "mb86H/usb/window.h"
struct xz_host {
    HANDLE file, stop, ready, worker, service_worker;
    WINUSB_INTERFACE_HANDLE usb;
    CRITICAL_SECTION queue_lock, log_lock, command_lock;
    struct mb86H_loader loader;
    struct mb86H_session session;
    struct usb_xz_read_cache xz_read_cache;
    struct mb86H_ts_receiver_core receiver;
    struct mb86H_ts_mux mux;
    uint8_t *bundle, *storage;
    FILE *snapshot_file;
    int boot0_saved;
    FILE *log;
    volatile LONG transport_error;
    const struct xz_operation *operation;
    const char *progress_stage,*failure_stage;
    ULONGLONG progress_bytes, progress_controls, progress_next, progress_next_controls;
    ULONGLONG operation_started,progress_updated;
    int receiving;
};
int xz_transport_open(struct xz_host *);
void xz_transport_close(struct xz_host *);
int xz_control(struct xz_host *, BYTE, WORD, WORD, void *, WORD, unsigned);
int xz_receive_start(struct xz_host *);
int xz_receive_stop(struct xz_host *);
void xz_log(struct xz_host *, const char *, ...);
int xz_operation_check(struct xz_host *, const struct xz_operation *);
void xz_operation_begin(struct xz_host *, const struct xz_operation *, const char *);
void xz_operation_end(struct xz_host *);
void xz_progress_window(struct xz_host *, const char *, size_t);
void xz_progress_control(struct xz_host *, unsigned);
#endif
