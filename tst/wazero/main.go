// Runs a WASI command under wazero and reports how long wazero took to
// compile it and how long it ran:
//
//	wazero [-interpreter] [-compile] [-reserve] module.wasm [args...]
//
// The command sees the host's root directory, environment and working
// directory, as under `wasm2o run`. -interpreter runs the module
// in wazero's interpreter instead of compiling it; -compile only compiles
// it, so the module may import anything; -reserve gives memory its maximum
// capacity up front, so growth never copies it. The last line on stderr is
// "wazero: compile <seconds> run <seconds>"; the exit code is the command's.
package main

import (
	"context"
	"crypto/rand"
	"errors"
	"fmt"
	"os"
	"strings"
	"time"

	"github.com/tetratelabs/wazero"
	"github.com/tetratelabs/wazero/imports/wasi_snapshot_preview1"
	"github.com/tetratelabs/wazero/sys"
)

func usage() {
	throwFmt("usage: wazero [-interpreter] [-compile] [-reserve] module.wasm [args...]")
}

func report(compile time.Duration, run time.Duration) {
	fmt.Fprintf(os.Stderr, "wazero: compile %.3f run %.3f\n", compile.Seconds(), run.Seconds())
}

// The command's exit code: proc_exit ends a WASI command with an ExitError.
func exitCode(err error) int {
	var exit *sys.ExitError

	if errors.As(err, &exit) {
		return int(exit.ExitCode())
	}

	throw(err)

	return 0
}

func moduleConfig(args []string) wazero.ModuleConfig {
	config := wazero.NewModuleConfig().
		WithArgs(args...).
		WithStdin(os.Stdin).
		WithStdout(os.Stdout).
		WithStderr(os.Stderr).
		WithFSConfig(wazero.NewFSConfig().WithDirMount("/", "/")).
		WithSysNanotime().
		WithSysWalltime().
		WithSysNanosleep().
		WithRandSource(rand.Reader).
		WithEnv("PWD", throw2(os.Getwd()))

	for _, entry := range os.Environ() {
		name, value, _ := strings.Cut(entry, "=")

		if name != "PWD" {
			config = config.WithEnv(name, value)
		}
	}

	return config
}

func run(args []string) int {
	interpreter, compileOnly, reserve := false, false, false

	for len(args) > 0 && strings.HasPrefix(args[0], "-") {
		switch args[0] {
		case "-interpreter":
			interpreter = true
		case "-compile":
			compileOnly = true
		case "-reserve":
			reserve = true
		default:
			usage()
		}

		args = args[1:]
	}

	if len(args) == 0 {
		usage()
	}

	ctx := context.Background()
	config := wazero.NewRuntimeConfigCompiler()

	if interpreter {
		config = wazero.NewRuntimeConfigInterpreter()
	}

	runtime := wazero.NewRuntimeWithConfig(ctx, config.WithMemoryCapacityFromMax(reserve))
	wasm := throw2(os.ReadFile(args[0]))
	start := time.Now()
	compiled := throw2(runtime.CompileModule(ctx, wasm))
	compile := time.Since(start)

	if compileOnly {
		report(compile, 0)

		return 0
	}

	wasi_snapshot_preview1.MustInstantiate(ctx, runtime)
	start = time.Now()
	_, err := runtime.InstantiateModule(ctx, compiled, moduleConfig(args))
	elapsed := time.Since(start)
	code := 0

	if err != nil {
		code = exitCode(err)
	}

	report(compile, elapsed)

	return code
}

func main() {
	code := 0

	if e := try(func() {
		code = run(os.Args[1:])
	}); e != nil {
		fmt.Fprintln(os.Stderr, "wazero:", e)
		os.Exit(1)
	}

	os.Exit(code)
}
