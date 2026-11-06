// The kernel: a Go program that runs the system's wasm modules.
//
// The rest of the kernel sees the system through wireBoard: a Compiler and a
// root FS. The bootstrap fills it: it starts the embedded rootfs.wasm under
// wazero, takes aot from it and compiles rootfs and aot with aot.
package main

import (
	"bytes"
	"context"
	"crypto/sha256"
	_ "embed"
	"fmt"
	"os"
	"strings"
	"time"

	"github.com/tetratelabs/wazero"
	"github.com/tetratelabs/wazero/api"
	"github.com/tetratelabs/wazero/imports/wasi_snapshot_preview1"
)

//go:embed rootfs.wasm
var rootfsWasm []byte

// aot: a module's bytes in, the artifact's bytes out.
type Compiler interface {
	compile(wasm []byte) []byte
}

// The root filesystem.
type FS interface {
	readFile(path string) []byte
}

type WireBoard struct {
	compiler Compiler
	fs       FS
}

var wireBoard WireBoard

const (
	sIfmt  = 0o170000
	sIfdir = 0o040000
	sIflnk = 0o120000

	// Symlinks one path may pass through, as Linux's MAXSYMLINKS.
	maxSymlinks = 40
)

type Exception struct {
	what func() error
}

func (e *Exception) error() string {
	return e.what().Error()
}

func (e *Exception) Error() string {
	return e.error()
}

func (e *Exception) throw() {
	panic(e)
}

func (e *Exception) catch(cb func(*Exception)) {
	if e != nil {
		cb(e)
	}
}

func newException(err error) *Exception {
	return &Exception{
		what: func() error {
			return err
		},
	}
}

func throw(err error) {
	if err != nil {
		newException(err).throw()
	}
}

func throw2[T any](val T, err error) T {
	throw(err)

	return val
}

func throwFmt(format string, args ...any) {
	newException(fmt.Errorf(format, args...)).throw()
}

func try(cb func()) (err *Exception) {
	defer func() {
		if rec := recover(); rec != nil {
			if exc, ok := rec.(*Exception); ok {
				err = exc
			} else {
				panic(rec)
			}
		}
	}()

	cb()

	return nil
}

func report(format string, args ...any) {
	fmt.Printf("kernel: "+format+"\n", args...)
}

// A module under wazero, the bootstrap engine.
type Instance struct {
	module api.Module
}

func instantiate(runtime wazero.Runtime, name string, wasm []byte, start ...string) *Instance {
	ctx := context.Background()
	compiled := throw2(runtime.CompileModule(ctx, wasm))
	config := wazero.NewModuleConfig().WithName(name).WithStartFunctions(start...)

	return &Instance{module: throw2(runtime.InstantiateModule(ctx, compiled, config))}
}

// The single result of the export name, or 0 for none.
func (i *Instance) call(name string, args ...uint64) uint64 {
	function := i.module.ExportedFunction(name)

	if function == nil {
		throwFmt("%s exports no %s", i.module.Name(), name)
	}

	results := throw2(function.Call(context.Background(), args...))

	if len(results) == 0 {
		return 0
	}

	return results[0]
}

func (i *Instance) read(address uint32, size uint32) []byte {
	data, ok := i.module.Memory().Read(address, size)

	if !ok {
		throwFmt("%s: %d bytes at %#x are out of memory", i.module.Name(), size, address)
	}

	return append([]byte(nil), data...)
}

func (i *Instance) write(address uint32, data []byte) {
	if !i.module.Memory().Write(address, data) {
		throwFmt("%s: %d bytes at %#x are out of memory", i.module.Name(), len(data), address)
	}
}

// aot.wasm's blob interface (bin/aot/wasm).
type WasmCompiler struct {
	instance *Instance
}

func (c *WasmCompiler) compile(wasm []byte) []byte {
	address := uint32(c.instance.call("aot_input", uint64(len(wasm))))
	c.instance.write(address, wasm)
	status := c.instance.call("aot_compile")
	output := c.instance.read(uint32(c.instance.call("aot_output")), uint32(c.instance.call("aot_output_size")))

	if status != 0 {
		throwFmt("aot: %s", output)
	}

	return output
}

