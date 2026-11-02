#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 tiper
# Drive animations (moves only, no OK) and collect frame timing. Ends back on the apps row.
# the dev app writes its own log (it cannot write Home's): read whichever is newer
L=$(ls -t /tmp/homenative-dev.log /tmp/homenative.log 2>/dev/null | head -1)
k() { echo "$*" > /tmp/nh-cmd; sleep 2; }
pass() { for i in 1 2 3; do k down; k down; k down; k up; k up; k up; done; for i in 1 2 3 4 5 6; do k right; done; for i in 1 2 3 4 5 6; do k left; done; k recents; k norecents; }
pass
k back
grep -E "fps|display" "$L" | tail -12
