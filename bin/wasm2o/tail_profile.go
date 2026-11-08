//go:build linux && amd64

package main

import (
	"encoding/binary"
	"encoding/json"
	"fmt"
	"os"
	"strings"
)

type TailProfileRow struct {
	Function  int    `json:"function"`
	Name      string `json:"name"`
	Guards    uint64 `json:"guards"`
	Fallbacks uint64 `json:"fallbacks"`
	Omitted   uint64 `json:"omitted_checks"`
	Checks    uint64 `json:"ordinary_checks"`
}

type TailProfile struct {
	Module    string           `json:"module"`
	Functions []TailProfileRow `json:"functions"`
}

func (i *Instance) tailProfile() TailProfile {
	out := TailProfile{Module: i.name}
	meta := i.obj.meta.memory

	if meta == nil || meta.profile == 0 {
		return out
	}

	names := make(map[int]string)

	for _, region := range i.obj.meta.regions {
		var fn int

		if strings.HasPrefix(region.name, "f") {
			if _, err := fmt.Sscanf(region.name, "f%d", &fn); err == nil {
				names[fn] = region.name
			}
		}
	}

	for fn := range int(meta.profileFuncs) {
		p := int(meta.profile) + fn*tailCounterBytes
		b := i.block[p : p+tailCounterBytes]
		r := TailProfileRow{Function: fn, Name: names[fn], Guards: binary.LittleEndian.Uint64(b), Fallbacks: binary.LittleEndian.Uint64(b[8:]), Omitted: binary.LittleEndian.Uint64(b[16:]), Checks: binary.LittleEndian.Uint64(b[24:])}

		if r.Guards != 0 || r.Checks != 0 {
			out.Functions = append(out.Functions, r)
		}
	}

	return out
}

func (i *Instance) writeTailProfile() {
	meta := i.obj.meta.memory
	path := os.Getenv("W2O_TAIL_PROFILE")

	if path == "" || meta == nil || meta.profile == 0 {
		return
	}

	f := throw2(os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o644))

	defer f.Close()

	throw(json.NewEncoder(f).Encode(i.tailProfile()))
}
