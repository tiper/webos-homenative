#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 tiper
# Put LG's Home back now, without rebooting (as root on the TV). With the 10-homenative boot hook installed a reboot
# brings Home Native back; to undo for good delete /var/lib/webosbrew/init.d/10-homenative.
# Lazy unmounts: SAM keeps Home alive and respawns it at once, so a plain umount can find the
# file busy; -l detaches the mount immediately and the next start runs LG's binary.
H=/usr/bin/com.webos.app.home
CP=/mnt/lg/cmn_data/checkpoint/com.webos.app.home
grep -q " $H " /proc/mounts && umount -l "$H"
# lift our empty checkpoint covers (tmpfs, possibly stacked)
while grep " $CP " /proc/mounts | tail -1 | grep -q "^tmpfs"; do umount -l "$CP" || break; done
pid=$(pidof com.webos.app.home); [ -n "$pid" ] && kill -9 $pid
sleep 2
[ -z "$(pidof com.webos.app.home)" ] && luna-send -n 1 luna://com.webos.applicationManager/launch '{"id":"com.webos.app.home"}' &
sleep 4
echo "== mounts"; grep -E " $H | $CP " /proc/mounts
P=$(pidof com.webos.app.home); echo "== home pid $P"; [ -n "$P" ] && md5sum /proc/$P/exe /usr/bin/com.webos.app.home
echo "== foreground"; luna-send -n 1 luna://com.webos.applicationManager/getForegroundAppInfo '{}'
