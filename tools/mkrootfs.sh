#!/bin/bash
# Build work/rootfs from Arch packages, layer rootfs/ on top, squash it.
# Runs as root (build.sh starts it through pkexec).
#   mkrootfs.sh <top dir> <uid to hand the results back to>
set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/bin

top=$1
uid=$2
root=$top/work/rootfs
out=$top/out

[ "$(id -u)" = 0 ] || { echo "needs root" >&2; exit 1; }
[ -f "$top/rootfs/packages.list" ] || { echo "$top is not the lifeboat tree" >&2; exit 1; }

mapfile -t pkgs < <(grep -vE '^\s*(#|$)' "$top/rootfs/packages.list")

if [ -d "$root" ]; then
    # leftovers from a run that died inside the chroot
    { grep -o "$root[^ ]*" /proc/mounts || true; } | sort -r | xargs -r umount -l
    rm -rf --one-file-system "$root"
fi
mkdir -p "$root" "$out"
pacstrap -c "$root" "${pkgs[@]}"

# pacstrap copies this machine's mirror list. keep only its https mirrors,
# behind the worldwide redirector, so the list works anywhere
{
    echo 'Server = https://geo.mirror.pkgbuild.com/$repo/os/$arch'
    grep '^Server = https://' /etc/pacman.d/mirrorlist | grep -v geo.mirror.pkgbuild.com || true
} > "$root/etc/pacman.d/mirrorlist"

# the kernel: our own build if there is one, otherwise the newest Arch kernel
# installed on this machine
if [ -f "$out/vmlinuz-lifeboat" ] && [ -d "$top/work/modules/lib/modules" ]; then
    kdir=$(ls -d "$top"/work/modules/lib/modules/* | tail -n1)
    cp "$out/vmlinuz-lifeboat" "$out/vmlinuz"
else
    kdir=$(ls -d /usr/lib/modules/*/vmlinuz | sort -V | tail -n1)
    kdir=${kdir%/vmlinuz}
    cp "$kdir/vmlinuz" "$out/vmlinuz"
fi
kver=${kdir##*/}
echo "$kver" > "$out/kernel-version"
echo "==> kernel $kver"
mkdir -p "$root/usr/lib/modules"
cp -a "$kdir" "$root/usr/lib/modules/"
rm -f "$root/usr/lib/modules/$kver"/{vmlinuz,build,source}

cp -a "$top/rootfs/." "$root/"
rm -f "$root/packages.list"
install -m755 "$out/lifeboat" "$root/usr/local/bin/lifeboat"
install -m755 "$out/lifeboat-splash" "$root/usr/local/bin/lifeboat-splash"

# yay is an aur package, so it can't come from pacstrap. it is one static
# binary, taken from this machine
if [ -x /usr/bin/yay ]; then
    install -m755 /usr/bin/yay "$root/usr/local/bin/yay"
else
    echo "==> WARNING: no yay on this machine, the image gets no aur support"
fi
ver=$(date +%Y.%m.%d-%H%M)
printf '%s\n' "$ver" > "$root/etc/lifeboat-version"
printf '%s\n' "$ver" > "$out/version"
chown -R 0:0 "$root/etc" "$root/usr/local"
# same clock as the machine it was built on
ln -sf "$(readlink -f /etc/localtime)" "$root/etc/localtime"

arch-chroot "$root" /usr/local/lib/lifeboat/setup.sh
arch-chroot "$root" depmod -a "$kver"

echo "==> rootfs $(du -sh "$root" | cut -f1), squashing"
# two parts: the system, and the drivers (firmware and modules, which are big,
# already compressed, and mostly for other machines). see initramfs/init
rm -f "$out/system.sfs" "$out/hw.sfs"
mksquashfs "$root" "$out/system.sfs" -comp zstd -Xcompression-level 15 -b 1M \
    -noappend -no-progress -wildcards \
    -e 'var/cache/pacman/pkg/*' 'usr/lib/firmware/*' 'usr/lib/modules/*' >/dev/null
mksquashfs "$root/usr/lib/firmware" "$root/usr/lib/modules" "$out/hw.sfs" \
    -comp zstd -Xcompression-level 3 -b 1M -noappend -no-progress >/dev/null
# whole 64K regions, which is what dm-clone moves them in (ram.c). squashfs
# ignores what's after its end
truncate -s %64K "$out/system.sfs" "$out/hw.sfs"
chown "$uid" "$out"/* "$top/work"
echo "==> system.sfs $(du -h "$out/system.sfs" | cut -f1), hw.sfs $(du -h "$out/hw.sfs" | cut -f1)"
