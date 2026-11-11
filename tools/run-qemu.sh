#!/bin/bash
# Boot out/ in qemu, kernel and initrds loaded directly.
#
#   run-qemu.sh                a window, with a fake usb stick (work/stick.img)
#   run-qemu.sh nostick        no stick plugged in
#   run-qemu.sh headless       no window, serial console in this terminal
#   run-qemu.sh patch          also copy rootfs/ and the fresh binary over the
#                              booted system, to try changes without a rebuild
#   run-qemu.sh fresh          boot with lifeboat.fresh
#   run-qemu.sh iso            boot out/lifeboat.iso as a disc, through uefi
#   STICK_OPTS=...             extra -drive options for the stick, e.g. throttling
#   run-qemu.sh isostick       boot it flashed onto a fake 8G stick (work/isostick.img),
#                              which is where "lifeboat persist" can be tried
#   run-qemu.sh usb            boot work/usb.img (made by ./build.sh usb work/usb.img)
#                              through uefi and the boot loader, like a real stick
#
# MEM=8G, APPEND="..." and QEMU_EXTRA="..." are picked up from the environment. The monitor is
# on work/qemu.sock (socat - unix:work/qemu.sock).
set -euo pipefail

top=$(cd "$(dirname "$0")/.." && pwd)
out=$top/out
work=$top/work
stick=$work/stick.img
append="console=tty0 console=ttyS0 loglevel=4 ${APPEND:-}"
display=(-device virtio-vga -display gtk)
serial=(-serial "file:$work/qemu-serial.log")
usb=1
uefi=0
parts=("$out/initramfs.img" "$out/system.img")

for arg in "$@"; do
    case $arg in
        nostick)  usb=0 ;;
        headless) display=(-device virtio-vga -display none); serial=(-serial stdio) ;;
        fresh)    append="$append lifeboat.fresh" ;;
        usb)      uefi=1 ;;
        iso)      uefi=2 ;;
        isostick) uefi=3 ;;
        patch)
            rm -rf "$work/patch"
            mkdir -p "$work/patch/patch/usr/local/bin"
            cp -a "$top/rootfs/." "$work/patch/patch/"
            rm -f "$work/patch/patch/packages.list"
            cp "$out/lifeboat" "$work/patch/patch/usr/local/bin/lifeboat"
            [ -d "$work/patch-extra" ] && cp -a "$work/patch-extra/." "$work/patch/patch/"
            (cd "$work/patch" && find . | cpio -o -H newc --owner=0:0 --quiet) > "$work/patch.img"
            parts+=("$work/patch.img")
            ;;
        *) echo "unknown option $arg" >&2; exit 1 ;;
    esac
done

# several initrds in one file, each padded so the next starts on 4 bytes
: > "$work/qemu-initrd.img"
for p in "${parts[@]}"; do
    cat "$p" >> "$work/qemu-initrd.img"
    pad=$(( (4 - $(stat -c %s "$work/qemu-initrd.img") % 4) % 4 ))
    [ "$pad" -gt 0 ] && head -c "$pad" /dev/zero >> "$work/qemu-initrd.img"
done

drive=()
if [ $usb = 1 ]; then
    if [ ! -f "$stick" ]; then
        truncate -s "${STICK_SIZE:-8G}" "$stick"
        mkfs.ext4 -q -F -L LIFEBOAT -m 0 "$stick"
    fi
    drive=(-drive "if=none,id=stick,file=$stick,format=raw"
           -device qemu-xhci -device usb-storage,drive=stick,removable=on)
fi

boot=(-kernel "$out/vmlinuz" -initrd "$work/qemu-initrd.img" -append "$append")
if [ $uefi != 0 ]; then
    cp /usr/share/edk2/x64/OVMF_VARS.4m.fd "$work/ovmf-vars.fd"
    boot=(-drive "if=pflash,format=raw,readonly=on,file=/usr/share/edk2/x64/OVMF_CODE.4m.fd"
          -drive "if=pflash,format=raw,file=$work/ovmf-vars.fd")
    img=$work/usb.img
    if [ $uefi = 3 ] && [ ! -f "$work/isostick.img" ]; then
        # what flashing the iso onto an 8G stick leaves behind
        cp "$out/lifeboat.iso" "$work/isostick.img"
        truncate -s 8G "$work/isostick.img"
    fi
    [ $uefi = 3 ] && img=$work/isostick.img
    [ -f "$img" ] || [ $uefi = 2 ] || { echo "no $img (./build.sh usb work/usb.img)" >&2; exit 1; }
    # STICK_OPTS="throttling.bps-total=20000000" makes the stick slow, to watch moves
    drive=(-drive "if=none,id=stick,file=$img,format=raw${STICK_OPTS:+,$STICK_OPTS}"
           -device qemu-xhci -device usb-storage,drive=stick,removable=on,bootindex=1)
    [ $uefi = 2 ] && drive=(-cdrom "$out/lifeboat.iso")
fi

# DISKS="a.img b.img" adds them as virtio disks (/dev/vda, /dev/vdb, ...)
for d in ${DISKS:-}; do
    drive+=(-drive "if=virtio,format=raw,file=$d")
done

rm -f "$work/qemu.sock"
exec qemu-system-x86_64 -enable-kvm -cpu host -smp 4 -m "${MEM:-4G}" \
    "${boot[@]}" \
    "${display[@]}" "${serial[@]}" "${drive[@]}" \
    -device virtio-rng-pci -nic user,model=virtio-net-pci \
    -monitor "unix:$work/qemu.sock,server,nowait" -no-reboot ${QEMU_EXTRA:-}
