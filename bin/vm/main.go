//go:build linux && amd64

package main

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime/pprof"
	"time"
)

func usage() {
	throwFmt("usage: vm [-v] [--asan] -o output.o input.wasm\n       vm run [--root dir] [-a argv0] [--asan] -- object.o [arg...]\n       vm spec test.json\n       vm stand [--repeat N] im|pdf|djvu|svg|magic object.o file...")
}

func main() {
	code := 0

	if e := try(func() {
		setupVmChaos()
		args := os.Args[1:]

		if len(args) > 0 && args[0] == "run" {
			code = runCommand(args[1:])

			return
		}

		if len(args) > 0 && args[0] == "spec" {
			code = specCommand(args[1:])

			return
		}

		if len(args) > 0 && args[0] == "stand" {
			code = standCommand(args[1:])

			return
		}

		compileCommand(args)
	}); e != nil {
		fmt.Fprintln(os.Stderr, "vm:", e)
		os.Exit(1)
	}

	os.Exit(code)
}

func compileCommand(args []string) {
	var input, output string
	verbose, asan := false, false

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
		case "--asan":
			asan = true
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

	source := throw2(os.Open(input))

	defer source.Close()

	destination := throw2(os.CreateTemp(filepath.Dir(output), ".vm-*"))

	defer destination.Close()
	defer os.Remove(destination.Name())

	if path := os.Getenv("W2X_PROFILE"); path != "" {
		f := throw2(os.Create(path))
		throw(pprof.StartCPUProfile(f))

		defer pprof.StopCPUProfile()
	}

	options := compileOptions()

	if asan {
		options = CompileOptions{asan: true}
	}

	start := time.Now()
	compileStreams(source, destination, options)
	elapsed := time.Since(start)
	throw(destination.Chmod(0o644))
	throw(destination.Close())
	throw(os.Rename(destination.Name(), output))

	if verbose {
		fmt.Fprintf(os.Stderr, "vm: %d bytes -> %d bytes in %s\n", throw2(source.Stat()).Size(), throw2(os.Stat(output)).Size(), elapsed)
	}
}

// runCommand runs object.o, a program of the kernel, or a WASI command:
// the options come first, then -- and the object with its arguments. The
// program's root is the host directory of --root, or nothing without it:
// a server of its own files, or a program that needs none, runs so. argv0
// is by default the object's path. --asan runs an object compiled with
// --asan under the kernel's address sanitizer (kernel_asan.go).
func runCommand(args []string) int {
	root, argv0, asan := "", "", false

	for len(args) > 0 && args[0] != "--" {
		switch {
		case args[0] == "--asan":
			asan = true
			args = args[1:]
		case len(args) > 1 && args[0] == "--root":
			root = args[1]
			args = args[2:]
		case len(args) > 1 && args[0] == "-a":
			argv0 = args[1]
			args = args[2:]
		default:
			usage()
		}
	}

	if len(args) < 2 {
		usage()
	}

	args = args[1:]

	if argv0 == "" {
		argv0 = args[0]
	}

	argv := append([]string{argv0}, args[1:]...)

	return runObject(args[0], root, argv, asan)
}
