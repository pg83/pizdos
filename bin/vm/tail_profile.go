//go:build linux && amd64

package main

import (
	"encoding/binary"
	"encoding/json"
	"fmt"
	"maps"
	"os"
	"slices"
	"strings"
)

// The tail profile of an object compiled with its counters (W2X_TAIL_PROFILE)
// goes, when an instance is released, to the file W2O_TAIL_PROFILE names: a
// line of JSON for the instance, with each function that counted anything.
// A function has its totals and the sites that counted, in the order of
// their offsets: a site's kind, the offset in the module of the wasm
// instruction it is of (0 for none) and its count. A check counts its
// executions; a guard of a checked span its own, a fallback the guard's
// failures and omitted the checks the guard stood for; loop a loop's
// iterations and entry its entries, and fast_loop and fast_entry those of a
// copy of the loop whose guard proved its accesses, which its entries took.

type TailProfileSite struct {
	Kind   string `json:"kind"`
	Offset uint32 `json:"offset"`
	Count  uint64 `json:"count"`
}

type TailProfileRow struct {
	Function  int               `json:"function"`
	Name      string            `json:"name"`
	Guards    uint64            `json:"guards"`
	Fallbacks uint64            `json:"fallbacks"`
	Omitted   uint64            `json:"omitted_checks"`
	Checks    uint64            `json:"ordinary_checks"`
	Sites     []TailProfileSite `json:"sites"`
}

type TailProfile struct {
	Module    string           `json:"module"`
	Functions []TailProfileRow `json:"functions"`
}

func (i *Instance) tailProfile() TailProfile {
	out := TailProfile{Module: i.name}
	meta := i.obj.meta.memory

	names := make(map[int]string)

	for _, region := range i.obj.meta.regions {
		var fn int

		if strings.HasPrefix(region.name, "f") {
			if _, err := fmt.Sscanf(region.name, "f%d", &fn); err == nil {
				names[fn] = region.name
			}
		}
	}

	rows := make(map[int]*TailProfileRow)

	for k, site := range meta.sites {
		p := int(meta.profile) + k*profileCounterBytes
		count := binary.LittleEndian.Uint64(i.block[p : p+profileCounterBytes])

		if count == 0 {
			continue
		}

		fn := int(site.function)
		r := rows[fn]

		if r == nil {
			r = &TailProfileRow{Function: fn, Name: names[fn]}
			rows[fn] = r
		}

		switch site.kind {
		case siteCheck:
			r.Checks += count
		case siteGuard:
			r.Guards += count
		case siteFallback:
			r.Fallbacks += count
		case siteOmitted:
			r.Omitted += count
		}

		r.Sites = append(r.Sites, TailProfileSite{Kind: siteKinds[site.kind], Offset: site.offset, Count: count})
	}

	for _, fn := range slices.Sorted(maps.Keys(rows)) {
		r := rows[fn]

		slices.SortStableFunc(r.Sites, func(a, b TailProfileSite) int {
			return int(a.Offset) - int(b.Offset)
		})

		out.Functions = append(out.Functions, *r)
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
