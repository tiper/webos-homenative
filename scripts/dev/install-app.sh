#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 tiper
# Install the Home Native prototype as an ordinary Developer Mode app. Does not launch it.
# Undo: luna-send -n 1 luna://com.webos.appInstallService/dev/remove '{"id":"io.github.tiper.homenative"}'
IPK=$(ls /tmp/io.github.tiper.homenative_*_arm.ipk 2>/dev/null | tail -1)
script -qc "luna-send -i -a com.webos.appInstallService luna://com.webos.appInstallService/dev/install '{\"id\":\"io.github.tiper.homenative\",\"ipkUrl\":\"$IPK\",\"subscribe\":true}'" /dev/null > /tmp/nh-install.out 2>&1 &
P=$!
for i in $(seq 1 30); do grep -q -E '"statusValue": *30|installed|"returnValue": *false|FAILED' /tmp/nh-install.out && break; sleep 1; done
kill $P 2>/dev/null
cat /tmp/nh-install.out | tail -15
rm -f $IPK
D=/media/developer/apps/usr/palm/applications/io.github.tiper.homenative
ls -la $D $D/assets
echo "== needed libs resolve?"; LD_LIBRARY_PATH= ldd $D/homenative 2>&1 | head -20 || true
echo "== role"; cat /var/palm/ls2-dev/roles/pub/io.github.tiper.homenative.json 2>/dev/null | head -5
echo "== foreground"; luna-send -n 1 luna://com.webos.applicationManager/getForegroundAppInfo '{}'
echo "== live inputs icons"; ls /usr/palm/plugins/inputapps/assets/small/ 2>/dev/null | head -30
