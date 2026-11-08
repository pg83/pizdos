#!/bin/sh
# Build the full stand with the same ix revision that built decode release 16.
# The checkout is private to this stand; the caller's ix tree is not changed.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
rev=53f12e265c480e7849bfa58eedf608db63020b1b
tree=$root/.build-stand/ix

if [ ! -d "$tree/.git" ]; then
    mkdir -p "$tree"
    git -C "$tree" init -q
    git -C "$tree" fetch -q --depth 1 https://github.com/pg83/ix "$rev"
    git -C "$tree" checkout -q --detach FETCH_HEAD
fi

test "$(git -C "$tree" rev-parse HEAD)" = "$rev"
cd "$root"
export STAND_IX_REV=$rev
exec "$tree/ix" run lib/image/magick lib/pdf/ium lib/djvulibre lib/magic lib/svg \
    lib/z lib/png lib/jpeg lib/webp lib/gif bld/pkg/config bld/compiler bld/wabt bin/zstd \
    -- ./build stand "$@"
