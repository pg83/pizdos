//go:build linux && amd64

package main

import (
	"fmt"
	"os"
	"runtime"
	"syscall"
)

// runModule compiles a module and runs it, and returns the exit code of
// vm: a program of the kernel as its first process, inside root, or a WASI
// command on this goroutine, whose code is proc_exit's, or 128 and the
// trap code after a trap. A module is all vm runs: no object of vm's.
func runModule(path, root string, argv []string, asan bool) (code int) {
	f := throw2(openHostFile(path))
	data, err := moduleData(nil, f)

	if err != nil {
		throwFmt("%s: %v", path, err)
	}

	options := programOptions(asan)
	obj, err := compileModule(data, options)

	if err == syscall.ENOEXEC {
		throwFmt("%s: not a WebAssembly module", path)
	}

	if err != nil {
		throwFmt("%s: %v", path, err)
	}

	if importsKernel(obj) {
		return runKernel(f, path, root, argv, options, data, obj)
	}

	f.unref()

	if asan {
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
			fmt.Fprintln(os.Stderr, e.Error())

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
