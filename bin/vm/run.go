//go:build linux && amd64

package main

import (
	"fmt"
	"os"
	"runtime"
)

// runObject runs an object and returns the exit code of vm: a program
// of the kernel as its first process, inside root, or a WASI command on
// this goroutine, whose code is proc_exit's, or 128 and the trap code after
// a trap.
func runObject(path, root string, argv []string, asan bool) (code int) {
	obj := readObject(throw2(os.ReadFile(path)))

	if importsKernel(obj) {
		return runKernel(path, root, argv, asan)
	}

	if asan || obj.meta.asan() {
		throwFmt("--asan runs programs of the kernel")
	}

	w := newWASI(argv)
	i := instantiate(argv[0], obj, w, runStack)
	i.attach(w)

	defer i.writeTailProfile()

	defer func() {
		switch e := recover().(type) {
		case nil:
		case *wasiExit:
			code = int(e.code)
		case *Trap:
			fmt.Fprintln(os.Stderr, e.message())

			for _, line := range traceLines(e.stack) {
				fmt.Fprintln(os.Stderr, "    "+line)
			}

			code = 128 + int(e.code)
		default:
			panic(e)
		}

		runtime.KeepAlive(i)
	}()

	i.start()
	i.startCommand()

	return 0
}
