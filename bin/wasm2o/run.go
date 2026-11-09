//go:build linux && amd64

package main

import (
	"fmt"
	"os"
	"runtime"
)

// runObject runs an object and returns the exit code of wasm2o: a program
// of the kernel as its first process, inside root, or a WASI command on
// this goroutine, whose code is proc_exit's, or 128 and the trap code after
// a trap.
func runObject(path, root string, argv []string) (code int) {
	obj := readObject(throw2(os.ReadFile(path)))

	if importsKernel(obj) {
		return runKernel(path, root, argv)
	}

	w := newWASI(argv)
	i := instantiate(argv[0], obj, w, runStack)
	i.host = w

	defer i.writeTailProfile()

	defer func() {
		switch e := recover().(type) {
		case nil:
		case *wasiExit:
			code = int(e.code)
		case *Trap:
			fmt.Fprintln(os.Stderr, e.message())
			code = 128 + int(e.code)
		default:
			panic(e)
		}

		runtime.KeepAlive(i)
	}()

	i.start()
	start, _ := i.export("_start")
	start.Interface().(func(uintptr))(i.ctx())

	return 0
}