// rootfs.wasm's API (bin/rootfs/main.go).
type WasmFS struct {
	instance *Instance
	scratch  uint32
}

func (fs *WasmFS) check(result int64, what string) int64 {
	if result < 0 {
		throwFmt("rootfs: %s: errno %d", what, -result)
	}

	return result
}

func (fs *WasmFS) lookup(dir uint32, name string) uint32 {
	fs.instance.write(fs.scratch, []byte(name))

	return uint32(fs.check(int64(int32(fs.instance.call("fs_lookup", uint64(dir), uint64(len(name))))), name))
}

func (fs *WasmFS) mode(inode uint32) uint32 {
	return uint32(fs.check(int64(int32(fs.instance.call("fs_mode", uint64(inode)))), "mode"))
}

// Every byte of a file or a symlink's target, extent after extent.
func (fs *WasmFS) contents(inode uint32) []byte {
	var out []byte

	for {
		extent := fs.check(int64(fs.instance.call("fs_extent", uint64(inode), uint64(len(out)))), "extent")

		if extent == 0 {
			return out
		}

		out = append(out, fs.instance.read(uint32(extent>>32), uint32(extent))...)
	}
}

// The inode at an absolute path, through symlinks.
func (fs *WasmFS) walk(path string) uint32 {
	pending := strings.Split(strings.Trim(path, "/"), "/")
	trail := []uint32{0}
	links := 0

	for len(pending) > 0 {
		name := pending[0]
		pending = pending[1:]

		switch name {
		case "", ".":
			continue
		case "..":
			if len(trail) > 1 {
				trail = trail[:len(trail)-1]
			}

			continue
		}

		inode := fs.lookup(trail[len(trail)-1], name)

		if fs.mode(inode)&sIfmt != sIflnk {
			trail = append(trail, inode)

			continue
		}

		links++

		if links > maxSymlinks {
			throwFmt("rootfs: %s: too many symlinks", path)
		}

		target := string(fs.contents(inode))

		if strings.HasPrefix(target, "/") {
			trail = trail[:1]
		}

		pending = append(strings.Split(strings.Trim(target, "/"), "/"), pending...)
	}

	return trail[len(trail)-1]
}

func (fs *WasmFS) readFile(path string) []byte {
	inode := fs.walk(path)

	if fs.mode(inode)&sIfmt == sIfdir {
		throwFmt("rootfs: %s is a directory", path)
	}

	return fs.contents(inode)
}

// Start rootfs from the kernel's own copy, take aot from it, and compile
// rootfs and aot with aot.
func bootstrap() {
	runtime := wazero.NewRuntime(context.Background())
	wasi_snapshot_preview1.MustInstantiate(context.Background(), runtime)
	fsInstance := instantiate(runtime, "rootfs", rootfsWasm, "_initialize")
	fs := &WasmFS{instance: fsInstance, scratch: uint32(fsInstance.call("fs_scratch"))}
	report("rootfs %d bytes sha256 %x", len(rootfsWasm), sha256.Sum256(rootfsWasm))

	if !bytes.Equal(fs.readFile("/bin/rootfs"), rootfsWasm) {
		throwFmt("/bin/rootfs is not the rootfs the kernel started")
	}

	compiler := &WasmCompiler{instance: instantiate(runtime, "aot", fs.readFile("/bin/aot"))}

	for _, path := range []string{"/bin/rootfs", "/bin/aot"} {
		wasm := fs.readFile(path)
		start := time.Now()
		code := compiler.compile(wasm)
		report("compiled %s: %d bytes to %d bytes sha256 %x in %s", path, len(wasm), len(code), sha256.Sum256(code), time.Since(start).Round(time.Millisecond))
	}

	wireBoard = WireBoard{compiler: compiler, fs: fs}
}

func main() {
	try(func() {
		bootstrap()
		report("wireboard ready")
	}).catch(func(e *Exception) {
		fmt.Fprintf(os.Stderr, "kernel: %v\n", e)
		os.Exit(1)
	})
}
