#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 tiper
# Bring Home to the front, let it settle 20 s, then record memory (same as memcheck.sh).
luna-send -n 1 luna://com.webos.applicationManager/launch '{"id":"com.webos.app.home"}'
sleep 20
sed '/^while/d' /tmp/memcheck.sh > /tmp/memnow.sh; sh /tmp/memnow.sh
