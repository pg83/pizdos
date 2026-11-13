# pizd0s

[![codecov](https://codecov.io/gh/pizd0s/kernel/branch/main/graph/badge.svg)](https://app.codecov.io/gh/pizd0s/kernel)

An operating system whose kernel is an ordinary program: a WebAssembly VM
written in Go. A process is a wasm module run by a goroutine; a system call
is a call of a kernel import. Bare metal is a port of that program to
hardware, not a different kernel.

Also see https://www.destroyallsoftware.com/talks/the-birth-and-death-of-javascript
