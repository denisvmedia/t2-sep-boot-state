# SPDX-License-Identifier: GPL-2.0-only OR MIT
# "make" builds the module for the running kernel, "make test" runs the unit
# tests in user space. KDIR selects another kernel build tree.
KDIR ?= /lib/modules/$(shell uname -r)/build
VERSION := $(shell sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' dkms.conf)
DKMS_SRC := /usr/src/t2-sep-boot-state-$(VERSION)

.PHONY: modules clean test analyze dkms-install dkms-remove

modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $(if $(W),W=$(W)) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	$(MAKE) -C tests clean

test:
	$(MAKE) -C tests

# gcc's static analyzer on the transaction, built as a normal program.
analyze:
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror -Wconversion -Wshadow -fanalyzer \
		-Isrc -c src/boot_state.c -o /dev/null

# Both run as root.
dkms-install:
	install -d $(DKMS_SRC)/src
	install -m 0644 Kbuild dkms.conf LICENSE-GPL-2.0 LICENSE-MIT $(DKMS_SRC)/
	install -m 0644 src/main.c src/boot_state.c src/boot_state.h $(DKMS_SRC)/src/
	dkms install t2-sep-boot-state/$(VERSION)

dkms-remove:
	dkms remove t2-sep-boot-state/$(VERSION) --all
	rm -rf $(DKMS_SRC)
