#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 tiper
# Swap in the new binary, (re)launch Home Native, screenshot it, print its log.
D=/media/developer/apps/usr/palm/applications/io.github.tiper.homenative
luna-send -n 1 luna://com.webos.applicationManager/closeByAppId '{"id":"io.github.tiper.homenative"}' >/dev/null
sleep 1
mv /tmp/homenative.new $D/homenative && chmod 755 $D/homenative
luna-send -n 1 luna://com.webos.applicationManager/launch '{"id":"io.github.tiper.homenative"}'
sleep 6
luna-send -n 1 luna://com.webos.service.capture/executeOneShot '{"path":"/tmp/nh-shot.png","method":"DISPLAY","format":"PNG"}'
cat /tmp/homenative.log | grep -v "img: .*ms$"
ls $D/iconcache | wc -l
