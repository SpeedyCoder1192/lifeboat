#!/bin/bash
# Put lifeboat on a stick. Runs as root (build.sh starts it through pkexec).
#   write-usb.sh <top dir> /dev/sdX [wipe|keep] [extra kernel options]
#   write-usb.sh <top dir> some.img ...     a disk image file, for qemu
#
# A stick that already is a lifeboat stick only gets its boot files replaced,
# so the saved changes survive. "wipe" starts over.
set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/bin

# --- progress on a terminal ------------------------------------------------
# mke2fs gives no percentage, so formatting shows bytes written and time. The
# copy itself lands in page cache; the wait is the umount flushing it, which
# is followed through Dirty/Writeback in /proc/meminfo.
prog_tty=0
if [ -t 1 ]; then prog_tty=1; fi
mon_pid=

human() {
    awk -v b="${1:-0}" 'BEGIN{
        if (b >= 1073741824) printf "%.2fG", b / 1073741824;
        else if (b >= 1048576) printf "%.0fM", b / 1048576;
        else if (b >= 1024) printf "%.0fK", b / 1024;
        else printf "%dB", b }'
}

written() {
    awk -v d="$1" '$3 == d { print $10 * 512; found = 1 } END { if (!found) print 0 }' /proc/diskstats
}

dirty() {
    awk '/^Dirty:/ { d = $2 } /^Writeback:/ { w = $2 } END { print (d + w) * 1024 }' /proc/meminfo
}

bar() {
    local pct=$1 w=24 f=$(( $1 * 24 / 100 )) s="" k
    if [ "$f" -lt 0 ]; then f=0; fi
    if [ "$f" -gt "$w" ]; then f=$w; fi
    for ((k = 0; k < w; k++)); do
        if [ "$k" -lt "$f" ]; then s+="#"; else s+="."; fi
    done
    printf "[%s] %3d%%" "$s" "$pct"
}

# monitor format|flush DEVICE [BYTES]
monitor() {
    local mode=$1 arg=$2 total=${3:-0}
    local parent=$$ spin='-\|/' i=0 prev="" prevat start now dt rate w d pct
    prevat=$(date +%s)
    start=$prevat
    while [ -d "/proc/$parent" ]; do
        now=$(date +%s)
        dt=$(( now - prevat ))
        if [ "$dt" -lt 1 ]; then dt=1; fi
        w=$(written "$arg")
        if [ -n "$prev" ]; then rate=$(( (w - prev) / dt )); else rate=0; fi
        prev=$w
        prevat=$now
        if [ "$mode" = format ]; then
            i=$(( (i + 1) % 4 ))
            printf "\r%c formatting %s   written %s   %s/s   %dm%02ds   " \
                "${spin:$i:1}" "$arg" "$(human "$w")" "$(human "$rate")" \
                $(( (now - start) / 60 )) $(( (now - start) % 60 ))
        else
            d=$(dirty)
            pct=0
            if [ "$total" -gt 0 ] && [ "$d" -lt "$total" ]; then
                pct=$(( 100 - d * 100 / total ))
            fi
            if [ "$pct" -lt 0 ]; then pct=0; fi
            printf "\r%s  flushing to the stick, %s left   %s/s   " \
                "$(bar "$pct")" "$(human "$d")" "$(human "$rate")"
        fi
        sleep 1
    done
}

mon_start() {
    mon_pid=
    if [ "$prog_tty" = 1 ]; then
        monitor "$@" &
        mon_pid=$!
    fi
    return 0
}

mon_stop() {
    if [ -n "${mon_pid:-}" ]; then
        kill "$mon_pid" 2>/dev/null || true
        wait "$mon_pid" 2>/dev/null || true
        mon_pid=
        if [ "$prog_tty" = 1 ]; then printf "\r%78s\r" ""; fi
    fi
    return 0
}

top=$1
dev=$2
wipe=${3:-}
extra=${4:-}
loop=
out=$top/out
root=$top/work/rootfs
sys=/usr/lib/syslinux/bios
mnt=$(mktemp -d)
trap 'mon_stop; umount "$mnt" 2>/dev/null || true; rmdir "$mnt" || true; [ -z "$loop" ] || losetup -d "$loop"' EXIT

[ "$(id -u)" = 0 ] || { echo "needs root" >&2; exit 1; }
if [ -f "$dev" ]; then
    # loops left behind by an earlier run on the same file
    losetup -j "$dev" | cut -d: -f1 | xargs -r -n1 losetup -d
    loop=$(losetup -P --find --show "$dev")
    dev=$loop
fi
[ -b "$dev" ] || { echo "$dev is not a disk" >&2; exit 1; }
case $(lsblk -dno TYPE "$dev") in
    disk|loop) ;;
    *) echo "$dev is not a whole disk" >&2; exit 1 ;;
esac
if lsblk -no MOUNTPOINT "$dev" | grep -q .; then
    echo "$dev has mounted partitions, unmount them first" >&2
    exit 1
fi
for f in vmlinuz initramfs.img system.sfs hw.sfs; do
    [ -f "$out/$f" ] || { echo "out/$f is missing (./build.sh image)" >&2; exit 1; }
done
for f in ldlinux.c32 menu.c32 libutil.c32 gptmbr.bin; do
    [ -f "$sys/$f" ] || { echo "$sys/$f is missing (pacman -S syslinux)" >&2; exit 1; }
