#!/usr/bin/env python3
"""Exercise production per-reader composite bind and string ownership."""
import argparse
from pathlib import Path
import subprocess
p=argparse.ArgumentParser()
p.add_argument("--cc",default="cc")
p.add_argument("--out",type=Path,required=True)
a=p.parse_args()
source=(Path(__file__).resolve().parents[1]/"linux/ccid.c").read_text()
start=source.index("static int ccid_composite_bind(")
end=source.index("\nstatic void ccid_reader_free",start)
production=source[start:end]
prelude=r"""
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#define USB_GADGET_MANUFACTURER_IDX 0
#define USB_GADGET_PRODUCT_IDX 1
#define USB_GADGET_SERIAL_IDX 2
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
struct device { const char *name; struct device *parent; };
static const char *dev_name(struct device *d) { return d->name; }
struct usb_gadget { struct device dev; };
struct usb_string { uint8_t id; const char *s; };
struct usb_gadget_strings { uint16_t language; struct usb_string *strings; };
struct usb_device_descriptor { uint8_t iManufacturer,iProduct,iSerialNumber; };
struct usb_configuration { int unused; };
struct usb_composite_driver { struct usb_gadget_strings **strings; };
struct usb_composite_dev { struct usb_composite_driver *driver; struct usb_gadget *gadget; };
struct ccid_reader {
 struct usb_composite_driver driver; struct usb_configuration config;
 struct usb_device_descriptor device; struct usb_string strings[4];
 char udc_name[32];
};
static int ccid_config_bind(struct usb_configuration *c) { (void)c; return 0; }
static int ids_error,config_error,config_calls;
static int usb_string_ids_tab(struct usb_composite_dev *c,struct usb_string *s) {
 (void)c;if(ids_error)return ids_error;
 for(unsigned i=0;s[i].s;++i)s[i].id=(uint8_t)(i+1);
 return 0;
}
static int usb_add_config(struct usb_composite_dev *c,struct usb_configuration *cfg,
                         int (*bind)(struct usb_configuration *)) {
 (void)c;++config_calls;return config_error?config_error:bind(cfg);
}
"""
checks=r"""
int main(void) {
 struct device udc0={.name="dummy_udc.0"},udc1={.name="dummy_udc.1"},physical={.name="physical-udc"};
 struct usb_gadget g0={.dev.parent=&udc0},g1={.dev.parent=&udc1};
 struct ccid_reader a={.udc_name="dummy_udc.0"},b={.udc_name="dummy_udc.1"};
 struct usb_gadget_strings la={.strings=a.strings},lb={.strings=b.strings};
 struct usb_gadget_strings *ta[]={&la,NULL},*tb[]={&lb,NULL};
 a.driver.strings=ta;b.driver.strings=tb;
 a.strings[0].s=b.strings[0].s="mb86H project";
 a.strings[1].s="XZ";b.strings[1].s="Sony";
 a.strings[2].s="04bb:0530:1-2";b.strings[2].s="054c:04b2:1-5";
 struct usb_composite_dev ca={&a.driver,&g0},cb={&b.driver,&g1};
 ids_error=-12;assert(ccid_composite_bind(&ca)==-12 && !config_calls);
 ids_error=0;config_error=-19;assert(ccid_composite_bind(&ca)==-19);
 config_error=0;assert(!ccid_composite_bind(&ca));assert(!ccid_composite_bind(&cb));
 assert(a.device.iSerialNumber==3 && b.device.iSerialNumber==3);
 assert(!strcmp(a.driver.strings[0]->strings[1].s,"XZ"));
 assert(!strcmp(b.driver.strings[0]->strings[1].s,"Sony"));
 assert(a.driver.strings[0]->strings!=b.driver.strings[0]->strings);
 assert(!a.driver.strings[1] && !b.driver.strings[1]);
 assert(!a.strings[3].s && !b.strings[3].s);
 ca.gadget=&g1;assert(ccid_composite_bind(&ca)==-ENODEV);
 ca.gadget=&g0;g0.dev.parent=&physical;assert(ccid_composite_bind(&ca)==-ENODEV);
 puts("CCID independent identities, bind failure, string lifetime and dummy-only binding: passed");
}
"""
a.out.parent.mkdir(parents=True,exist_ok=True)
unit=a.out.with_suffix(".c");unit.write_text(prelude+production+checks)
subprocess.run([a.cc,"-std=c11","-Wall","-Wextra","-Werror",str(unit),"-o",str(a.out)],check=True)
subprocess.run([str(a.out)],check=True)
