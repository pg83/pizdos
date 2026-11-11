# pizd0s

An operating system whose kernel is an ordinary program: a WebAssembly VM
written in Go. A process is a wasm module run by a goroutine; a system call
is a call of a kernel import. Bare metal is a port of that program to
hardware, not a different kernel.

CI builds the native and Wasm compilers with Clang 23, runs the executable
C++ cases, Python CLI checks and WebAssembly spec suite, and uploads native
AOT, Wasm AOT and VM coverage to Codecov using OIDC. Run `./build coverage`
to collect the reports and `./build coverage_check` to enforce the 99% floor.
The host needs Clang's profiling runtime, matching `llvm-profdata` and
`llvm-cov`, Go 1.25, WABT, CMake, Ninja and LLVM/Clang tablegen tools.

The AOT floor and Codecov count LLVM source lines excluding standalone
`{` and `}` lines. `.build/coverage/summary.txt` also retains both raw LLVM
percentages; the unmodified LCOV reports and Go profile remain alongside
the Codecov files. VM coverage uses Go statement counts. Normal and chaos
builds are separate, and the report combines their executed paths.
