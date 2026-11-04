#!/usr/bin/env python3
"""Build a self-contained release-FW IFD bundle, never compile firmware."""
import argparse,hashlib,json,plistlib,subprocess,sys,shutil,re
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
wire=dict(re.findall(r'^#define (USB_[A-Z_]+) (0x[0-9a-fA-F]+)U$',
    (ROOT/'include/mb86H/protocol/constants.h').read_text(), re.MULTILINE))
device_ids=dict(re.findall(r'^#define (MB86H_PIX_DT295_(?:VENDOR|PRODUCT)_ID) (0x[0-9a-fA-F]+)u$',
    (ROOT/'include/mb86H/device.h').read_text(), re.MULTILINE))
wire.update(device_ids)
def usb_id(name):
    return f'0x{int(wire[name],16):04x}'
p=argparse.ArgumentParser();p.add_argument('--firmware',type=Path,required=True);a=p.parse_args()
fw=a.firmware.resolve();manifest=fw.with_suffix('.manifest.json')
m=json.loads(manifest.read_text());raw=fw.read_bytes()
assert len(raw)>=64 and raw[:4] in (b'XZFB',b'SBC2') and int.from_bytes(raw[4:8],'big')==1
assert (not m['manifest']['debug'] and m['manifest']['device']=='usb-shared' and
        m['manifest']['targets']==['xz','torne','dt295'] and
        hashlib.sha256(raw).hexdigest()==m['sha256'])
out=ROOT/'build/pcsc';out.mkdir(parents=True,exist_ok=True)
platform='MacOS' if sys.platform=='darwin' else 'Linux'
bundle=out/'ifd-mb86H.bundle';binary=bundle/'Contents'/platform/'ifd-mb86H';binary.parent.mkdir(parents=True,exist_ok=True)
header=out/'firmware_blob.h';header.write_text('#define PCSC_FIRMWARE_SHA256 "'+m['sha256']+'"\n#define PCSC_READER_XZ_NAME "I-O DATA GV-MVP/XZ3 Smart Card Reader"\n#define PCSC_READER_TORNE_NAME "Sony torne CECH-ZD1 J Smart Card Reader"\n#define PCSC_READER_DT295_NAME "PIX-DT295 Smart Card Reader"\nstatic const unsigned char pcsc_firmware[] = {\n'+''.join(','.join(str(v) for v in raw[i:i+32])+',\n' for i in range(0,len(raw),32))+'};\n')
sources=['userland/pcsc/ifd.c','core/card.c','core/card_atr.c','core/bootstrap.c','core/loader.c','core/loader_snapshot.c','core/recovery.c','core/resident_image.c','core/protocol.c','core/session.c','core/device_query.c','core/frontend.c','core/native_download.c','usb/window.c','usb/boot.c','usb/userland/libusb_device.c','usb/userland/libusb_control.c','usb/userland/libusb_stream.c','usb/userland/libusb_boot.c','usb/userland/libusb.c','device/device.c','device/firmware.c','device/gv_mvp_xz/device.c','device/gv_mvp_xz/boot.c','device/torne/device.c','device/pix_dt295/device.c','device/pix_dt295/boot.c']
flags=subprocess.check_output(['pkg-config','--cflags','--libs','libusb-1.0'],text=True).split()
if sys.platform!='darwin':flags+=subprocess.check_output(['pkg-config','--cflags','libpcsclite'],text=True).split()
cmd=['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-O2','-fPIC','-Wall','-Wextra','-Werror','-pthread','-I'+str(ROOT/'include'),'-I'+str(out),*(['-dynamiclib'] if sys.platform=='darwin' else ['-shared']),*[str(ROOT/s) for s in sources],'-o',str(binary),*flags]
r=subprocess.run(cmd,cwd=ROOT,text=True,capture_output=True)
log=dict(command=cmd,cwd=str(ROOT),stdout=r.stdout,stderr=r.stderr,exit=r.returncode,firmware_sha256=m['sha256'])
(out/'build.json').write_text(json.dumps(log,indent=2));print(r.stdout,end='');print(r.stderr,end='');r.check_returncode()
info={'CFBundleExecutable':'ifd-mb86H','CFBundleIdentifier':'org.mb86H.ifd','CFBundleName':'mb86H Smart Card Reader','CFBundleDisplayName':'mb86H Smart Card Reader','CFBundlePackageType':'BNDL','CFBundleInfoDictionaryVersion':'6.0','CFBundleVersion':'0.1.3','CFBundleShortVersionString':'0.1.3','ifdManufacturerString':'mb86H project','ifdProductString':'XZ3, Sony torne and PIX-DT295 release FW smart-card relay','ifdVersionNumber':'0x00000001','ifdProtocolSupport':'0x00000003','ifdCapabilities':'0x00000000','ifdVendorID':[usb_id('USB_VENDOR_ID'),usb_id('USB_TORNE_VENDOR_ID'),usb_id('MB86H_PIX_DT295_VENDOR_ID')],'ifdProductID':[usb_id('USB_PRODUCT_ID'),usb_id('USB_TORNE_PRODUCT_ID'),usb_id('MB86H_PIX_DT295_PRODUCT_ID')],'ifdFriendlyName':['I-O DATA GV-MVP/XZ3 Smart Card Reader','Sony torne CECH-ZD1 J Smart Card Reader','PIX-DT295 Smart Card Reader']}
(bundle/'Contents/Info.plist').write_bytes(plistlib.dumps(info))
if sys.platform=='darwin':
    deps=subprocess.check_output(['otool','-L',str(binary)],text=True).splitlines()[1:]
    for line in deps:
        dependency=line.strip().split(' (')[0]
        if 'libusb-1.0' not in dependency:continue
        local=binary.parent/Path(dependency).name
        shutil.copyfile(dependency,local)
        subprocess.run(['install_name_tool','-change',dependency,'@loader_path/'+local.name,str(binary)],check=True)
        subprocess.run(['install_name_tool','-id','@loader_path/'+local.name,str(local)],check=True)
        subprocess.run(['codesign','--force','--sign','-','--timestamp=none',str(local)],check=True)
    subprocess.run(['install_name_tool','-id','@loader_path/ifd-mb86H',str(binary)],check=True)
    subprocess.run(['codesign','--force','--sign','-','--timestamp=none',str(binary)],check=True)
(out/'manifest.json').write_text(json.dumps(dict(bundle=str(bundle),binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),firmware_sha256=m['sha256'],device='usb-shared',debug=False),indent=2))
print(bundle)