done

need=$(( ($(stat -c %s "$out/system.sfs") + $(stat -c %s "$out/hw.sfs") + $(stat -c %s "$out/initramfs.img") + $(stat -c %s "$out/vmlinuz")) / 1048576 * 2 + 400 ))
[ "$need" -lt 2048 ] && need=2048

parts() { lsblk -lnpo NAME,TYPE "$dev" | awk '$2 == "part" {print $1}'; }
esp=$(parts | sed -n 1p)
data=$(parts | sed -n 2p)

if [ "$wipe" != wipe ] && [ -n "$data" ] && [ "$(blkid -o value -s LABEL "$data")" = LIFEBOAT ] &&
   [ $(( $(blockdev --getsize64 "$esp") / 1048576 )) -ge "$need" ]; then
    echo "==> lifeboat stick, keeping the saved changes"
else
    echo "==> wiping $dev"
    wipefs -aq "$dev"
    sgdisk -Z "$dev" >/dev/null
    sgdisk -n "1:0:+${need}M" -t 1:ef00 -c 1:lifeboat-boot \
           -n 2:0:0 -t 2:8300 -c 2:lifeboat-data "$dev" >/dev/null
    partprobe "$dev" || true
    partx -u "$dev" 2>/dev/null || true
    udevadm settle
    # the kernel can take a moment to show the new partitions
    for _ in $(seq 40); do
        [ "$(parts | wc -l)" -ge 2 ] && break
        sleep 0.25
    done
    [ "$(parts | wc -l)" -ge 2 ] || { echo "the new partitions never showed up" >&2; exit 1; }
    esp=$(parts | sed -n 1p)
    data=$(parts | sed -n 2p)
    echo "==> formatting $esp (LBBOOT)"
    mkfs.vfat -F 32 -n LBBOOT "$esp" >/dev/null
    echo "==> formatting $data (mke2fs has no percentage, this is the slow part)"
    mon_start format "${dev##*/}"
    mkfs.ext4 -q -F -L LIFEBOAT -m 0 "$data"
    mon_stop
fi

mount "$esp" "$mnt"
mkdir -p "$mnt/EFI/BOOT" "$mnt/loader/entries" "$mnt/lifeboat" "$mnt/boot/syslinux"
cp "$root/usr/lib/systemd/boot/efi/systemd-bootx64.efi" "$mnt/EFI/BOOT/BOOTX64.EFI"
rm -f "$mnt/lifeboat/system.img"
bytes=$(( $(stat -c %s "$out/vmlinuz") + $(stat -c %s "$out/initramfs.img") + \
          $(stat -c %s "$out/system.sfs") + $(stat -c %s "$out/hw.sfs") + \
          $(stat -c %s "$root/usr/lib/systemd/boot/efi/systemd-bootx64.efi") ))
echo "==> copying $(human "$bytes") to $esp"
cp "$out/vmlinuz" "$out/initramfs.img" "$out/system.sfs" "$out/hw.sfs" "$mnt/lifeboat/"
verfile=$out/version
[ -f "$verfile" ] || verfile=$root/etc/lifeboat-version
cp "$verfile" "$mnt/lifeboat/version"
cat > "$mnt/loader/loader.conf" <<END
default lifeboat.conf
timeout 2
editor yes
END
cat > "$mnt/loader/entries/lifeboat.conf" <<END
title   lifeboat
linux   /lifeboat/vmlinuz
initrd  /lifeboat/initramfs.img
options quiet loglevel=3 $extra
END
cat > "$mnt/loader/entries/lifeboat-fresh.conf" <<END
title   lifeboat, ignoring what is saved on the stick
linux   /lifeboat/vmlinuz
initrd  /lifeboat/initramfs.img
options quiet loglevel=3 lifeboat.fresh $extra
END

# legacy bios: syslinux lives in the fat partition, with gptmbr in the mbr
# pointing at it, so the same stick boots both kinds of firmware
cp "$sys/ldlinux.c32" "$sys/menu.c32" "$sys/libutil.c32" "$mnt/boot/syslinux/"
cat > "$mnt/boot/syslinux/syslinux.cfg" <<END
UI menu.c32
PROMPT 0
TIMEOUT 20
DEFAULT lifeboat
MENU TITLE lifeboat

LABEL lifeboat
    MENU LABEL lifeboat
    LINUX /lifeboat/vmlinuz
    INITRD /lifeboat/initramfs.img
    APPEND quiet loglevel=3 $extra

LABEL fresh
    MENU LABEL lifeboat, ignoring what is saved on the stick
    LINUX /lifeboat/vmlinuz
    INITRD /lifeboat/initramfs.img
    APPEND quiet loglevel=3 lifeboat.fresh $extra
END

echo "==> flushing to the stick, this is where the time goes"
mon_start flush "${dev##*/}" "$bytes"
umount "$mnt"
mon_stop
sync

echo "==> installing the legacy bios boot loader on $dev"
syslinux --install --directory /boot/syslinux "$esp"
dd if="$sys/gptmbr.bin" of="$dev" bs=440 count=1 conv=notrunc status=none
# the attribute gptmbr looks for when it picks the partition to start
sgdisk -A 1:set:2 "$dev" >/dev/null
sync
echo "==> $dev is ready"
