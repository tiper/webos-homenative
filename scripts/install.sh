#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 tiper
# Install Home Native on a rooted webOS TV and make it the system Home from the next boot.
# Run as root on the TV, from the folder holding the release files:
#   io.github.tiper.homenative_<version>_arm.ipk  10-homenative  install.sh
# Writes nothing to the read-only system: the app goes to the Developer Mode apps folder and
# the boot hook to Homebrew Channel's init.d. Undo: uninstall.sh, then reboot.
D=$(cd "$(dirname "$0")" && pwd)
IPK=$(ls "$D"/io.github.tiper.homenative_*_arm.ipk 2>/dev/null | tail -1)
HOOKS=/var/lib/webosbrew/init.d
APP=/media/developer/apps/usr/palm/applications/io.github.tiper.homenative

fail() { echo "install: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || fail "run this as root (a rooted TV with Homebrew Channel)"
[ -d "$HOOKS" ] || fail "no $HOOKS: Homebrew Channel is needed, with root"
[ -n "$IPK" ] || fail "no io.github.tiper.homenative_*_arm.ipk next to this script"
[ -f "$D/10-homenative" ] || fail "no 10-homenative next to this script"

echo "== installing $(basename "$IPK")"
# appInstallService reports progress on a subscription; wait for "installed" or a failure
OUT=/tmp/homenative-install.out
script -qc "luna-send -i -a com.webos.appInstallService luna://com.webos.appInstallService/dev/install '{\"id\":\"io.github.tiper.homenative\",\"ipkUrl\":\"$IPK\",\"subscribe\":true}'" /dev/null > $OUT 2>&1 &
P=$!
for i in $(seq 1 60); do grep -q -E '"state":"installed"|"returnValue":false|FAILED' $OUT && break; sleep 1; done
kill $P 2>/dev/null
grep -q '"state":"installed"' $OUT || { tail -5 $OUT; fail "the app did not install"; }
rm -f $OUT
[ -x "$APP/homenative" ] || fail "installed, but $APP/homenative is missing"

echo "== installing the boot hook"
cp "$D/10-homenative" $HOOKS/10-homenative && chmod 755 $HOOKS/10-homenative || fail "cannot write $HOOKS"
sh -n $HOOKS/10-homenative || fail "the hook does not parse"
mkdir -p /var/lib/homenative
rm -f /var/lib/homenative/disabled /var/lib/homenative/failed
# /tmp is emptied at every boot: keep the way back next to the settings
for f in uninstall.sh restore.sh; do [ -f "$D/$f" ] && cp "$D/$f" /var/lib/homenative/$f; done

# Already the Home: the mount still holds the old binary, so point it at the new one and restart.
# Nothing else on screen is touched: Home comes to the front only if it was there.
H=/usr/bin/com.webos.app.home
if grep -q " $H " /proc/mounts; then
  echo "== switching to the new build"
  fg=$(luna-send -n 1 luna://com.webos.applicationManager/getForegroundAppInfo '{}' 2>/dev/null |
    sed -n 's/.*"appId": *"\([^"]*\)".*/\1/p')
  umount -l "$H" && mount --bind "$APP/homenative" "$H" || fail "cannot swap the mount: reboot to finish"
  pid=$(pidof com.webos.app.home); [ -n "$pid" ] && kill $pid
  sleep 2
  if [ -z "$(pidof com.webos.app.home)" ]; then
    if [ "$fg" = "com.webos.app.home" ]; then p='{"id":"com.webos.app.home"}'; else p='{"id":"com.webos.app.home","preload":"full"}'; fi
    luna-send -n 1 luna://com.webos.applicationManager/launch "$p" >/dev/null 2>&1
    if [ -n "$fg" ] && [ "$fg" != "com.webos.app.home" ]; then
      for i in $(seq 1 60); do
        now=$(luna-send -n 1 luna://com.webos.applicationManager/getForegroundAppInfo '{}' 2>/dev/null |
          sed -n 's/.*"appId": *"\([^"]*\)".*/\1/p')
        if [ "$now" = "com.webos.app.home" ]; then
          luna-send -n 1 luna://com.webos.applicationManager/launch "{\"id\":\"$fg\"}" >/dev/null 2>&1
          break
        fi
      done
    fi
  fi
  # the hook, again: its watch on Home starts now rather than at the next boot
  sh $HOOKS/10-homenative
  echo
  echo "Done. The new build is running."
else
  echo
  echo "Done. Reboot the TV (reboot) and Home Native is the Home from then on."
fi
echo "Your own wallpaper: put it at /var/lib/homenative/wallpaper.png (or .jpg), 1920 x 1080."
echo "To undo later: sh /var/lib/homenative/uninstall.sh (or restore.sh for LG's Home until reboot)."
