#!/bin/sh
set -eu

image=$1
root=$2
shift 2
mount -t squashfs -o loop,ro,nodev,nosuid "$image" "$root"
mount -t proc -o nosuid,nodev,noexec proc "$root/proc"
mount -t tmpfs -o nosuid,mode=0755 tmpfs "$root/dev"
mount -t tmpfs -o nosuid,nodev,mode=1777 tmpfs "$root/tmp"
mount -t tmpfs -o nosuid,nodev,mode=0755 tmpfs "$root/run"
mount --bind /sys "$root/sys"
mount -o remount,bind,ro,nosuid,nodev,noexec "$root/sys"

for device in null zero random urandom full; do
    touch "$root/dev/$device"
    mount --bind "/dev/$device" "$root/dev/$device"
done
mkdir -m 1777 "$root/dev/shm"
ln -s /proc/self/fd "$root/dev/fd"

groups=65534
if [ "${1:-}" != "--smoke" ]; then
    for source in /dev/dri /dev/input /run/udev; do
        [ -d "$source" ] || continue
        mkdir -p "$root$source"
        mount --bind "$source" "$root$source"
        mount -o remount,bind,ro,nosuid,noexec "$root$source"
    done
    for device in /dev/dri/card* /dev/dri/renderD* /dev/input/event*; do
        [ -c "$device" ] || continue
        group=$(stat -c '%g' "$device")
        [ "$group" -eq 0 ] || groups="$groups,$group"
    done
fi

if [ -r /etc/resolv.conf ]; then
    cp -L /etc/resolv.conf "$root/run/resolv.conf"
    chmod 644 "$root/run/resolv.conf"
    mount --bind "$root/run/resolv.conf" "$root/etc/resolv.conf"
fi
mkdir -m 700 "$root/run/browser"
chown 65534:65534 "$root/run/browser"

cd "$root"
pivot_root . .
umount --internal-only --no-canonicalize --lazy .
exec env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin \
    HOME=/run/browser XDG_RUNTIME_DIR=/run/browser LANG=C.UTF-8 \
    GIZLET_SWAP_AB="${GIZLET_SWAP_AB:-0}" \
    setpriv --reuid=65534 --regid=65534 --groups="$groups" --no-new-privs \
    /usr/bin/gizlet-browser "$@"
