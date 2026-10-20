#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 tiper
# Remove Home Native: the boot hook, its settings (hidden apps, flags, your wallpaper copy in
# /var/lib/homenative) and the app. Reboot afterwards for LG's Home; or run restore.sh first to
# get it back at once.
[ "$(id -u)" = 0 ] || { echo "uninstall: run this as root" >&2; exit 1; }
rm -f /var/lib/webosbrew/init.d/10-homenative && echo "removed the boot hook"
rm -rf /var/lib/homenative && echo "removed /var/lib/homenative"
OUT=/tmp/homenative-remove.out
script -qc "luna-send -i -a com.webos.appInstallService luna://com.webos.appInstallService/dev/remove '{\"id\":\"io.github.tiper.homenative\",\"subscribe\":true}'" /dev/null > $OUT 2>&1 &
P=$!
for i in $(seq 1 30); do grep -q -E '"state":"removed"|"statusValue": *2[0-9]|"returnValue":false' $OUT && break; sleep 1; done
kill $P 2>/dev/null
ls -d /media/developer/apps/usr/palm/applications/io.github.tiper.homenative >/dev/null 2>&1 \
  && { tail -3 $OUT; echo "the app may still be there"; } || echo "removed the app"
rm -f $OUT
echo "Reboot the TV for LG's Home."
