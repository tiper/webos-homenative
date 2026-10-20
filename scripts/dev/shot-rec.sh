#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 tiper
# Recents screenshot. Sends no OK: nothing is launched or closed.
cap() { luna-send -n 1 luna://com.webos.service.capture/executeOneShot "{\"path\":\"/tmp/$1.png\",\"method\":\"DISPLAY\",\"format\":\"PNG\"}" >/dev/null; }
k() { echo "$*" > /tmp/nh-cmd; sleep 2; }
k recents; cap nh-r1
k norecents
grep -E "recents|listApps|FAIL" /tmp/homenative.log | tail -5
