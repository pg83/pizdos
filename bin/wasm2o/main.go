package main

import (
	"fmt"
	"os"
	"runtime/pprof"
	"time"
)

func usage() {
	throwFmt("usage: wasm2o [-v] -o output.o input.wasm\n       wasm2o run [--root dir] [-a argv0] object.o [-- arg...]\n       wasm2o spec test.json")
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

// runCommand runs object.o, a program of the kernel inside the root
// directory, by default /, or a WASI command. Its arguments are argv0, by
// default the object's path, and those after --.
func runCommand(args []string) int {
	root, argv0 := "/", ""

	for len(args) > 1 && (args[0] == "--root" || args[0] == "-a") {
		if args[0] == "--root" {
			root = args[1]
		} else {
			argv0 = args[1]
		}

		args = args[2:]
	}

	if len(args) == 0 || len(args) > 1 && args[1] != "--" {
		usage()
	}

	if argv0 == "" {
		argv0 = args[0]
	}

	argv := []string{argv0}

	if len(args) > 1 {
		argv = append(argv, args[2:]...)
	}

	return runObject(args[0], root, argv)
}
