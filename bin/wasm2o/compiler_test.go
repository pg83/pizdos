//go:build linux && amd64

package main

import (
	"bytes"
	"io"
	"strings"
	"sync"
	"testing"
	"testing/iotest"
)

// Shared linked code must keep each invocation's WASI streams and memory
// separate, including reads that stop in the middle of a wasm integer.
func TestCompilerConcurrentStreams(t *testing.T) {
	var work sync.WaitGroup

	for value := int32(0); value < 8; value++ {
		work.Add(1)

		go func() {
			defer work.Done()

			if e := try(func() {
				m := &testModule{funcs: []testFunc{{results: i32, export: "value", code: i32c(value + 1234)}}}
				data := m.encode()
				var out bytes.Buffer
				compileStreams(iotest.OneByteReader(bytes.NewReader(data)), &out, CompileOptions{})
				i := instantiate(t.Name(), readObject(out.Bytes()), nil, runStack)

				defer i.release()

				i.start()
				entry, _ := i.export("value")

				if got := entry.Interface().(func(uintptr) uint32)(i.ctx()); got != uint32(value+1234) {
					t.Errorf("concurrent compile: got %d, want %d", got, value+1234)
				}
			}); e != nil {
				t.Error(e)
			}
		}()
	}

	work.Wait()
}

type CompilerFailedWriter struct{}

func (CompilerFailedWriter) Write([]byte) (int, error) {
	return 0, io.ErrClosedPipe
}

func TestCompilerFailureRecovery(t *testing.T) {
	valid := (&testModule{}).encode()

	for _, data := range [][]byte{nil, []byte("not wasm"), {0, 'a', 's', 'm', 1, 0, 0, 0, 1, 1, 128}} {
		if e := try(func() { compileModule(data) }); e == nil || !strings.Contains(e.message(), "aot exited") {
			t.Fatalf("malformed input: %v", e)
		}
	}

	if e := try(func() {
		compileStreams(bytes.NewReader(valid), CompilerFailedWriter{}, CompileOptions{})
	}); e == nil || !strings.Contains(e.message(), io.ErrClosedPipe.Error()) {
		t.Fatalf("output error: %v", e)
	}

	if obj := readObject(compileModule(valid)); obj.meta.memory != nil {
		t.Fatal("valid compilation after failure changed the module")
	}
}
