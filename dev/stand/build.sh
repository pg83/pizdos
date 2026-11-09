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
python3 dev/stand/scalar.py "$tree"
export STAND_IX_REV=$rev
export STAND_NATIVE_SCALAR=1
exec "$tree/ix" run lib/image/magick --stand_scalar=1 lib/pdf/ium --stand_scalar=1 \
    lib/djvulibre --stand_scalar=1 lib/magic --stand_scalar=1 lib/svg --stand_scalar=1 \
    lib/z --stand_scalar=1 lib/png --stand_scalar=1 lib/jpeg --stand_scalar=1 \
    lib/webp --stand_scalar=1 lib/gif --stand_scalar=1 bld/pkg/config bld/compiler bin/zstd \
    -- ./build stand "$@"
