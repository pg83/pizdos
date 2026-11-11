//go:build linux && amd64

package main

import (
	"bytes"
	_ "embed"
	"encoding/binary"
	"io"
	"os"
	"strings"
	"sync"
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

type CompilerImports struct{}

type CompilerIO struct {
	input      io.Reader
	output     io.Writer
	diagnostic bytes.Buffer
}

func (CompilerImports) function(imp *ImportMeta) any {
	if imp.module != "wasi_snapshot_preview1" {
		return nil
	}

	switch imp.name {
	case "fd_read":
		return compilerRead
	case "fd_write":
		return compilerWrite
	case "proc_exit":
		return compilerExit
	}

	return nil
}

func (CompilerImports) global(*ImportMeta) uintptr {
	return 0
}

func (CompilerImports) memory(*ImportMeta) *LinearMemory {
	return nil
}

func (CompilerImports) table(*ImportMeta) uintptr {
	return 0
}

func compilerTransfer(ctx uintptr, fd, iovs, count, done uint32, write bool) uint32 {
	m := guest(ctx)
	host := instanceAt(ctx).host.(*CompilerIO)

	if count > 1024 || span(m, done, 4) == nil {
		return wasiEfault
	}

	table := span(m, iovs, uint64(count)*8)

	if table == nil {
		return wasiEfault
	}

	if !write && fd != 0 || write && fd != 1 && fd != 2 {
		return wasiEbadf
	}

	total := uint64(0)

	for j := uint32(0); j < count; j++ {
		at := binary.LittleEndian.Uint32(table[j*8:])
		size := binary.LittleEndian.Uint32(table[j*8+4:])
		b := span(m, at, uint64(size))

		if b == nil {
			return wasiEfault
		}

		var n int
		var err error

		if write {
			var out io.Writer = host.output

			if fd == 2 {
				out = &host.diagnostic
			}

			n, err = out.Write(b)
		} else {
			n, err = host.input.Read(b)
		}

		if err != nil && err != io.EOF {
			throw(err)
		}

		total += uint64(n)

		if total > 0xffffffff {
			return wasiEinval
		}

		if n < len(b) || err == io.EOF {
			break
		}
	}

	put32(m, done, uint32(total))

	return wasiOK
}

func compilerRead(ctx uintptr, fd, iovs, count, done uint32) uint32 {
	return compilerTransfer(ctx, fd, iovs, count, done, false)
}

func compilerWrite(ctx uintptr, fd, iovs, count, done uint32) uint32 {
	return compilerTransfer(ctx, fd, iovs, count, done, true)
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

func compileModule(data []byte) []byte {
	return compileModuleWithOptions(data, compileOptions())
}

func compileModuleWithOptions(data []byte, options CompileOptions) []byte {
	var out bytes.Buffer
	compileStreams(bytes.NewReader(data), &out, options)

	return out.Bytes()
}
