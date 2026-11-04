#!/usr/bin/env python3
"""Exercise production CCID admission and module startup without a gadget UDC."""
import argparse
from pathlib import Path
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--cc', default='cc')
p.add_argument('--out', type=Path, required=True)
a = p.parse_args()
source = (Path(__file__).resolve().parents[1] / 'usb/linux/transport.c').read_text()
reader = source[source.index('static void usb_register_reader('):
                source.index('static int usb_probe(')]
startup = source[source.index('static int __init usb_host_init('):
                 source.index('static void __exit usb_host_exit(')]
prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
#include <stdarg.h>
#include <string.h>
#define __init
struct device { int unused; };
struct usb_interface { struct device dev; };
struct physical_device { unsigned vendor, product; const char *name; };
struct usb_device { const char *serial; struct device dev; };
struct usb_host {
    const struct physical_device *device;
    struct usb_device *udev;
    struct usb_interface *intf;
    bool ccid_registered;
};
static int usb_ccid_ops, usb_host_driver;
static int card_result, card_calls, driver_result, driver_calls, warnings;
static const char *dev_name(const struct device *d) { (void)d; return "1-3"; }
static void warning(const char *format, ...) { (void)format; ++warnings; }
#define dev_warn(device, ...) ((void)(device), warning(__VA_ARGS__))
#define pr_warn(...) warning(__VA_ARGS__)
static int mb86H_ccid_register(const int *ops, void *owner, const char *name, const char *serial) {
    assert(ops == &usb_ccid_ops && owner && name);
    assert(!strcmp(serial, "04bb:0530:1-3"));
    ++card_calls;
    return card_result;
}
static int usb_register(int *driver) { assert(driver == &usb_host_driver); ++driver_calls; return driver_result; }
'''
checks = r'''
int main(void) {
    const struct physical_device device = {0x04bb, 0x0530, "XZ"};
    struct usb_device usb = {0};
    struct usb_interface intf = {0};
    struct usb_host first = {&device, &usb, &intf, false}, second = first;
    usb_register_reader(&first);
    assert(first.ccid_registered && card_calls == 1 && warnings == 0);
    const int errors[] = {-ENODEV, -EBUSY, -ENOMEM, -EINVAL};
    for (unsigned i = 0; i < sizeof(errors)/sizeof(errors[0]); ++i) {
        card_result = errors[i];
        int before = card_calls;
        usb_register_reader(&second);
        assert(!second.ccid_registered && first.ccid_registered);
        assert(card_calls == before + 1); /* No retry or other-reader teardown. */
    }
    assert(warnings == 4);
    card_result = 0;
    usb_register_reader(&second); /* Fresh bind/resume can obtain a reader. */
    assert(second.ccid_registered && first.ccid_registered);
    /* USB startup no longer owns or initializes the shared CCID registry. */
    assert(usb_host_init() == 0 && driver_calls == 1 && warnings == 4);
    driver_result = -EIO;
    assert(usb_host_init() == -EIO && driver_calls == 2 && warnings == 4);
    puts("Optional CCID: reader failures isolated; DVB module admission preserved");
}
'''
a.out.parent.mkdir(parents=True, exist_ok=True)
unit = a.out.with_suffix('.c')
unit.write_text(prelude + reader + startup + checks)
subprocess.run([a.cc, '-std=c11', '-Wall', '-Wextra', '-Werror', str(unit),
                '-o', str(a.out)], check=True)
subprocess.run([str(a.out)], check=True)
