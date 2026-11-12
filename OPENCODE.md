# OPENCODE.md — managing Lifeboat OS

I am looking after this tree until the owner says otherwise. This file is the
record: what I verified, what I changed, what I think needs doing next. The
owner's README stays the description of the product; this is the work log.

Started: 2026-11-12.

## How I work on it

- Build as the normal user. Root steps go through `pkexec` (`build.sh` refuses
  to run as root, `mkrootfs.sh` and `write-usb.sh` need root).
- Never write to a real disk (`/dev/sdX`) without the owner explicitly asking
  and confirming the device. The QEMU images under `work/` are fair game.
- Commit through `./git.sh`, never plain `git`. It sets the owner's name and
  email and strips assistant `Co-Authored-By` trailers.
- Keep `README.md` true. A behaviour change is not finished until the README
  says so.
- Do not delete `work/*.img` (test sticks, carrier disks) or anything in `out/`
  without asking; they are expensive to rebuild.
- Every change gets an entry in the change log below.

## Build quick reference

```
./build.sh tool          compile boat/ -> out/lifeboat (+ lifeboat-splash)
./build.sh rootfs        pacstrap + overlay + squash -> system.sfs, hw.sfs (root)
./build.sh image         initramfs.img, system.img (no root)
./build.sh all           tool, rootfs, image
./build.sh iso           out/lifeboat.iso (no root)
./build.sh qemu [...]    boot it (see tools/run-qemu.sh)
./build.sh usb /dev/sdX  write a real stick (root, wipes unless it is ours)
```

The binary reaches the ISO only through `system.sfs`: `mkrootfs.sh` installs
`out/lifeboat` into `work/rootfs` and squashes it. So after a C change the
sequence is `./build.sh all && ./build.sh iso` — `./build.sh iso` alone reuses
the old squash.

## State I verified (2026-11-12)

- Git: branch `main`, one commit `c9f1cd5`, working tree clean, 65 tracked
  files. `session-*.md` is excluded via `.git/info/exclude`.
- Sources: all C compiles with the project's flags, no warnings; `make -q`
  reports `out/lifeboat` up to date; `bash -n` passes for every script;
  `sh -n initramfs/init` passes; no `TODO`/`FIXME`/`XXX`/`HACK` markers.
- No build input is newer than `out/system.sfs`, i.e. the shipped squash is not
  stale relative to `boat/`, `rootfs/`, `initramfs/` or `tools/`.
- Artifacts in `out/`: kernel `7.2.15-arch1-1`, `system.sfs` 576M, `hw.sfs`
  578M, `initramfs.img` 9.3M, `lifeboat.iso` 1.2G (2026-11-11 22:37).
- Boot test: `MEM=4G ./build.sh qemu iso headless`, UEFI + OVMF, ISO as a disc.
  It reached the systemd-boot menu, booted, and rendered the home screen
  (OCR of the framebuffer shows the APPS column and the app drawer). The
  screenshot is `work/boot-check-2026-11-12.png`. The current ISO is good.

## Known issues, in the order I would take them

1. **`write-usb.sh` can wipe a lifeboat stick's saved changes.** The keep-path
   test is one `blkid` label read; if the label is not readable at that moment
   the `&&` chain falls into the `else` and the disk is repartitioned. Fix is a
   short udev-settle/retry and a `PARTLABEL=lifeboat-data` fallback. I proposed
   this before and it was reverted, so it stays here until the owner asks.
2. **`./build.sh iso` can ship an old binary.** See the note above. A guard in
   `mkiso.sh` that compares mtimes with `out/system.sfs` would make it loud.
3. **The USB stick dropped off the bus twice while flashing** (`/dev/sda`
   vanished mid-write, kernel log lost the device). `write-usb.sh` now reports
   flush progress, but this needs one clean real-stick flash to close. Suspect
   the port/stick before the script.
4. **`rfkill unblock bluetooth` runs unprivileged** (`boat/probe.c`, "on"
   toggle) and is not in `rootfs/etc/doas.conf`, so a soft-blocked adapter stays
   blocked. Backlight is fine: the udev rule plus the `video` group covers it.
5. **Disk is 94% full** (7.6G free); `work/` holds roughly 28G of QEMU test
   images. Not urgent, but a build needs room.
6. `out/system.sfs`, `out/hw.sfs`, `out/vmlinuz` are root-owned although
   `mkrootfs.sh` chowns `out/*` back at the end; only root can rewrite them.
   Harmless for `iso` (read-only), but worth understanding before a rebuild.

## Backlog (not started, no promises)

- A fast path for code-only changes: install the new binary into `work/rootfs`
  and re-squash, without a full `pacstrap`.
- A `./build.sh check` that does the syntax/compile/staleness checks above.
- Make the QEMU `iso` boot test scriptable (boot, screenshot, exit non-zero if
  the home screen never appears).
- Re-read the README against the code once the open issues are settled.

## Change log

### 2026-11-12

- Created this file.
- No source changes.
- Verified: compile/syntax/marker checks, artifact freshness, and a full UEFI
  boot of `out/lifeboat.iso` in QEMU to the home screen.
  Evidence: `work/boot-check-2026-11-12.png`.
