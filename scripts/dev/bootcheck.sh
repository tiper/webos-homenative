#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 tiper
# Read-only: did 10-homenative make Home Native the Home at boot?
NH=/media/developer/apps/usr/palm/applications/io.github.tiper.homenative/homenative
echo "== uptime"; cat /proc/uptime
echo "== mounts"; grep -E " /usr/bin/com.webos.app.home | /mnt/lg/cmn_data/checkpoint" /proc/mounts
P=$(pidof com.webos.app.home); echo "== home pid $P"
[ -n "$P" ] && { md5sum /proc/$P/exe $NH; tr '\0' ' ' < /proc/$P/cmdline | head -c 250; echo;
  echo "Home process started $(awk '{printf "%.1f", $22/100}' /proc/$P/stat) s after boot"; }
echo "== hook run (first Home Native log line, and LG's Home start)"; grep -E "SAM APP_LAUNCH.*com.webos.app.home.*start_prelaunching|NATIVE_APP_HANDLER.*home.*(registered|current_client)" /var/log/messages | head -4 | cut -c1-200
echo "== foreground"; luna-send -n 1 luna://com.webos.applicationManager/getForegroundAppInfo '{}'
echo "== home native log"; grep -v "img: .*ms$" /tmp/homenative.log 2>/dev/null | grep -v "luna luna" | head -25
echo "== SAM home handling since boot"; grep -E "NATIVE_APP_HANDLER|LAUNCH_LASTAPP" /var/log/messages | grep home | tail -6
