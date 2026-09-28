#!/usr/bin/env bash
set -euo pipefail

cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
version=$(<VERSION)
dist="$PWD/dist"
if ! command -v dh >/dev/null; then
    printf 'Build on Debian/Ubuntu with build-essential, debhelper and python3 installed.\n' >&2
    exit 1
fi

work=$(mktemp -d "$dist/.deb.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
tar -xzf "$dist/dell-rf-$version.tar.gz" -C "$work"
cd -- "$work/dell-rf-$version"
dpkg-buildpackage --build=binary --no-sign

packages=("$work"/*.deb)
for package in "${packages[@]}"; do
    cp -- "$package" "$dist/"
done
printf 'Debian packages: %s\n' "$dist"
