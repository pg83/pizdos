//go:build pizdoscoverage

package main

import (
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sync"
)

// Staged into the VM package by the coverage targets only. The metadata is
// immutable; one cumulative counter file per process avoids retaining a copy
// of the function records for every compilation in the spec suite.
type CompilerProfile struct {
	path   string
	counts []byte
}

var compilerProfiles = struct {
	sync.Mutex
	profiles map[string]*CompilerProfile
}{profiles: make(map[string]*CompilerProfile)}

func compilerCoverage(i *Instance) {
	directory := os.Getenv("PIZDOS_WASM_COVERDIR")
	_, compiler := i.host.(*CompilerIO)

	if directory == "" || !compiler {
		return
	}

	entry, _ := i.export("__coverage_info")
	info := entry.Interface().(func(uintptr, uint32) uint32)
	var layout [11]uint32

	for k := range layout {
		layout[k] = info(i.ctx(), uint32(k))
	}

	memory := i.memoryBytes()
	sections := make([][]byte, 3)

	for k := range sections {
		start, end := layout[k*2], layout[k*2+1]

		if start > end || uint64(end) > uint64(len(memory)) {
			throwFmt("invalid Wasm coverage section %d: %d..%d", k, start, end)
		}

		sections[k] = memory[start:end]
	}

	metadata := throw2(json.Marshal(struct {
		Layout [11]uint32 `json:"layout"`
		Data   []byte     `json:"data"`
		Names  []byte     `json:"names"`
	}{layout, sections[0], sections[2]}))
	hash := sha256.Sum256(metadata)
	stem := hex.EncodeToString(hash[:])
	key := filepath.Join(directory, stem)
	compilerProfiles.Lock()

	defer compilerProfiles.Unlock()

	profile := compilerProfiles.profiles[key]

	if profile == nil {
		throw(os.WriteFile(key+".json", metadata, 0o600))
		file := throw2(os.CreateTemp(directory, fmt.Sprintf("%s-%d-*.counts", stem, os.Getpid())))
		throw(file.Close())
		profile = &CompilerProfile{path: file.Name(), counts: make([]byte, len(sections[1]))}
		compilerProfiles.profiles[key] = profile
	}

	if len(sections[1])%8 != 0 || len(profile.counts) != len(sections[1]) {
		throwFmt("invalid Wasm coverage counters")
	}

	for offset := 0; offset < len(profile.counts); offset += 8 {
		count := binary.LittleEndian.Uint64(profile.counts[offset:])
		count += binary.LittleEndian.Uint64(sections[1][offset:])
		binary.LittleEndian.PutUint64(profile.counts[offset:], count)
	}

	throw(os.WriteFile(profile.path, profile.counts, 0o600))
}
