#!/usr/bin/env bash
set -euo pipefail

cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
version=$(<VERSION)
dist="$PWD/dist"
work=$(mktemp -d "$dist/.arch.XXXXXX")
trap 'rm -rf -- "$work"' EXIT

cp -- "$dist/aur/PKGBUILD" "$work/PKGBUILD"
cd -- "$work"
makepkg --printsrcinfo > "$dist/aur/.SRCINFO"
PKGDEST="$dist" SRCDEST="$dist" makepkg --cleanbuild --force

tar --sort=name --mtime="@${SOURCE_DATE_EPOCH:-0}" \
    --owner=0 --group=0 --numeric-owner \
    -C "$dist/aur" -cf - PKGBUILD .SRCINFO |
    gzip -n > "$dist/dell-rf-$version-aur.tar.gz"

printf 'Arch package and AUR files: %s\n' "$dist"
