//go:build linux && amd64

package main

import (
	"bytes"
	_ "embed"
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"strings"
	"sync"
	"syscall"

	"github.com/klauspost/compress/zstd"
)

// The native bootstrap compiles aot.wasm to this object in build.py.
// No wasm decoder, optimizer or code generator lives in the Go host.
//
//go:embed aot.aot
var compilerBlob []byte

var compilerCode = sync.OnceValue(func() *Code {
	return link("aot", readObject(compilerBlob), CompilerImports{})
})

type CompileOptions struct {
	memoryTail  bool
	tailRanges  bool
	tailProfile bool
	// asan compiles every access with a check of the shadow map
	// (kernel_asan.go) in place of the tail's checks.
	asan bool
}

func compileOptions() CompileOptions {
	options := CompileOptions{memoryTail: true, tailRanges: true}

	switch mode := os.Getenv("W2X_MEMORY_TAIL"); mode {
	case "", "1", "optimized":
	case "plain":
		options.tailRanges = false
	case "strict":
		options.memoryTail, options.tailRanges = false, false
	default:
		throwFmt("unknown memory tail mode %q", mode)
	}

	options.tailProfile = options.tailRanges && os.Getenv("W2X_TAIL_PROFILE") != ""

	return options
}

type CompilerImports struct{ FunctionImports }

type CompilerIO struct {
	input      io.Reader
	output     io.Writer
	diagnostic bytes.Buffer
}

func (CompilerImports) function(imp *ImportMeta) any {
	// compilerBlob is fixed at build time: its only imports are these three
	// wasi_snapshot_preview1 functions, declared by aot.cpp.
	switch imp.name {
	case "fd_read":
		return compilerRead
	case "fd_write":
		return compilerWrite
	}

	return compilerExit
}

// The private compiler bridge serves aot.cpp's read and writeAll only.
// Both pass one valid iovec and a valid done pointer; reads use fd 0 and
// writes fd 1/2. Its single uint32-sized buffer also bounds the byte count.
func compilerTransfer(ctx uintptr, fd, iovs, done uint32, write bool) uint32 {
	m := guest(ctx)
	host := instanceAt(ctx).host.(*CompilerIO)
	table := span(m, iovs, 8)
	at := binary.LittleEndian.Uint32(table)
	size := binary.LittleEndian.Uint32(table[4:])
	b := span(m, at, uint64(size))
	var n int
	var err error

	if write {
		var out io.Writer = host.output

		if fd == 2 {
			out = &host.diagnostic
		}

		if chaosFail("compile-write") {
			err = io.ErrShortWrite
		} else {
			n, err = out.Write(b)
		}
	} else {
		if chaosFail("compile-read") {
			err = io.ErrUnexpectedEOF
		} else {
			n, err = host.input.Read(b)
		}
	}

	if err != nil && err != io.EOF {
		throw(err)
	}

	put32(m, done, uint32(n))

	return wasiOK
}

func compilerRead(ctx uintptr, fd, iovs, count, done uint32) uint32 {
	return compilerTransfer(ctx, fd, iovs, done, false)
}

func compilerWrite(ctx uintptr, fd, iovs, count, done uint32) uint32 {
	return compilerTransfer(ctx, fd, iovs, done, true)
}

func compilerExit(ctx uintptr, code uint32) {
	diagnostic := strings.TrimSpace(instanceAt(ctx).host.(*CompilerIO).diagnostic.String())
	throwFmt("aot exited with code %d: %s", code, diagnostic)
}

// Each invocation owns its instance and linear memory. The linked machine
// code is shared, and its WASI imports reach the invocation through ctx.
func compileStreams(input io.Reader, output io.Writer, options CompileOptions) {
	imports := CompilerImports{}
	i := newInstance(compilerCode(), imports, runStack)

	defer i.release()

	i.attach(&CompilerIO{input: input, output: output})
	i.start()
	setupCompilerChaos(i)
	flags := uint32(0)

	if options.memoryTail {
		flags |= 1
	}

	if options.tailRanges {
		flags |= 2
	}

	if options.tailProfile {
		flags |= 4
	}

	if options.asan {
		flags |= 8
	}

	if flags == 3 {
		entry, _ := i.export("compile")
		entry.Interface().(func(uintptr))(i.ctx())
	} else {
		entry, _ := i.export("compileOptions")
		entry.Interface().(func(uintptr, uint32))(i.ctx(), flags)
	}
}

func compileModuleWithOptions(data []byte, options CompileOptions) []byte {
	var out bytes.Buffer
	compileStreams(bytes.NewReader(data), &out, options)

	return out.Bytes()
}

// programOptions are the options a program is compiled with: the
// environment's, or with asan only the checks of the shadow map.
func programOptions(asan bool) CompileOptions {
	if asan {
		return CompileOptions{asan: true}
	}

	return compileOptions()
}

// wasmHeader begins a WebAssembly module, of version 1.
var wasmHeader = []byte("\x00asm\x01\x00\x00\x00")

// zstdMagic begins a Zstandard frame (RFC 8878): a file of a module may be
// the module compressed, as the image keeps its programs.
var zstdMagic = []byte{0x28, 0xb5, 0x2f, 0xfd}

// unpackLimit bounds what a compressed module unpacks to, 1 GiB, four
// times llvm's module: a small file does not take the host's memory.
const unpackLimit = 1 << 30

// unpacker decodes whole frames, every call on the caller's goroutine.
var unpacker = sync.OnceValue(func() *zstd.Decoder {
	return throw2(zstd.NewReader(nil, zstd.WithDecoderMaxMemory(unpackLimit)))
})

// unpack is the module a file's data is: the data, or what its zstd
// frames unpack to, skippable frames skipped; ENOEXEC, with why, when they
// do not.
func unpack(data []byte) ([]byte, error) {
	if !bytes.HasPrefix(data, zstdMagic) {
		return data, nil
	}

	module, err := unpacker().DecodeAll(data, nil)

	if err != nil {
		return nil, fmt.Errorf("zstd: %v: %w", err, syscall.ENOEXEC)
	}

	return module, nil
}

// compileModule compiles a module, or a file of one compressed by zstd, to
// its object: ENOEXEC for what is no module, with why for frames that do
// not unpack, or a module the compiler refuses, with its diagnostic, or
// traps on.
func compileModule(data []byte, options CompileOptions) (obj *Object, err error) {
	if data, err = unpack(data); err != nil {
		return nil, err
	}

	if !bytes.HasPrefix(data, wasmHeader) {
		return nil, syscall.ENOEXEC
	}

	if t := tryAs[*Trap](func() {
		if e := try(func() { obj = readObject(compileModuleWithOptions(data, options)) }); e != nil {
			err = fmt.Errorf("%s: %w", e, syscall.ENOEXEC)
		}
	}); t != nil {
		return nil, fmt.Errorf("the compiler: %s: %w", t.Error(), syscall.ENOEXEC)
	}

	return obj, err
}
