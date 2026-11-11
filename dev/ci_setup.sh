#!/bin/bash
set -euo pipefail

sudo apt-get update -qq
sudo apt-get install -y --no-install-recommends ca-certificates curl cmake ninja-build musl-dev wabt
curl --fail --silent --show-error --location https://apt.llvm.org/llvm-snapshot.gpg.key -o "$RUNNER_TEMP/llvm.asc"
sudo install -m 644 "$RUNNER_TEMP/llvm.asc" /usr/share/keyrings/llvm.asc
printf '%s\n' 'deb [signed-by=/usr/share/keyrings/llvm.asc] https://apt.llvm.org/noble/ llvm-toolchain-noble-23 main' | sudo tee /etc/apt/sources.list.d/llvm-23.list
sudo apt-get update -qq
sudo apt-get install -y --no-install-recommends clang-23 clang-tools-23 lld-23 llvm-23 llvm-23-dev libclang-rt-23-dev

printf '%s\n' /usr/lib/llvm-23/bin >> "$GITHUB_PATH"
# Ubuntu's compiler-rt uses glibc. Instrument the compiler with its host
# headers/libraries; executable C++ references use musl as in local runs.
cat >> "$GITHUB_ENV" <<'EOF'
CPPFLAGS=-nostdinc -isystem /usr/include/x86_64-linux-musl -isystem /usr/lib/llvm-23/lib/clang/23/include
LDFLAGS=-static -nostdlib -fuse-ld=lld -x none /usr/lib/x86_64-linux-musl/crt1.o /usr/lib/x86_64-linux-musl/crti.o -L/usr/lib/x86_64-linux-musl -lc /usr/lib/x86_64-linux-musl/crtn.o
COVERAGE_CPPFLAGS=
COVERAGE_LDFLAGS=-static -fuse-ld=lld --rtlib=compiler-rt
BUILD_JOBS=2
EOF
