# Checks on the TV

Things this code does that only the TV can confirm. Run after installing a new build (the bind
mount keeps the old binary until a reboot).

1. **Starts as Home.** `scripts/dev/bootcheck.sh`: Home's pid runs our md5, about 46 s after boot,
   and `/tmp/homenative-hook.log` shows "killing LG Home", then "preloading".
2. **Images decode off the main thread.** The wallpaper and icons appear; with
   `/var/lib/homenative/verbose` the log has `img: ... ms` lines and no `decoder thread` error.
   Opening Recents after using a few apps shows no hitch, and a card's picture changes after
   the app is used again.
3. **Cache sweep.** `touch /var/lib/homenative/dev`, restart Home, then `echo sweep > /tmp/nh-cmd`:
   the log shows `tcache sweep: N freed`. Headings, the Recents title and the edit-mode trash
   and arrows still draw (they are re-created on demand).
4. **Log stays small.** As Home without `verbose`, `/tmp/homenative.log` grows by a few lines
   per use, and never past 512 KB.
5. **Standby.** Leave the TV in standby overnight, wake it, press Home: the clock and date are
   right, inputs and Recents update when something changes, and the Home key responds at once.
6. **Memory.** `scripts/dev/memcheck.sh` and `scripts/dev/memopen.sh`, as before (Home about 12 MB
   proportional set size when open).

7. **Safety net.** Install a build that exits at once, then reboot: Home restarts four times, then
   LG's Home appears, `/var/lib/homenative/failed` holds the build's md5, and after another reboot
   the hook log says `this build failed`. Installing a good build clears it.
8. **Watchdog.** `kill -STOP` then `-CONT` after 90 s: Home dies, SAM restarts it, the log kept in
   `/tmp/homenative.log.prev` ends in the backtrace.
9. **Sleeping while hidden.** With an app in front, Home's voluntary context
   switches stay near 1 a second (about 100 before), and the log says `sleeping on the Wayland
   socket`, with no `stays readable` line. Home key response is unchanged.
10. **Languages and clock.** Change the TV's language: pills and headings follow without a restart.
    A US region shows `3:12 PM`.
11. **A JPEG wallpaper** at `/var/lib/homenative/wallpaper.jpg` shows; a corrupt one falls back to
    the shipped picture.

The host test (`tests/host/run.sh [seeds] [frames]`) runs the real `src/home.c` against a fake TV
under ASan and UBSan, with random keys and hostile service replies. It needs a C compiler and
GLib, not a TV, and CI runs it.

Not known yet: whether the GL context survives every standby cycle (all textures would be lost
and Home would draw black). If that ever happens, Home needs to recreate its textures on
`SDL_RENDER_DEVICE_RESET` / context loss.

**Hiding apps.** In edit mode, Up over a tile shows the eye (and LG's trash for apps that can be
deleted). OK hides the app: once out of edit mode its tile collapses, and
`/var/lib/homenative/hidden` lists its launch point id. In edit mode hidden apps come back dimmed,
with a crossed eye in the corner; the eye over them shows them again. Survives a reboot.
