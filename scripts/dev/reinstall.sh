#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 tiper
# Fresh install: the Home Native dev app, then the 10-homenative boot hook. No reboot here.
IPK=$(ls /tmp/io.github.tiper.homenative_*_arm.ipk 2>/dev/null | tail -1)
script -qc "luna-send -i -a com.webos.appInstallService luna://com.webos.appInstallService/dev/install '{\"id\":\"io.github.tiper.homenative\",\"ipkUrl\":\"$IPK\",\"subscribe\":true}'" /dev/null > /tmp/nh-install.out 2>&1 &
P=$!
for i in $(seq 1 30); do grep -q -E '"state":"installed"|"returnValue":false|FAILED' /tmp/nh-install.out && break; sleep 1; done
kill $P 2>/dev/null
grep -o '"state":"[^"]*"' /tmp/nh-install.out | tail -1
rm -f $IPK /tmp/nh-install.out
D=/media/developer/apps/usr/palm/applications/io.github.tiper.homenative
ls -la $D
cp /tmp/10-homenative /var/lib/webosbrew/init.d/10-homenative
chmod 755 /var/lib/webosbrew/init.d/10-homenative
sh -n /var/lib/webosbrew/init.d/10-homenative && echo "hook installed: $(md5sum < /var/lib/webosbrew/init.d/10-homenative)"
rm -f /tmp/10-homenative
ls /var/lib/webosbrew/init.d/
