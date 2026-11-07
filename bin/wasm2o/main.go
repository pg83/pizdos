package main

import (
	"fmt"
	"os"
	"runtime/pprof"
	"time"
)

func usage() {
	throwFmt("usage: wasm2o [-v] -o output.o input.wasm\n       wasm2o run object.o [-- arg...]\n       wasm2o spec test.json")
}

func main() {
	code := 0

	if e := try(func() {
		args := os.Args[1:]

		if len(args) > 0 && args[0] == "run" {
			code = runCommand(args[1:])

			return
		}

		if len(args) > 0 && args[0] == "spec" {
			code = specCommand(args[1:])

			return
		}

		compileCommand(args)
	}); e != nil {
		fmt.Fprintln(os.Stderr, "wasm2o:", e)
		os.Exit(1)
	}

	os.Exit(code)
}

func compileCommand(args []string) {
	var input, output string
	verbose := false

	for len(args) > 0 {
		arg := args[0]
		args = args[1:]

		switch arg {
		case "-o":
			if len(args) == 0 {
				usage()
			}

			output = args[0]
			args = args[1:]
		case "-v":
			verbose = true
		default:
			if input != "" {
				usage()
			}

			input = arg
		}
	}

	if input == "" || output == "" {
		usage()
	}

	data := throw2(os.ReadFile(input))

	if path := os.Getenv("W2X_PROFILE"); path != "" {
		f := throw2(os.Create(path))
		throw(pprof.StartCPUProfile(f))

		defer pprof.StopCPUProfile()
	}

	start := time.Now()
	obj := compileModule(data)
	elapsed := time.Since(start)
	throw(os.WriteFile(output, obj, 0o644))

	if verbose {
		fmt.Fprintf(os.Stderr, "wasm2o: %d bytes -> %d bytes in %s\n", len(data), len(obj), elapsed)
	}
}

// runCommand runs object.o as a WASI command: its arguments are the object's
// path and those after --.
func runCommand(args []string) int {
	if len(args) == 0 || len(args) > 1 && args[1] != "--" {
		usage()
	}

	argv := []string{args[0]}

	if len(args) > 1 {
		argv = append(argv, args[2:]...)
	}

	return runObject(throw2(os.ReadFile(args[0])), argv)
}
