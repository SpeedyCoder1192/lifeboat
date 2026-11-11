#!/bin/bash
# out/lifeboat.iso: boots through uefi from a flashed stick, a vm or a disc.
#   mkiso.sh [extra kernel options]
#
# The iso9660 part (label LBBOOT) carries everything: the boot loader, kernel,
# initramfs and system.sfs, so tools that copy files instead of the image
# (rufus in "iso image" mode, or a plain copy onto a fat32 stick) end up with a
# stick that boots and is still a normal stick. For the image itself, uefi
# boots from a small fat partition appended to it with the boot loader, kernel
# and initramfs again, because firmware can't read iso9660. No root needed.
#
# An iso can't hold changes. "lifeboat persist" (9 in the rescue tools) adds a
# LIFEBOAT partition in the free space after it on the stick.
set -euo pipefail

top=$(cd "$(dirname "$0")/.." && pwd)
out=$top/out
root=$top/work/rootfs
work=$top/work/iso
extra=${1:-}

for f in vmlinuz initramfs.img system.sfs hw.sfs; do
    [ -f "$out/$f" ] || { echo "out/$f is missing (./build.sh all)" >&2; exit 1; }
done

rm -rf "$work"
mkdir -p "$work/tree/lifeboat" "$work/loader/entries" "$work/tree/EFI/BOOT"
for f in system.sfs hw.sfs; do
    ln "$out/$f" "$work/tree/lifeboat/$f" 2>/dev/null || cp "$out/$f" "$work/tree/lifeboat/$f"
done

cat > "$work/loader/loader.conf" <<END
default lifeboat.conf
timeout 2
editor yes
END
cat > "$work/loader/entries/lifeboat.conf" <<END
title   lifeboat
linux   /lifeboat/vmlinuz
initrd  /lifeboat/initramfs.img
options quiet loglevel=3 $extra
END
cat > "$work/loader/entries/lifeboat-fresh.conf" <<END
title   lifeboat, ignoring what is saved on the stick
linux   /lifeboat/vmlinuz
initrd  /lifeboat/initramfs.img
options quiet loglevel=3 lifeboat.fresh $extra
END

# the same boot files inside the iso9660 tree, for copying onto a fat stick
cp "$root/usr/lib/systemd/boot/efi/systemd-bootx64.efi" "$work/tree/EFI/BOOT/BOOTX64.EFI"
cp -r "$work/loader" "$work/tree/loader"
cp "$out/vmlinuz" "$out/initramfs.img" "$work/tree/lifeboat/"
cp "$root/etc/lifeboat-version" "$work/tree/lifeboat/version"
cat > "$work/tree/README.txt" <<END
lifeboat

This stick boots lifeboat on UEFI machines (Secure Boot off). It is still a
normal stick: anything else you put on it stays.

Logins: user "lifeboat", password "user" (has sudo). root's password is
"root1234".

To save sessions on it, boot it, open DISKS on the home screen, pick this stick,
press d and then 9. That makes lifeboat/save.img (up to 4 GB) next to these
files. Deleting that file deletes the saved session.
END

esp=$work/efi.img
mb=$(( ($(stat -c %s "$out/vmlinuz") + $(stat -c %s "$out/initramfs.img")) / 1048576 + 8 ))
truncate -s "${mb}M" "$esp"
mformat -i "$esp" -v LBEFI ::
mmd -i "$esp" ::/EFI ::/EFI/BOOT ::/loader ::/loader/entries ::/lifeboat
mcopy -i "$esp" "$root/usr/lib/systemd/boot/efi/systemd-bootx64.efi" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$esp" "$out/vmlinuz" "$out/initramfs.img" ::/lifeboat/
mcopy -i "$esp" "$work/loader/loader.conf" ::/loader/
mcopy -i "$esp" "$work/loader/entries/lifeboat.conf" "$work/loader/entries/lifeboat-fresh.conf" ::/loader/entries/

xorriso -as mkisofs -quiet -o "$out/lifeboat.iso" -V LBBOOT -R -J -iso-level 3 \
    -partition_offset 16 \
    -append_partition 2 C12A7328-F81F-11D2-BA4B-00A0C93EC93B "$esp" -appended_part_as_gpt \
    -e --interval:appended_partition_2:all:: -no-emul-boot \
    "$work/tree"
rm -rf "$work"
echo "==> out/lifeboat.iso $(du -h "$out/lifeboat.iso" | cut -f1)"
