#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 tiper
# Screenshots of the page below the apps: the dashboard row, the channels, the Browser and the
# footer. Needs /var/lib/homenative/dev (nh-cmd). Sends no OK.
cap() { luna-send -n 1 luna://com.webos.service.capture/executeOneShot "{\"path\":\"/tmp/$1.png\",\"method\":\"DISPLAY\",\"format\":\"PNG\"}" >/dev/null; }
k() { echo "$*" > /tmp/nh-cmd; sleep 2; }
k down; sleep 1; cap nh-p1
k right; sleep 1; cap nh-p2
k left; k down; sleep 1; cap nh-p3
k down; sleep 1; cap nh-p4
k down; sleep 1; cap nh-p5
k right; sleep 1; cap nh-p6
k back
