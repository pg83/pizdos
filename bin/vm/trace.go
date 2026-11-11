//go:build linux && amd64

package main

import (
	"fmt"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
)

// The trace of a crash runs through the module's code and the Go code it
// called as one stack: the loader registered the code's functions with Go
// (gort.go), so Go's unwinder walks their frames as it walks its own and
// names them as the object does, f<index>:<name>. A trace keeps the frames
// from the fault down to the module's entry: the kernel's functions above
// the module's where an import was called, then the module's own. The
// loader's thunks, adapters and stubs (w2o.*) are left out, and so is the
// harness above the module's entry.

// traceDepth is how many frames a trace captures, tracePrinted how many a
// report shows: the top and the bottom around a gap.
const (
	traceDepth   = 1024
	tracePrinted = 64
)

// loaded is the registered code (registerCode): what tells a frame of the
// module from one of Go.
var loaded struct {
	sync.Mutex
	ranges []codeRange
}

type codeRange struct {
	base, end uintptr
	name      string
}

// noteCode records registered code for the traces.
func noteCode(name string, base, end uintptr) {
	loaded.Lock()
	loaded.ranges = append(loaded.ranges, codeRange{base, end, name})
	loaded.Unlock()
}

// codeAt is the registered code pc is in, if any.
func codeAt(pc uintptr) (codeRange, bool) {
	loaded.Lock()
	defer loaded.Unlock()

	for _, r := range loaded.ranges {
		if pc >= r.base && pc < r.end {
			return r, true
		}
	}

	return codeRange{}, false
}

// callers captures the stack from the caller's caller up, less skip frames
// above it.
func callers(skip int) []uintptr {
	pcs := make([]uintptr, traceDepth)

	return pcs[:runtime.Callers(skip+2, pcs)]
}

// traceLines formats a captured stack, a frame a line: a function of the
// module with the offset of its instruction, a Go function with its file and
// line, aligned.
func traceLines(pcs []uintptr) []string {
	type frame struct {
		name, place string
		module      bool
	}

	var frames []frame
	it := runtime.CallersFrames(pcs)

	for {
		f, more := it.Next()

		if f.PC != 0 {
			if code, ok := codeAt(f.PC); ok {
				// A function of the module: with its source line when the
				// object has them, else its file is the object's name.
				if !strings.HasPrefix(f.Function, "w2o.") {
					fr := frame{name: fmt.Sprintf("%s+%#x", moduleFunction(f.Function), f.PC-f.Entry), module: true}

					if f.File != code.name {
						fr.place = fmt.Sprintf("%s:%d", filepath.Base(f.File), f.Line)
					}

					frames = append(frames, fr)
				}
			} else {
				frames = append(frames, frame{name: goFunction(f.Function), place: fmt.Sprintf("%s:%d", filepath.Base(f.File), f.Line)})
			}
		}

		if !more {
			break
		}
	}

	last := -1

	for i, f := range frames {
		if f.module {
			last = i
		}
	}

	if last >= 0 {
		frames = frames[:last+1]
	}

	width := 0

	for _, f := range frames {
		if f.place != "" && len(f.name) > width && len(f.name) <= 48 {
			width = len(f.name)
		}
	}

	var lines []string
	emit := func(f frame) {
		if f.place == "" {
			lines = append(lines, f.name)
		} else {
			lines = append(lines, fmt.Sprintf("%-*s  %s", width, f.name, f.place))
		}
	}

	if len(frames) <= tracePrinted {
		for _, f := range frames {
			emit(f)
		}

		return lines
	}

	head, tail := tracePrinted/2, tracePrinted/2-1

	for _, f := range frames[:head] {
		emit(f)
	}

	lines = append(lines, fmt.Sprintf("... %d frames", len(frames)-head-tail))

	for _, f := range frames[len(frames)-tail:] {
		emit(f)
	}

	return lines
}

// goFunction is a Go function's name without its package: the kernel's
// functions are all of one.
func goFunction(name string) string {
	start := strings.LastIndexByte(name, '/') + 1

	if dot := strings.IndexByte(name[start:], '.'); dot >= 0 {
		return name[start+dot+1:]
	}

	return name
}

// moduleFunction is the name of a function of the module from the name the
// object registered: f<index>:<name>, or f<index> alone for a function the
// module did not name.
func moduleFunction(registered string) string {
	if _, name, ok := strings.Cut(registered, ":"); ok {
		return name
	}

	return registered
}
