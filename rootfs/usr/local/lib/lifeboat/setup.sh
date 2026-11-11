#!/bin/bash
# Runs inside work/rootfs under arch-chroot, once, after pacstrap.
set -euo pipefail

ln -sf busybox /usr/bin/init
# systemd's poweroff and reboot need systemd running. busybox's talk to our init
for applet in poweroff reboot halt; do
    ln -sf /usr/bin/busybox /usr/local/bin/$applet
done

for g in wheel seat video render input audio power storage; do
    groupadd -f "$g"
done
useradd -m -U -u 1000 \
    -G wheel,seat,video,render,input,audio,power,storage \
    -s /bin/bash lifeboat
# known defaults, written on the stick's README too
echo 'lifeboat:user' | chpasswd
echo 'root:root1234' | chpasswd
chmod 700 /home/lifeboat
install -d -o lifeboat -g lifeboat /home/lifeboat/Applications /home/lifeboat/Downloads

[ -e /etc/localtime ] || ln -sf /usr/share/zoneinfo/UTC /etc/localtime
# the shell integration that prints an escape sequence around every command
rm -f /etc/profile.d/80-systemd-osc-context.sh /usr/lib/systemd/profile.d/80-systemd-osc-context.sh
dbus-uuidgen --ensure
mkdir -p /var/lib/iwd /var/lib/bluetooth /mnt
chmod 700 /var/lib/iwd
chmod 600 /etc/doas.conf
# sudo for wheel, with the user's password
echo '%wheel ALL=(ALL:ALL) ALL' > /etc/sudoers.d/10-wheel
chmod 440 /etc/sudoers.d/10-wheel

# pacman works at runtime, but there is no point shipping a stale sync db
rm -rf /var/cache/pacman/pkg/* /var/lib/pacman/sync/*

# shave the image
rm -rf /usr/share/doc /usr/share/info /usr/share/gtk-doc /usr/include
# man pages stay, but only the english ones
find /usr/share/man -mindepth 1 -maxdepth 1 ! -name 'man*' -exec rm -rf {} +
mandb -q 2>/dev/null || true
find /usr/share/locale -mindepth 1 -maxdepth 1 \
    ! -name 'en*' ! -name 'locale.alias' -exec rm -rf {} +
find /usr/lib -name '*.a' -delete
find /usr/lib/python3* -depth \( -name test -o -name tests -o -name idlelib -o -name tkinter \
    -o -name ensurepip -o -name __pycache__ \) -exec rm -rf {} + 2>/dev/null || true

ldconfig
