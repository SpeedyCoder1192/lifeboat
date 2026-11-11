#!/bin/bash
# out/initramfs.img: busybox, dmsetup, mkfs.xfs, the modules init needs.
# out/system.img:    the squashfs wrapped in a cpio. only tools/run-qemu.sh
#                    uses it, to hand the system over as a second initrd. a
#                    real stick carries system.sfs as a plain file instead,
#                    because boot loaders can't load an initrd that big.
set -euo pipefail

top=$(cd "$(dirname "$0")/.." && pwd)
root=$top/work/rootfs
stage=$top/work/initramfs
out=$top/out

[ -x "$root/usr/bin/busybox" ] || { echo "no rootfs yet (./build.sh rootfs)" >&2; exit 1; }
[ -f "$out/system.sfs" ] && [ -f "$out/hw.sfs" ] || { echo "no out/system.sfs and hw.sfs yet (./build.sh rootfs)" >&2; exit 1; }

rm -rf "$stage"
mkdir -p "$stage"/{usr/bin,usr/lib,etc,proc,sys,dev,run,newroot}
ln -s usr/bin "$stage/bin"
ln -s usr/bin "$stage/sbin"
ln -s usr/lib "$stage/lib"
ln -s usr/lib "$stage/lib64"

install -m755 "$root/usr/bin/busybox" "$stage/usr/bin/busybox"
for applet in $("$stage/usr/bin/busybox" --list); do
    [ -e "$stage/usr/bin/$applet" ] || ln -s busybox "$stage/usr/bin/$applet"
done

# a binary from the rootfs plus every library it asks for
copy_elf() {
    local from=$1 to=$2 lib
    install -Dm755 "$from" "$to"
    for lib in $(readelf -d "$from" 2>/dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
        [ -e "$stage/usr/lib/$lib" ] || copy_elf "$(realpath "$root/usr/lib/$lib")" "$stage/usr/lib/$lib"
    done
}
for bin in dmsetup mkfs.xfs cryptsetup; do
    rm -f "$stage/usr/bin/$bin"
    copy_elf "$(realpath "$root/usr/bin/$bin")" "$stage/usr/bin/$bin"
done
copy_elf "$(realpath "$root/usr/lib/ld-linux-x86-64.so.2")" "$stage/usr/lib/ld-linux-x86-64.so.2"

install -m755 "$top/initramfs/init" "$stage/init"
install -m755 "$top/boat/lifeboat-splash" "$stage/usr/bin/lifeboat-splash"
install -m644 "$top/rootfs/etc/lifeboat.conf" "$stage/etc/lifeboat.conf"
install -m644 "$top/initramfs/modules" "$stage/etc/modules"

# modules, uncompressed because busybox modprobe can't read .zst
kver=$(cat "$out/kernel-version")
kdir=$root/usr/lib/modules/$kver
mkdir -p "$stage/usr/lib/modules/$kver"
while read -r mod; do
    modprobe -d "$root" -S "$kver" --show-depends "$mod" 2>/dev/null |
        sed -n 's/^insmod \(\S*\).*/\1/p'
done < "$top/initramfs/modules" | sort -u | while read -r ko; do
    rel=${ko#*"/modules/$kver/"}
    dest=$stage/usr/lib/modules/$kver/${rel%.zst}
    mkdir -p "$(dirname "$dest")"
    case $ko in
        *.zst) zstd -dqf "$ko" -o "$dest" ;;
        *)     cp "$ko" "$dest" ;;
    esac
done
cp "$kdir"/modules.{builtin,builtin.modinfo,order} "$stage/usr/lib/modules/$kver/" 2>/dev/null || true
depmod -b "$stage" "$kver"

(cd "$stage" && find . | LC_ALL=C sort | cpio -o -H newc --owner=0:0 --quiet | zstd -9 -q) \
    > "$out/initramfs.img"
(cd "$out" && printf 'system.sfs\nhw.sfs\n' | cpio -o -H newc --owner=0:0 --quiet) > "$out/system.img"

echo "==> initramfs.img $(du -h "$out/initramfs.img" | cut -f1), system.img $(du -h "$out/system.img" | cut -f1)"
