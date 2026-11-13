//go:build linux && amd64

package main

import (
	"bufio"
	"encoding/binary"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"unsafe"
)

// With W2O_PERFMAP set, the code the kernel links is named for perf, in
// the directory W2O_PERFMAP names when it is absolute, else in the
// temporary directory, TMPDIR's, in two files. perf-<pid>.map is the perf
// map, which perf reads from /tmp alone. jit-<pid>.dump is a jitdump,
// which perf finds wherever the recording saw it mapped:
//
//	perf record -k mono -g -- vm run ...
//	perf inject --jit -i perf.data -o perf.jit.data
//	perf report -i perf.jit.data
//
// inject makes an ELF file of every function beside the dump, with its
// code and, for a module with a DWARF line table, its source lines; then
// report and annotate name all code: the vm's own by its symbols, a
// module's as module:function. Both files are best effort: a directory
// that is not there gets none.

var perfOut = newPerfOutput(os.Getenv("W2O_PERFMAP"))

// perfOutput writes the names of linked code for perf.
type perfOutput struct {
	mu     sync.Mutex
	dir    string
	jit    *os.File
	failed bool
	index  uint64
}

func newPerfOutput(setting string) *perfOutput {
	if setting == "" {
		return nil
	}

	dir := os.TempDir()

	if strings.HasPrefix(setting, "/") {
		dir = setting
	}

	return &perfOutput{dir: dir}
}

// The jitdump's format, perf's tools/perf/util/jitdump.h: a header, then
// records, each its kind, its size and when it was written, on the
// monotonic clock a recording with -k mono uses.
const (
	jitMagic     = 0x4a695444
	jitHeader    = 40
	jitCodeLoad  = 0
	jitDebugInfo = 2
	emX86_64     = 62
)

func monotonic() uint64 {
	var ts syscall.Timespec

	syscall.Syscall(syscall.SYS_CLOCK_GETTIME, 1, uintptr(unsafe.Pointer(&ts)), 0)

	return uint64(ts.Sec)*1e9 + uint64(ts.Nsec)
}

// jitdump is the dump, made with its header and mapped executable once,
// for perf record to see where it is; nil when it cannot be.
func (o *perfOutput) jitdump() *os.File {
	if o.jit != nil || o.failed {
		return o.jit
	}

	o.failed = true
	f, err := os.OpenFile(filepath.Join(o.dir, fmt.Sprintf("jit-%d.dump", os.Getpid())), os.O_RDWR|os.O_CREATE|os.O_TRUNC, 0o644)

	if err != nil {
		return nil
	}

	header := make([]byte, jitHeader)
	le := binary.LittleEndian
	le.PutUint32(header, jitMagic)
	le.PutUint32(header[4:], 1)
	le.PutUint32(header[8:], jitHeader)
	le.PutUint32(header[12:], emX86_64)
	le.PutUint32(header[20:], uint32(os.Getpid()))
	le.PutUint64(header[24:], monotonic())

	if _, err := f.Write(header); err != nil {
		f.Close()

		return nil
	}

	// The mapping stays: perf record notes it, and inject reads the file
	// it names.
	if _, err := syscall.Mmap(int(f.Fd()), 0, jitHeader, syscall.PROT_READ|syscall.PROT_EXEC, syscall.MAP_PRIVATE); err != nil {
		f.Close()

		return nil
	}

	o.jit, o.failed = f, false

	return f
}

// perfName is a region's name for perf: the module's name, its file's
// without the extension, and the region's.
func perfName(module, region string) string {
	return strings.TrimSuffix(filepath.Base(module), ".wasm") + ":" + region
}

// code names the regions of a module linked at base: in the map, and in
// the dump with their code and source lines.
func (o *perfOutput) code(module string, base uintptr, regions []Region, files []string, lines []lineEntry) {
	if o == nil {
		return
	}

	o.mu.Lock()
	defer o.mu.Unlock()

	if f, err := os.OpenFile(filepath.Join(o.dir, fmt.Sprintf("perf-%d.map", os.Getpid())), os.O_WRONLY|os.O_CREATE|os.O_APPEND, 0o644); err == nil {
		w := bufio.NewWriter(f)

		for _, r := range regions {
			fmt.Fprintf(w, "%x %x %s\n", base+uintptr(r.start), r.end-r.start, perfName(module, r.name))
		}

		w.Flush()
		f.Close()
	}

	jit := o.jitdump()

	if jit == nil {
		return
	}

	w := bufio.NewWriter(jit)
	le := binary.LittleEndian
	pid, tid := uint32(os.Getpid()), uint32(syscall.Gettid())
	at := 0

	for _, r := range regions {
		addr := uint64(base) + uint64(r.start)

		// The source lines of the region, before its code.
		for at < len(lines) && lines[at].pc < r.start {
			at++
		}

		var entries []byte
		count := 0

		for k := at; k < len(lines) && lines[k].pc < r.end; k++ {
			entry := make([]byte, 16)
			le.PutUint64(entry, uint64(base)+uint64(lines[k].pc))
			le.PutUint32(entry[8:], lines[k].line)
			entries = append(append(append(entries, entry...), files[lines[k].file]...), 0)
			count++
		}

		if count > 0 {
			record := make([]byte, 32, 32+len(entries))
			le.PutUint32(record, jitDebugInfo)
			le.PutUint32(record[4:], uint32(32+len(entries)))
			le.PutUint64(record[8:], monotonic())
			le.PutUint64(record[16:], addr)
			le.PutUint64(record[24:], uint64(count))
			w.Write(append(record, entries...))
		}

		name := perfName(module, r.name)
		size := uint64(r.end - r.start)
		record := make([]byte, 56)
		le.PutUint32(record, jitCodeLoad)
		le.PutUint32(record[4:], uint32(56+len(name)+1)+uint32(size))
		le.PutUint64(record[8:], monotonic())
		le.PutUint32(record[16:], pid)
		le.PutUint32(record[20:], tid)
		le.PutUint64(record[24:], addr)
		le.PutUint64(record[32:], addr)
		le.PutUint64(record[40:], size)
		le.PutUint64(record[48:], o.index)
		o.index++
		w.Write(record)
		w.WriteString(name)
		w.WriteByte(0)
		w.Write(unsafe.Slice((*byte)(unsafe.Pointer(uintptr(addr))), size))
	}

	w.Flush()
}
