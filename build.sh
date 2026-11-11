#!/bin/bash
# lifeboat build entry point
#
#   ./build.sh tool            compile the lifeboat binary into out/
#   ./build.sh rootfs          pacstrap the userland and squash it (asks for root)
#   ./build.sh image           initramfs.img + system.img into out/
#   ./build.sh all             tool, rootfs, image
#   ./build.sh iso             out/lifeboat.iso, for flashing, vms and discs
#   ./build.sh qemu [...]      boot it in qemu (see tools/run-qemu.sh)
#   ./build.sh usb /dev/sdX [wipe]   write a boot stick (asks for root)
#   ./build.sh usb work/usb.img      the same into an image file, for "qemu usb"
#   ./build.sh kernel          optional: compile our own 7.2 kernel instead of
#                              using the Arch kernel installed on this machine
#
set -euo pipefail

top=$(cd "$(dirname "$0")" && pwd)
src=$top/src/linux-7.2.11
out=$top/out
mods=$top/work/modules
jobs=${JOBS:-$(nproc)}

if [ "$(id -u)" = 0 ]; then
    echo "run as your normal user; root steps use pkexec" >&2
    exit 1
fi
mkdir -p "$out" "$top/work"

tool() {
    make -s -C "$top/boat"
    install -m755 "$top/boat/lifeboat" "$top/boat/lifeboat-splash" "$out/"
    echo "==> out/lifeboat $(du -h "$out/lifeboat" | cut -f1)"
}

kernel() {
    echo "==> kernel, $jobs jobs"
    cd "$src"
    cp "$top/kernel/arch.config" .config
    make olddefconfig
    scripts/kconfig/merge_config.sh -m .config "$top/kernel/lifeboat.config"
    make olddefconfig
    echo "==> $(make -s kernelrelease)"
    make -j"$jobs" bzImage
    make -j"$jobs" modules

    rm -rf "$mods"
    make INSTALL_MOD_PATH="$mods" modules_install
    rm -f "$mods"/lib/modules/*/build "$mods"/lib/modules/*/source

    install -Dm644 arch/x86/boot/bzImage "$out/vmlinuz-lifeboat"
    echo "==> $out/vmlinuz-lifeboat, modules in $mods. run rootfs and image again to use it"
}

rootfs() {
    [ -x "$out/lifeboat" ] || tool
    pkexec "$top/tools/mkrootfs.sh" "$top" "$(id -u)"
}

image() {
    "$top/tools/mkinitramfs.sh"
}

usb() {
    [ $# -ge 1 ] || { echo "usage: $0 usb /dev/sdX [wipe]" >&2; exit 1; }
    if [ -b "$1" ]; then
        lsblk -o NAME,SIZE,MODEL,TRAN,LABEL "$1"
        read -r -p "write lifeboat to $1? type yes: " answer
        [ "$answer" = yes ] || exit 1
    else
        # an image file, for ./build.sh qemu usb
        [ -f "$1" ] || truncate -s 8G "$1"
        set -- "$(realpath "$1")" "${@:2}"
    fi
    pkexec "$top/tools/write-usb.sh" "$top" "$@"
}

case ${1:-all} in
    tool)   tool ;;
    kernel) kernel ;;
    rootfs) rootfs ;;
    image)  image ;;
    iso)    shift; "$top/tools/mkiso.sh" "$@" ;;
    qemu)   shift; exec "$top/tools/run-qemu.sh" "$@" ;;
    usb)    shift; usb "$@" ;;
    all)    tool; rootfs; image ;;
    *)      sed -n '2,15p' "$0"; exit 1 ;;
esac
