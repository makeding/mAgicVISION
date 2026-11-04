KDIR ?= /lib/modules/$(shell uname -r)/build
PYTHON ?= python3
DEPMOD ?= depmod
KERNEL_RELEASE = $(shell $(MAKE) -s -C "$(KDIR)" kernelrelease)
CCID_PLIST ?=
INSTALL_MOD_PATH ?=
MODULE_DIR := $(abspath $(CURDIR)/..)
CCID_ARGS = --root "$(if $(INSTALL_MOD_PATH),$(INSTALL_MOD_PATH),/)" $(if $(CCID_PLIST),"$(CCID_PLIST)")

.PHONY: all clean install check-pcsc install-pcsc

all:
	$(MAKE) -C $(KDIR) M=$(MODULE_DIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(MODULE_DIR) clean

check-pcsc:
	$(PYTHON) ccid_metadata.py $(CCID_ARGS)

install-pcsc:
	$(PYTHON) ccid_metadata.py --apply $(CCID_ARGS)

install: check-pcsc
	$(MAKE) -C $(KDIR) M=$(MODULE_DIR) INSTALL_MOD_PATH="$(INSTALL_MOD_PATH)" modules_install
	$(PYTHON) ccid_metadata.py --apply $(CCID_ARGS)
	test -n "$(KERNEL_RELEASE)"
	$(DEPMOD) $(if $(INSTALL_MOD_PATH),-b "$(INSTALL_MOD_PATH)") -a "$(KERNEL_RELEASE)"
