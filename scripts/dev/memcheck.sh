#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 tiper
# Read-only: wait until 120 s after boot, then record memory and which Home is running.
while [ "$(cut -d. -f1 /proc/uptime)" -lt 120 ]; do sleep 2; done
echo "== uptime"; cut -d' ' -f1 /proc/uptime
echo "== disabled flag"; ls /var/lib/homenative/disabled 2>&1
echo "== free"; free
grep -E "MemTotal|MemFree|MemAvailable|^Cached|SwapFree" /proc/meminfo
echo "== home"; for p in $(pidof com.webos.app.home); do
  pss=$(awk '/^Pss:/{s+=$2} END{print s}' /proc/$p/smaps 2>/dev/null)
  echo "pid $p rss $(awk '/VmRSS/{print $2}' /proc/$p/status)kB pss ${pss}kB swap $(awk '/VmSwap/{print $2}' /proc/$p/status)kB md5 $(md5sum < /proc/$p/exe)"; done
echo "== foreground"; luna-send -n 1 luna://com.webos.applicationManager/getForegroundAppInfo '{}'
echo "== running"; luna-send -n 1 luna://com.webos.applicationManager/running '{}' | grep -o '"id":"[^"]*"' | tr '\n' ' '; echo
