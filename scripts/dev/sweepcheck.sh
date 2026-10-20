#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 tiper
# TESTING.md 2-4: decoder log lines, a forced texture-cache sweep in dev mode, log size.
# Restarts Home twice (into dev mode and back out); leaves Home in front.
L=/tmp/homenative.log
cap() { luna-send -n 1 luna://com.webos.service.capture/executeOneShot "{\"path\":\"/tmp/$1.png\",\"method\":\"DISPLAY\",\"format\":\"PNG\"}" >/dev/null; }
restart() { for p in $(pidof com.webos.app.home); do kill $p; done; sleep 3
  luna-send -n 1 luna://com.webos.applicationManager/launch '{"id":"com.webos.app.home"}' >/dev/null; sleep 8; }
echo "== decoder"; grep -c "img: " $L; grep -i "decoder" $L || echo "no decoder errors"
echo "== log size before restart"; ls -la $L
touch /var/lib/homenative/dev /var/lib/homenative/verbose
restart
echo "== home pid $(pidof com.webos.app.home)"
echo "img lines this run: $(grep -c 'img: ' $L)"; grep "img: " $L | head -3
echo sweep > /tmp/nh-cmd; sleep 2
grep "tcache" $L | tail -3
echo up > /tmp/nh-cmd; sleep 2; cap nh-sw1
echo down > /tmp/nh-cmd; sleep 1; echo recents > /tmp/nh-cmd; sleep 2; cap nh-sw2
echo norecents > /tmp/nh-cmd; sleep 1
rm -f /var/lib/homenative/dev /var/lib/homenative/verbose
restart
echo "== back to normal, pid $(pidof com.webos.app.home)"; ls -la $L; grep -v "img: " $L | tail -5
