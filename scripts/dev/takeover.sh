#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 tiper
# One-off test: make Home Native the system Home until the next reboot.
# Writes nothing to the system: two bind mounts (gone on reboot) and a kill/launch.
# Undo without rebooting: sh /tmp/restore.sh
NH=/media/developer/apps/usr/palm/applications/io.github.tiper.homenative/homenative
H=/usr/bin/com.webos.app.home
CP=/mnt/lg/cmn_data/checkpoint/com.webos.app.home
E=/tmp/nh-empty-checkpoint

[ -x "$NH" ] || { echo "no Home Native binary at $NH"; exit 1; }
# our binary in place of LG's Home
grep -q " $H " /proc/mounts || mount --bind "$NH" "$H" || { echo "bind failed"; exit 1; }
# an empty checkpoint on top of the one there now, so SAM cold-starts the binary instead of
# restoring LG's snapshot (stacked: restore.sh lifts only this one, the boot hook's stays)
mkdir -p $E && mount --bind $E $CP
echo "== mounts"; grep -E " $H | $CP " /proc/mounts
# one instance only: close the dev app, stop LG's Home, start "Home"
luna-send -n 1 luna://com.webos.applicationManager/closeByAppId '{"id":"io.github.tiper.homenative"}' >/dev/null
pid=$(pidof com.webos.app.home); [ -n "$pid" ] && kill -9 $pid
sleep 2
luna-send -n 1 luna://com.webos.applicationManager/launch '{"id":"com.webos.app.home"}'
sleep 5
P=$(pidof com.webos.app.home); echo "== home pid $P exe $(readlink /proc/$P/exe)"
md5sum /proc/$P/exe $NH 2>/dev/null
echo "== foreground"; luna-send -n 1 luna://com.webos.applicationManager/getForegroundAppInfo '{}'
echo "== log"; grep -v "img: .*ms$" /tmp/homenative.log | head -30
luna-send -n 1 luna://com.webos.service.capture/executeOneShot '{"path":"/tmp/nh-home1.png","method":"DISPLAY","format":"PNG"}' >/dev/null
# what the Home key does, twice: SAM launches com.webos.app.home with launchType homeKey.
# A registered Home keeps its pid and gets "relaunch" events.
for i in 1 2; do
  before=$(pidof com.webos.app.home)
  luna-send -n 1 luna://com.webos.applicationManager/launch '{"id":"com.webos.app.home","params":{"activateType":"home","launchType":"homeKey"}}'
  sleep 3
  echo "== Home key $i: pid $before -> $(pidof com.webos.app.home)"
done
echo "== foreground"; luna-send -n 1 luna://com.webos.applicationManager/getForegroundAppInfo '{}'
grep -E "lifecycle|focus gained" /tmp/homenative.log | tail -8
grep "NATIVE_APP_HANDLER" /var/log/messages | grep home | tail -3
