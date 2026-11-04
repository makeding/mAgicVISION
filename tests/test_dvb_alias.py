#!/usr/bin/env python3
"""Exercise the production model alias allocator and failed-publication cleanup."""
import argparse
from pathlib import Path
import subprocess

p = argparse.ArgumentParser()
p.add_argument("--cc", default="cc")
p.add_argument("--out", type=Path, required=True)
a = p.parse_args()
source = (Path(__file__).resolve().parents[1] / "usb/linux/transport.c").read_text()
production = source[source.index("static DEFINE_IDA(usb_xz_alias_ids);"):
                    source.index("static void usb_stop_rx(")]
prelude = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#define USB_TORNE_VENDOR_ID 0x054c
#define GFP_KERNEL 0
struct ida { bool used[8]; };
#define DEFINE_IDA(name) struct ida name
static int ida_alloc(struct ida *ids, int flags) {
    (void)flags;
    for(int i=0;i<8;++i)if(!ids->used[i]){ids->used[i]=true;return i;}
    return -ENOSPC;
}
static void ida_free(struct ida *ids,int i) { assert(ids->used[i]);ids->used[i]=false; }
struct device { void *data; };
struct usb_interface { struct device dev; };
struct device_attribute { int unused; };
/* Mirrors enum mb86H_firmware_target in include/mb86H/device.h. */
enum { MB86H_FIRMWARE_NONE, MB86H_FIRMWARE_XZ, MB86H_FIRMWARE_TORNE, MB86H_FIRMWARE_DT295 };
struct device_info { unsigned vendor; int firmware_target; };
struct usb_host {
    struct usb_interface *intf;
    const struct device_info *device;
    struct ida *alias_ids;
    int alias_id;
    bool alias_registered;
    char alias[24];
};
static struct usb_interface *to_usb_interface(struct device *dev) { return (void *)dev; }
static void *usb_get_intfdata(struct usb_interface *intf) { return intf->dev.data; }
static int sysfs_emit(char *buf,const char *format,...) {
    va_list args;va_start(args,format);int n=vsprintf(buf,format,args);va_end(args);return n;
}
#define DEVICE_ATTR_RO(name) struct device_attribute dev_attr_##name
static int create_error,created,removed;
static int device_create_file(struct device *dev,struct device_attribute *attr) {
    (void)dev;(void)attr;
    if(create_error)return create_error;
    ++created;return 0;
}
static void device_remove_file(struct device *dev,struct device_attribute *attr) {
    (void)dev;(void)attr;++removed;
}
"""
checks = r"""
int main(void) {
    const struct device_info xz={0x04bb,MB86H_FIRMWARE_XZ},
        torne={USB_TORNE_VENDOR_ID,MB86H_FIRMWARE_TORNE};
    struct usb_interface ai={0},bi={0},ci={0},di={0};
    struct usb_host a={.intf=&ai,.device=&xz,.alias_id=-1};
    struct usb_host b={.intf=&bi,.device=&torne,.alias_id=-1};
    struct usb_host c={.intf=&ci,.device=&xz,.alias_id=-1};
    struct usb_host d={.intf=&di,.device=&torne,.alias_id=-1};
    ai.dev.data=&a;bi.dev.data=&b;ci.dev.data=&c;di.dev.data=&d;
    /* Interleaved global enumeration must not consume another model's zero. */
    assert(!usb_alias_register(&a) && !strcmp(a.alias,"xz0"));
    assert(!usb_alias_register(&b) && !strcmp(b.alias,"torne0"));
    assert(!usb_alias_register(&c) && !strcmp(c.alias,"xz1"));
    assert(!usb_alias_register(&d) && !strcmp(d.alias,"torne1"));
    char buf[32];assert(mb86H_alias_show(&bi.dev,NULL,buf)==7 && !strcmp(buf,"torne0\n"));
    usb_alias_remove(&a);assert(a.alias_id==-1 && !a.alias_registered);
    usb_alias_remove(&a);assert(removed==1);
    create_error=-ENOMEM;
    assert(usb_alias_register(&a)==-ENOMEM && a.alias_id==-1 && !a.alias_registered);
    assert(removed==1);create_error=0;
    assert(!usb_alias_register(&a) && !strcmp(a.alias,"xz0"));
    assert(c.alias_id==1 && b.alias_id==0 && d.alias_id==1);
    usb_alias_remove(&a);usb_alias_remove(&b);usb_alias_remove(&c);usb_alias_remove(&d);
    assert(created==5 && removed==5);
    for(unsigned i=0;i<8;++i)assert(!usb_xz_alias_ids.used[i] && !usb_torne_alias_ids.used[i]);
    bi.dev.data=NULL;assert(mb86H_alias_show(&bi.dev,NULL,buf)==-ENODEV);
    puts("DVB aliases: model-local zero, multiple devices, reuse and publication failure passed");
}
"""
a.out.parent.mkdir(parents=True, exist_ok=True)
unit = a.out.with_suffix(".c")
unit.write_text(prelude + production + checks)
subprocess.run([a.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", str(unit),
                "-o", str(a.out)], check=True)
subprocess.run([str(a.out)], check=True)
