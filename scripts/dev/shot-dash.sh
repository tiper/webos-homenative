#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 tiper
# Home Native is in front: Down to the dashboard, screenshot; Down twice more, screenshot; back up.
cap() { luna-send -n 1 luna://com.webos.service.capture/executeOneShot "{\"path\":\"/tmp/$1.png\",\"method\":\"DISPLAY\",\"format\":\"PNG\"}" >/dev/null; }
echo down > /tmp/nh-cmd; sleep 3; cap nh-d1
echo "down down" > /tmp/nh-cmd; sleep 3; cap nh-d2
echo back > /tmp/nh-cmd; sleep 2; cap nh-d0
grep -E "fps|FAIL|move|edit" /tmp/homenative.log | tail -10
