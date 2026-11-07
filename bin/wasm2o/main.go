package main

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime/pprof"
	"strings"
	"time"
)

func usage() {
	throwFmt("usage: wasm2o [-p prefix] [-i import-prefix] [-H header.h] -o output.o input.wasm")
}

func main() {
	defer func() {
		if e := recover(); e != nil {
			if _, ok := e.(*Exception); !ok {
				panic(e)
			}

			fmt.Fprintln(os.Stderr, "wasm2o:", e)
			os.Exit(1)
		}
	}()

	var input, output, header, prefix, imports string
	verbose := false
	args := os.Args[1:]

	for len(args) > 0 {
		arg := args[0]
		args = args[1:]

		switch arg {
		case "-o", "-H", "-p", "-i":
			if len(args) == 0 {
				usage()
			}

			switch arg {
			case "-o":
				output = args[0]
			case "-H":
				header = args[0]
			case "-i":
				imports = args[0]
			default:
				prefix = args[0]
			}

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

	if prefix == "" {
		prefix = sanitize(strings.TrimSuffix(filepath.Base(input), filepath.Ext(input)))
	}

	data := throw2(os.ReadFile(input))

	if path := os.Getenv("W2X_PROFILE"); path != "" {
		f := throw2(os.Create(path))
		throw(pprof.StartCPUProfile(f))

		defer pprof.StopCPUProfile()
	}

	start := time.Now()
	obj, h := compileModule(data, prefix, imports)
	elapsed := time.Since(start)
	throw(os.WriteFile(output, obj, 0o644))

	if header != "" {
		throw(os.WriteFile(header, h, 0o644))
	}

	if verbose {
		fmt.Fprintf(os.Stderr, "wasm2o: %d bytes -> %d bytes in %s\n", len(data), len(obj), elapsed)
	}
}
