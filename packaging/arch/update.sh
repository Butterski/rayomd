#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Sets both Arch packages to a release: pkgver, pkgrel, sha256sums and .SRCINFO.
#
#   packaging/arch/update.sh 4.3.1
#
# Run it as a normal user on Arch (makepkg refuses root) once the release and its assets are
# published. A new version resets pkgrel to 1; the same version with a changed checksum (assets
# rebuilt) raises it; an unchanged package is left alone.
set -eu

version=$1
dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
url=https://github.com/Butterski/rayomd

checksum() {
    curl -fsSL --retry 5 "$1" | sha256sum | cut -d ' ' -f 1
}

update() {
    pkg=$1
    sum=$2
    file="$dir/$pkg/PKGBUILD"
    old_version=$(sed -n 's/^pkgver=//p' "$file")
    old_release=$(sed -n 's/^pkgrel=//p' "$file")
    old_sum=$(sed -n "s/^sha256sums=('\([0-9a-f]*\)')\$/\1/p" "$file")
    if [ -z "$old_sum" ]; then
        echo "$file: expected one sha256sums=('...') line" >&2
        exit 1
    fi
    if [ "$old_version" = "$version" ]; then
        if [ "$old_sum" = "$sum" ]; then
            echo "$pkg: already $version-$old_release"
            return 0
        fi
        release=$((old_release + 1))
    else
        release=1
    fi
    sed -i -e "s/^pkgver=.*/pkgver=$version/" -e "s/^pkgrel=.*/pkgrel=$release/" \
        -e "s/^sha256sums=.*/sha256sums=('$sum')/" "$file"
    (cd "$dir/$pkg" && makepkg --printsrcinfo > .SRCINFO)
    echo "$pkg: $version-$release"
}

update rayomd "$(checksum "$url/archive/refs/tags/v$version.tar.gz")"
update rayomd-bin "$(checksum "$url/releases/download/v$version/rayomd-$version-linux-x64.tar.gz")"
