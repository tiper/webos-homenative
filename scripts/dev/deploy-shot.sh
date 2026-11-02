#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 tiper
# Swap in the new binary, (re)launch Home Native, screenshot it, print its log.
# the dev app writes its own log (it cannot write Home's): read whichever is newer
L=$(ls -t /tmp/homenative-dev.log /tmp/homenative.log 2>/dev/null | head -1)
D=/media/developer/apps/usr/palm/applications/io.github.tiper.homenative
luna-send -n 1 luna://com.webos.applicationManager/closeByAppId '{"id":"io.github.tiper.homenative"}' >/dev/null
sleep 1
mv /tmp/homenative.new $D/homenative && chmod 755 $D/homenative
luna-send -n 1 luna://com.webos.applicationManager/launch '{"id":"io.github.tiper.homenative"}'
sleep 6
luna-send -n 1 luna://com.webos.service.capture/executeOneShot '{"path":"/tmp/nh-shot.png","method":"DISPLAY","format":"PNG"}'
cat "$L" | grep -v "img: .*ms$"
ls $D/iconcache | wc -l
