#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 tiper
# Top buttons and Recents screenshots. Sends no OK.
cap() { luna-send -n 1 luna://com.webos.service.capture/executeOneShot "{\"path\":\"/tmp/$1.png\",\"method\":\"DISPLAY\",\"format\":\"PNG\"}" >/dev/null; }
k() { echo "$*" > /tmp/nh-cmd; sleep 2; }
k up; k up; cap nh-t1
k down; k down
k recents; cap nh-r1
k norecents
grep -E "glyph|dingbat" /tmp/homenative.log | tail -5
