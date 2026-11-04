# Linux module composition.  Source files are compiled as real Kbuild objects;
# no Linux wrapper includes a userspace .c translation unit.
obj-m += magicvision_ccid.o
magicvision_ccid-y := linux/ccid.o core/card_atr.o

obj-m += magicvision_usb.o
magicvision_usb-y := linux/dvb.o core/card.o core/card_atr.o core/stream_input.o core/device_query.o usb/linux/transport.o usb/window.o usb/boot.o core/native_download.o \
	core/bootstrap.o core/loader.o core/loader_snapshot.o core/recovery.o core/resident_image.o core/session.o core/frontend.o core/ts_mux.o \
	core/ts_framer.o core/protocol.o device/device.o device/firmware.o device/gv_mvp_xz/device.o device/gv_mvp_xz/boot.o device/torne/device.o \
	device/pix_dt295/device.o device/pix_dt295/boot.o \
	linux/firmware_embedded.o

ccflags-y += -I$(src)/include

# The firmware generator owns this artifact; the module only embeds it.
USB_BUNDLE := $(src)/firmware/firmware.bin
ifeq ($(wildcard $(USB_BUNDLE)),)
$(error $(USB_BUNDLE) missing: supply the externally built resident bundle)
endif
AFLAGS_linux/firmware_embedded.o := -DFIRMWARE_BUNDLE=\"$(USB_BUNDLE)\"
$(obj)/linux/firmware_embedded.o: $(USB_BUNDLE)
