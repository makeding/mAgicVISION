[日本語](README.md) | English

<p align="center"><img src="assets/banner.png" width="960" alt="Magic Vision — mAgic VISION"></p>

mAgicVISION is a standard dvb driver for mb86-series TV tuners.  
**The Windows WinUSB driver / BonDriver is still incomplete — PRs are very welcome.**

[![ひろめ隊](https://img.youtube.com/vi/FGhd4D_h9xc/hqdefault.jpg)](https://www.youtube.com/watch?v=FGhd4D_h9xc)

## Supported devices

| Device | USB ID | Standards | Chip |
| --- | --- | --- | --- |
| Company I GV-MVP/XZ–XZ3 | `04bb:0530` | ISDB-T/ISDB-S | MB86H58A |
| Company S torne CECH-ZD1 J | `054c:04b2` | ISDB-T | MB86H47A |
| Company P PIX-DT295 | `06b8:103b` | ISDB-T/ISDB-S | MB86M01 |


* Each device provides one tuner.

### About support requests
We would like to support more models, but verifying new devices inevitably requires physical hardware, and at present we cannot accommodate requests remotely.  
If you would like us to support a device, we would appreciate it if you could send the target device to us.


## Currently unimplemented features
- MULTI2 decryption
- H.264 output
- Windows WinUSB support
- BonDriver support

## Firmware / host compatibility

The firmware (FW) is closed source, while the host/driver is open source.  
FW and host versions do not need to match one-to-one; **they only need to share the same ABI version**.

Releases are labeled like `v1 + 0.1.1`, where `v1` is the FW ABI compatibility version and `0.1.1` is the normal version. Any host/FW combination within the `V1` range should work for most features (some edge features may be incompatible, but this generally does not affect viewing).

Still, to get the latest fixes and features, **updating the host and FW together is strongly recommended**.

## Installation (Debian / Ubuntu)
Download the two `.deb` packages, `firmware` and `dkms`, from [GitHub Releases](https://github.com/makeding/mAgicVISION/releases).
```sh
sudo apt install linux-headers-$(uname -r)
sudo apt install ./magicvision-firmware_*_all.deb ./magicvision-usb-dkms_*_all.deb
```

If you only need the firmware, extract it from the `.deb` in the Release.

```sh
dpkg-deb -x ./magicvision-firmware_*_all.deb firmware-extract
```

`firmware.bin` is extracted to `firmware-extract/usr/lib/firmware/magicvision/firmware.bin`.

## Installation (NixOS)

On NixOS you can install from the flake module in [jodtv-packages](https://github.com/makeding/jodtv-packages).

```nix
inputs.jodtv-packages = {
  url = "github:makeding/jodtv-packages";
};

imports = [
  inputs.jodtv-packages.nixosModules.packages
  inputs.jodtv-packages.nixosModules.dtv
];
```

## Using with Linux DVB

```sh
sudo modprobe dummy_hcd num=2
sudo modprobe mb86H_ccid
sudo modprobe mb86H_usb
```

When you connect a device, `/dev/dvb/xz0/frontend0`, `/dev/dvb/torne0/frontend0`, or `/dev/dvb/dt2950/frontend0` is created.
If you have multiple devices of the same model, the numbers increase. Prepare virtual USB controllers for CCID according to the number of readers.

For recording, use a separate tool such as recisdb that supports standard Linux DVB. However, to use device names like `/dev/dvb/xz0`, you need the fork at [makeding/recisdb-rs](https://github.com/makeding/recisdb-rs).  
The upstream version only supports the `/dev/dvb/adapter0/frontend0`, `/dev/dvb/adapter1/frontend0` format.  

The example records terrestrial channel 24 for 10 seconds.

```sh
recisdb tune --device /dev/dvb/xz0/frontend0 --channel T24 --time 10  sample.ts
```

## Mirakurun

Example `tuners.yml`. Make recisdb usable by the user running Mirakurun.

```yaml
- name: mAgicVISION torne
  types: [GR]
  command: recisdb tune --device /dev/dvb/torne0/frontend0 --channel <channel> -
  checkDevicePath: /dev/dvb/torne0/frontend0

- name: mAgicVISION XZ
  types: [GR, BS, CS]
  command: recisdb tune --device /dev/dvb/xz0/frontend0 --channel <channel> -
  checkDevicePath: /dev/dvb/xz0/frontend0

- name: mAgicVISION DT295
  types: [GR, BS, CS]
  command: recisdb tune --device /dev/dvb/dt2950/frontend0 --channel <channel> -
  checkDevicePath: /dev/dvb/dt2950/frontend0

```
## Benchmarks on real hardware

### Short recordings on three models

On 2026-11-04 15:23 JST, each model was recorded individually for 8 seconds via Linux DVB (`recisdb tune --no-decode --no-strip --time 8`). The torne values are from the afternoon.

| Model | Channel | TS data size | sync / TEI / CC errors |
| --- | --- | ---: | ---: |
| GV-MVP/XZ | GR24 | 17.26 MB | 0 / 0 / 0 |
| torne | GR24 | 17.25 MB | 0 / 0 / 0 |
| PIX-DT295 | BS15_0 | 23.90 MB | 0 / 0 / 0 |

All three recordings exited with code 0, the PAT CRC was valid, and there was no incomplete TS data at the end.  
These are individual 8-second measurements, not measurements of simultaneous recording or long-term stability.

### DT295 channel switching

Each channel was measured once for 5 seconds within the same USB session (standard: 2026-11-04 05:44 JST, fast: 06:03 JST). Times are from the `TUNE` call to host reception.

| Channel | Procedure | First complete TS | Target PAT with valid CRC | TUNE return |
| --- | --- | ---: | ---: | ---: |
| GR24 | Standard | 501 ms | 1,084 ms | 1,370 ms |
| GR24 | Fast | 271 ms | 699 ms | 1,110 ms |
| BS15_0 | Standard | 789 ms | 848 ms | 822 ms |
| BS15_0 | Fast | 298 ms | 449 ms | 432 ms |
| GR22 | Standard | 642 ms | 1,916 ms | 3,155 ms |
| GR22 | Fast | 279 ms | 1,685 ms | 2,642 ms |


## Userland / PC/SC

The libusb verification tool is `build/usbctl`, and the boot/firmware verification tool is `build/usbboot`.  
Do not open the same physical USB device simultaneously from the Linux kernel driver and userland.
`usbboot start` initializes and restores changed regions. `download` and `ota` preserve and restore all RAM.

```sh
./build/usbctl devices
./build/usbboot check-image 04bb 0530 firmware/firmware.bin
```

## License

- Linux kernel code (`linux/`, `usb/linux/`) is [GPL-2.0-only](LICENSE).
- userland and its reusable shared code (`core/`, `device/`, `include/mb86H/`, `usb/boot.c`, `usb/window.c`, `usb/host_contract.h`, `usb/userland/`, `userland/`) is [MIT](LICENSE-MIT). The relevant sources carry `SPDX-License-Identifier: MIT`.
- If shared code is incorporated into a Linux kernel module and distributed, the entire module is distributed under the terms of GPL-2.0-only.
- Firmware artifacts such as `firmware.bin` may be redistributed.


Made with ❤️  
ひろめ隊
