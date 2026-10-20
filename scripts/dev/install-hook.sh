#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 tiper
# Install the 10-homenative boot hook and run it once (it is idempotent).
set -e
cp /tmp/10-homenative /var/lib/webosbrew/init.d/10-homenative
chmod 755 /var/lib/webosbrew/init.d/10-homenative
sh -n /var/lib/webosbrew/init.d/10-homenative && echo "syntax ok"
mkdir -p /var/lib/homenative
ls -la /var/lib/webosbrew/init.d/
sh /var/lib/webosbrew/init.d/10-homenative
echo "== mounts"; grep -E " /usr/bin/com.webos.app.home | /mnt/lg/cmn_data/checkpoint" /proc/mounts
echo "== write test on the checkpoint (must fail)"; touch /mnt/lg/cmn_data/checkpoint/com.webos.app.home/x 2>&1 || echo "read-only: ok"
P=$(pidof com.webos.app.home); echo "== home pid $P"; md5sum /proc/$P/exe
