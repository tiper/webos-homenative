# Changelog

## 0.2.0

Stability
- A build that keeps crashing steps aside for LG's Home, so the TV is never left without one. Only
  dying counts: restarting Home yourself doesn't.
- A watchdog restarts a hung Home. Crashes leave the registers and a backtrace in the kept previous
  log.
- Home restarts if it loses the display connection or can't reach the system bus, instead of
  staying up with nothing working.
- A Home that dies behind an app or in standby comes back within a minute. webOS doesn't restart
  it, even on wake.
- A subscription that can't start at boot keeps trying, and data missing at boot is asked for again
  rather than 30 minutes later.
- Fixed a stack overflow on very long names (the delete dialog).
- A wallpaper that can't be read falls back to the shipped one.
- The delete dialog cancels itself if the app it is asking about goes away.

Memory
- The image cache has a 24 MB budget. The wallpaper is scaled to 1920 x 1080 and Recents pictures
  to thumbnails before they reach the GPU.

Performance
- While hidden, Home sleeps until there is something to do, and doesn't redraw its clock.

Polish
- Spanish, French, German and Italian. The language changes without a restart.
- A 12-hour clock where it is the norm.
- Empty Recents says so, without a Close All that does nothing.
- An app that won't open says so.
- A focused card no longer bounces when the app list refreshes.
- No "No information" flash on channels, and no empty channel row.
- The marquee stops once the screen has dimmed.
- Home key from another app shows Home at once, with no slide.
- Names in Japanese, Chinese, Korean and Thai draw in the TV's own fonts.
- Hidden apps stay hidden when they disappear for a while, such as with a USB drive out.
- Holding blue works on a dimmed Home.
- A moved tile no longer snaps back for a moment.
- Focus stays sensible when the row under it disappears.
- The clock follows a daylight-saving change while Home stays on screen.

Install
- `install.sh` upgrades in place, with no reboot, and leaves the app on screen in front.
- `uninstall.sh` brings LG's Home back at once.

Tests
- `tests/host`: the real `home.c` on a fake TV under ASan and UBSan, run by CI.
