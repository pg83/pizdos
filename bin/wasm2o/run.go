//go:build linux && amd64

package main

import (
	"fmt"
	"os"
	"runtime"
)

// runObject runs an object as a WASI command on this goroutine and returns
// its exit code: proc_exit's, or 128 and the trap code after a trap.
func runObject(data []byte, argv []string) (code int) {
	obj := readObject(data)
	i := instantiate(argv[0], obj, newWASI(argv))

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
