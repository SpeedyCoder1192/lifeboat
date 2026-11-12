#!/bin/bash
# Make a patch from an older lifeboat iso to the files in out/.
#   mkpatch.sh <top> <old.iso> [new-version]
#
# The result is out/patches/lifeboat-<old-version>.patch, a tar.zst holding a
# manifest, the new version, and a zstd binary patch for each big file. The
# updater applies it with the old file as the reference, so a stick downloads
# and writes only the difference instead of the whole 1.2G iso.
set -euo pipefail

top=$1
oldiso=$2
out=$top/out
newver=${3:-}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

[ -f "$oldiso" ] || { echo "$oldiso not found" >&2; exit 1; }
for f in vmlinuz initramfs.img system.sfs hw.sfs; do
    [ -f "$out/$f" ] || { echo "out/$f is missing (./build.sh all)" >&2; exit 1; }
done
if [ -z "$newver" ]; then
    newver=$(cat "$top/out/version" 2>/dev/null ||
             cat "$top/work/rootfs/etc/lifeboat-version" 2>/dev/null || true)
fi
[ -n "$newver" ] || { echo "no new version; build the rootfs first or pass one" >&2; exit 1; }

echo "==> reading $oldiso"
bsdtar -xf "$oldiso" -C "$tmp" lifeboat/vmlinuz lifeboat/initramfs.img \
    lifeboat/system.sfs lifeboat/hw.sfs lifeboat/version
oldver=$(cat "$tmp/lifeboat/version")
[ -n "$oldver" ] || { echo "$oldiso has no lifeboat/version" >&2; exit 1; }
[ "$oldver" != "$newver" ] || { echo "old and new are both $oldver" >&2; exit 1; }

bundle=$tmp/bundle
mkdir -p "$bundle" "$out/patches"
printf 'from %s\nto %s\n' "$oldver" "$newver" > "$bundle/manifest"
printf '%s\n' "$newver" > "$bundle/version"

for f in vmlinuz initramfs.img system.sfs hw.sfs; do
    old=$tmp/lifeboat/$f
    new=$out/$f
    sha=$(sha256sum "$new" | cut -d' ' -f1)
    nsize=$(stat -c %s "$new")
    made=0
    if [ -f "$old" ]; then
        if zstd -q -15 --long=31 --patch-from="$old" "$new" -o "$tmp/$f.patch" 2>/dev/null; then
            psize=$(stat -c %s "$tmp/$f.patch")
            if [ "$psize" -lt "$nsize" ]; then
                mv "$tmp/$f.patch" "$bundle/$f.patch"
                printf 'file %s patch %s %s\n' "$f" "$sha" "$nsize" >> "$bundle/manifest"
                made=1
            fi
        fi
    fi
    if [ "$made" = 0 ]; then
        cp "$new" "$bundle/$f"
        printf 'file %s full %s %s\n' "$f" "$sha" "$nsize" >> "$bundle/manifest"
    fi
done

patch=$out/patches/lifeboat-$oldver.patch
tar -C "$bundle" -cf - . | zstd -q -19 -f -o "$patch"
echo "==> $patch ($(du -h "$patch" | cut -f1)), $oldver -> $newver"
