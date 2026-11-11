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
	throwFmt("usage: vm [-v] -o output.o input.wasm\n       vm run [--root dir] [-a argv0] object.o [-- arg...]\n       vm spec test.json\n       vm stand [--repeat N] im|pdf|djvu|svg|magic object.o file...")
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

	start := time.Now()
	compileStreams(source, destination, compileOptions())
	elapsed := time.Since(start)
	throw(destination.Chmod(0o644))
	throw(destination.Close())
	throw(os.Rename(destination.Name(), output))

	if verbose {
		fmt.Fprintf(os.Stderr, "vm: %d bytes -> %d bytes in %s\n", throw2(source.Stat()).Size(), throw2(os.Stat(output)).Size(), elapsed)
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
