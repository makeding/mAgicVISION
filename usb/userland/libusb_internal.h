/* SPDX-License-Identifier: MIT */
/* Private state and cross-unit helpers for the libusb backend.  Included only
 * by usb/userland/libusb_*.c; the public surface stays in mb86H/usb/libusb.h. */
#ifndef MB86H_USB_LIBUSB_INTERNAL_H
#define MB86H_USB_LIBUSB_INTERNAL_H

#define _POSIX_C_SOURCE 200809L

#include "mb86H/usb/libusb.h"
#include "mb86H/device.h"
#include "mb86H/usb/window.h"
#include "../host_contract.h"

#include <libusb.h>
#include <pthread.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <errno.h>
#include <time.h>

#define USB_STREAM_FLUSH_MS 100

/* Kept local for the implementation-including lifecycle test. */
enum { USB_MAX_STREAMS = USB_ENDPOINT_SLOTS };

#ifdef USB_LIBUSB_TESTING
extern int (*test_submit_transfer)(struct libusb_transfer *transfer);
extern int (*test_cancel_transfer)(struct libusb_transfer *transfer);
extern int (*test_endpoint_type)(uint8_t endpoint, uint8_t *type);
extern int (*test_clear_halt)(uint8_t endpoint);
#endif

struct control_wait {
    pthread_mutex_t mutex;
    pthread_cond_t complete;
    int done;
    enum libusb_transfer_status status;
    int actual;
};

struct usb_stream {
    struct usb_device *device;
    struct libusb_transfer *transfer;
    pthread_mutex_t mutex;
    pthread_cond_t drained;
    usb_stream_callback callback;
    void *opaque;
    struct usb_stream_stats stats;
    int submitted;
    int cancelling;
    int cancel_requested;
    int in_callback;
};

struct usb_device {
    struct mb86H_transport transport;
    libusb_context *context;
    libusb_device_handle *handle;
    pthread_t event_thread;
    pthread_mutex_t control_mutex;
    pthread_mutex_t streams_mutex;
    uint16_t vendor;
    uint16_t product;
    const struct mb86H_device_info *info;
    struct usb_libusb_identity identity;
    struct usb_libusb_receive_status receive_status;
    struct usb_xz_read_cache xz_read_cache;
    usb_control_observer observer;
    void *observer_opaque;
    usb_native_observer native_observer;
    void *native_observer_opaque;
    const struct mb86H_native_download_options *download_options;
    atomic_bool native_taken_over;
    bool torne_block_writes;
    atomic_bool closing;
    int event_started;
    struct usb_stream streams[USB_ENDPOINT_SLOTS];
};

/* libusb_control.c */
int submit_transfer(struct libusb_transfer *transfer);
int cancel_transfer(struct libusb_transfer *transfer);
void control_done(struct libusb_transfer *transfer);
int control_locked(struct usb_device *device, struct usb_control_step *steps,
                   size_t step_count, unsigned timeout_ms);

/* libusb_device.c */
bool native_boot_only(const struct usb_device *device);
bool is_torne(const struct usb_device *device);
const struct usb_native_window_profile *native_window_profile(
    const struct usb_device *device);
int receive_type(libusb_device *device, uint8_t endpoint, uint8_t *type);

/* libusb_boot.c */
int native_result(int rc);
extern const struct usb_native_ops native_ops;

#endif
