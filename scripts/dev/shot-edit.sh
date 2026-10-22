#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 tiper
# Edit-mode screenshots. Sends no OK: states are set directly, and "noedit" leaves without
# telling SAM anything. Nothing is launched, moved or deleted.
# the dev app writes its own log (it cannot write Home's): read whichever is newer
L=$(ls -t /tmp/homenative-dev.log "$L" 2>/dev/null | head -1)
cap() { luna-send -n 1 luna://com.webos.service.capture/executeOneShot "{\"path\":\"/tmp/$1.png\",\"method\":\"DISPLAY\",\"format\":\"PNG\"}" >/dev/null; }
k() { echo "$*" > /tmp/nh-cmd; sleep 2; }
k edit; k right; k right; k right; cap nh-e1
k trash; cap nh-e2
k confirm; cap nh-e3
k pick; k right; cap nh-e4
k left; k noedit   # carried tile back in place, leave without telling SAM
grep -E "move |delete|remove|launch " "$L" | tail -5
luna-send -n 1 luna://com.webos.applicationManager/getForegroundAppInfo '{}'
