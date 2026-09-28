#!/usr/bin/env bash
set -euo pipefail

cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
version=$(<VERSION)
if [[ ! $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    printf 'Invalid release version: %s\n' "$version" >&2
    exit 1
fi

debian_version=$(sed -n '1s/^dell-rf (\([^)]*\)).*/\1/p' debian/changelog)
if [[ ${debian_version%-*} != "$version" ]]; then
    printf 'VERSION and debian/changelog disagree.\n' >&2
    exit 1
fi

epoch=${SOURCE_DATE_EPOCH:-0}
if [[ ! $epoch =~ ^[0-9]+$ ]]; then
    printf 'SOURCE_DATE_EPOCH must be a non-negative integer.\n' >&2
    exit 1
fi

sources=(
    VERSION Makefile LICENSE README.md .clang-format
    src/*.c include/*.h tests/*.c tests/*.py tests/fixtures/*.h
    docs/*.md udev/*.rules packaging/*.sh packaging/arch/PKGBUILD.in
    debian/control debian/changelog debian/copyright debian/rules
    debian/docs debian/dell-rf.udev debian/source/format
)

mkdir -p dist/aur
archive="dist/dell-rf-$version.tar.gz"
temporary=$(mktemp "dist/.source.XXXXXX")
trap 'rm -f -- "$temporary"' EXIT

tar --sort=name --mtime="@$epoch" --owner=0 --group=0 --numeric-owner \
    --transform="flags=r;s,^,dell-rf-$version/," -cf - -- "${sources[@]}" |
    gzip -n > "$temporary"
mv -- "$temporary" "$archive"

checksum=$(sha256sum "$archive")
checksum=${checksum%% *}
sed -e "s/@VERSION@/$version/g" -e "s/@SOURCE_SHA256@/$checksum/g" \
    packaging/arch/PKGBUILD.in > dist/aur/PKGBUILD

printf 'Source: %s\nArch recipe: dist/aur/PKGBUILD\n' "$archive"
