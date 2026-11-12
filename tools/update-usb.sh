#!/bin/bash
# Apply a patch to an existing lifeboat stick, keeping its saved changes.
#   update-usb.sh <top> /dev/sdX [patch]
#   update-usb.sh <top> some.img [patch]     an image file, for testing
#
# Without a patch argument it looks in out/patches/ for one matching the
# stick's version, then downloads lifeboat-<version>.patch from the latest
# GitLab release. It never repartitions or touches the save partition.
set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/bin

top=$1
dev=$2
patch=${3:-}
out=$top/out
mnt=$(mktemp -d)
tmp=$(mktemp -d)
loop=
trap 'umount "$mnt" 2>/dev/null || true; rmdir "$mnt" 2>/dev/null || true; rm -rf "$tmp"; [ -z "$loop" ] || losetup -d "$loop"' EXIT

[ "$(id -u)" = 0 ] || { echo "needs root" >&2; exit 1; }
if [ -f "$dev" ]; then
    losetup -j "$dev" | cut -d: -f1 | xargs -r -n1 losetup -d
    loop=$(losetup -P --find --show "$dev")
    dev=$loop
    udevadm settle
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

parts() { lsblk -lnpo NAME,TYPE "$dev" | awk '$2 == "part" {print $1}'; }
esp=$(parts | sed -n 1p)
[ -n "$esp" ] || { echo "$dev has no partitions" >&2; exit 1; }
mount -o rw "$esp" "$mnt"
ver=$(cat "$mnt/lifeboat/version" 2>/dev/null || true)
[ -n "$ver" ] || { echo "$esp has no lifeboat/version, so it isn't a lifeboat stick" >&2; exit 1; }
echo "==> stick is $ver"

if [ -n "$patch" ]; then
    [ -f "$patch" ] || { echo "$patch not found" >&2; exit 1; }
elif [ -f "$out/patches/lifeboat-$ver.patch" ]; then
    patch=$out/patches/lifeboat-$ver.patch
else
    url="https://gitlab.com/aahiluzzama27/lifeboat/-/releases/permalink/latest/downloads/lifeboat-$ver.patch"
    echo "==> downloading $url"
    if ! curl -fL --progress-bar -o "$tmp/lifeboat.patch" "$url"; then
        echo "no patch from $ver is published." >&2
        echo "use ./build.sh usb $dev for a full write, or ./build.sh patch OLD.iso first" >&2
        exit 1
    fi
    patch=$tmp/lifeboat.patch
fi

echo "==> applying $(basename "$patch")"
zstd -q -dc "$patch" | tar -xf - -C "$tmp"
[ -f "$tmp/manifest" ] || { echo "that patch has no manifest" >&2; exit 1; }
from=$(awk '$1 == "from" {print $2}' "$tmp/manifest")
to=$(awk '$1 == "to" {print $2}' "$tmp/manifest")
[ "$from" = "$ver" ] || { echo "the patch is from $from, but the stick is $ver" >&2; exit 1; }

# writing each .new file next to the old one needs room for the biggest one
need=$(awk '$1 == "file" && $5 > m {m = $5} END {print m + 0}' "$tmp/manifest")
free=$(df -Pm "$mnt" | awk 'NR == 2 {print $4}')
if [ "$need" -gt 0 ] && [ "$free" -lt $(( need / 1048576 + 32 )) ]; then
    echo "not enough room on $esp: need about $(( need / 1048576 + 32 ))M, have ${free}M" >&2
    exit 1
fi

while read -r tag name kind sha size; do
    [ "$tag" = file ] || continue
    old=$mnt/lifeboat/$name
    new=$mnt/lifeboat/$name.new
    echo "==> $name"
    if [ "$kind" = patch ]; then
        zstd -q -d --long=31 --patch-from="$old" "$tmp/$name.patch" -o "$new"
    else
        cp "$tmp/$name" "$new"
    fi
    got=$(sha256sum "$new" | awk '{print $1}')
    if [ "$got" != "$sha" ]; then
        rm -f "$new"
        echo "$name did not verify (got $got, wanted $sha)" >&2
        exit 1
    fi
    mv "$new" "$old"
done < "$tmp/manifest"

cp "$tmp/version" "$mnt/lifeboat/version"
sync
umount "$mnt"
echo "==> updated $ver -> $to"
