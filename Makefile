# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 tiper
# homenative — cross-compiled with the webosbrew NDK
WEBOS_SDK ?= $(HOME)/webos-ndk/arm-webos-linux-gnueabi_sdk-buildroot
SYSROOT    = $(WEBOS_SDK)/arm-webos-linux-gnueabi/sysroot
CC         = $(WEBOS_SDK)/bin/arm-webos-linux-gnueabi-gcc
CFLAGS     = --sysroot=$(SYSROOT) -O2 -Wall -Wextra -std=gnu11 -Ivendor \
             -I$(SYSROOT)/usr/include/glib-2.0 -I$(SYSROOT)/usr/lib/glib-2.0/include -I$(SYSROOT)/usr/include/freetype2
LIBS       = -lSDL2 -lSDL2_image -lSDL2_ttf -lGLESv2 -lluna-service2 -lglib-2.0 -lfreetype -ldl -lm
APPID      = io.github.tiper.homenative

pkg/homenative: src/home.c vendor/cJSON.c vendor/cJSON.h
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^) $(LIBS) -Wl,--as-needed
	$(WEBOS_SDK)/bin/arm-webos-linux-gnueabi-strip $@

ipk: pkg/homenative
	python3 mkipk.py

# the release: the ipk, the boot hook and the install / uninstall / restore scripts
# macOS's bsdtar adds metadata the TV's busybox tar chokes on (it skips files); GNU tar adds none
TARFLAGS   = $(shell tar --version 2>/dev/null | grep -q bsdtar && echo --no-mac-metadata --no-xattrs)
VERSION    = $(shell python3 -c "import json;print(json.load(open('pkg/appinfo.json'))['version'])")
dist: ipk
	rm -rf dist/homenative-$(VERSION) && mkdir -p dist/homenative-$(VERSION)
	cp $(APPID)_$(VERSION)_arm.ipk hook/10-homenative scripts/install.sh scripts/uninstall.sh scripts/restore.sh dist/homenative-$(VERSION)/
	COPYFILE_DISABLE=1 tar $(TARFLAGS) --format=ustar -C dist -czf dist/homenative-$(VERSION).tar.gz homenative-$(VERSION)

clean:
	rm -rf pkg/homenative *.ipk dist
