#!/bin/sh
set -eu

project=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
if [ "$(id -u)" -ne 0 ] || [ "$(uname -m)" != aarch64 ]; then
    echo 'Build on ARM64 Linux with sudo.' >&2
    exit 1
fi
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
mirror=https://snapshot.debian.org/archive/debian/20261124T000000Z/
engine=2.54.3-1
runtime_packages="libwpewebkit-2.0-1=$engine,libevdev2,libegl1,libegl-mesa0,libgles2,libgl1-mesa-dri,libgbm1,mesa-libgallium,ca-certificates,glib-networking,fonts-dejavu-core,fonts-noto-cjk,bubblewrap,xdg-dbus-proxy,mount,util-linux"
build_packages="libwpewebkit-2.0-dev=$engine,libevdev-dev,libgbm-dev,libc6-dev,gcc,rustc,pkgconf"

bootstrap() {
    mmdebstrap --mode=root --variant=minbase --architectures=arm64 \
        --aptopt='Acquire::Check-Valid-Until "false"' \
        --aptopt='APT::Install-Recommends "false"' \
        --include="$1" --format=directory forky "$2" "$mirror"
}

bootstrap "$build_packages" "$build_dir/build"
mkdir "$build_dir/build/project"
cp -R "$project/src" "$project/ui" "$build_dir/build/project/"
chroot "$build_dir/build" /bin/sh -ec '
    cd /project
    flags=$(pkg-config --cflags wpe-webkit-2.0 wpe-platform-2.0 libevdev glib-2.0)
    libs=$(pkg-config --libs-only-l wpe-webkit-2.0 wpe-platform-2.0 libevdev glib-2.0)
    cc -Wall -Wextra -Werror -std=gnu17 -O2 $flags -c src/platform.c -o platform.o
    set -- $libs
    rustc --edition=2024 -C opt-level=2 -C strip=symbols -C link-arg=platform.o \
        $(printf "%s\n" "$@" | sed "s/^/-C link-arg=/") src/main.rs -o gizlet-browser
'

runtime="$build_dir/runtime"
bootstrap "$runtime_packages" "$runtime"
install -m 0755 "$build_dir/build/project/gizlet-browser" "$runtime/usr/bin/gizlet-browser"
mkdir -p "$runtime/proc" "$runtime/sys" "$runtime/run" "$runtime/dev" "$runtime/tmp"
rm -f "$runtime/etc/resolv.conf"
touch "$runtime/etc/resolv.conf"
rm -rf "$runtime/var/lib/apt/lists/"* "$runtime/var/cache/apt/"*
find "$runtime/dev" -mindepth 1 -delete
find "$runtime/usr/share/fonts/opentype/noto" -name '*.ttc' ! -name NotoSansCJK-Regular.ttc -delete
test -x "$runtime/usr/bin/bwrap"
test -x "$runtime/usr/lib/aarch64-linux-gnu/wpe-webkit-2.0/WPEWebProcess" ||
    find "$runtime/usr" -type f -name WPEWebProcess -perm -111 | grep -q .

app="$build_dir/app"
mkdir -p "$app/bin" "$project/dist"
install -m 0755 "$project/scripts/start.sh" "$app/bin/start"
install -m 0755 "$project/scripts/session.sh" "$app/bin/session"
install -m 0644 "$project/manifest.toml" "$app/manifest.toml"
mksquashfs "$runtime" "$app/runtime.sqfs" -comp zstd -noappend -no-progress
"$app/bin/start" --smoke
python3 "$project/scripts/pack.py" "$app" "$project/dist/io.gizlet.browser-0.1.0-arm64.gzl"
