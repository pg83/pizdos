package main

import (
	"strconv"
	"strings"
)

// Symbols of an object. A function's import or export carries its wasm
// type in its name, so an ordinary linker can match them and a loader needs
// nothing else:
//
//	w2o.import.<module>.<name>.<params>.<results>  a Go function the code calls
//	w2o.export.<name>.<params>.<results>           a thunk Go calls
//	w2o.init                                       the thunk that starts an instance
//
// Names are spelled by symbolPart, types by a letter each: i i32, I i64,
// f f32, F f64, v v128, r funcref, e externref. The code also calls, through
// the GOT, the Go functions w2o.trap, w2o.memory_grow and w2o.table_grow,
// the loader's stubs w2o.morestack, which stack checks call, and
// w2o.interrupt_call, which a return from an import calls and Go's runtime
// injects (stubs_amd64.s), and lib/str's functions str.memcpy,
// str.memmove and str.memset (str.go), which the bulk memory instructions
// and the imports of lib/c's memcpy, memmove and memset call with host
// addresses after checking the ranges themselves: the destination, the
// source or byte and the length in AX, CX and DX, as the module's
// functions call each other.
const (
	symInit          = "w2o.init"
	symMorestackStub = "w2o.morestack"
	symImport        = "w2o.import."
	symExport        = "w2o.export."
	symTrap          = "w2o.trap"
	symMemoryGrow    = "w2o.memory_grow"
	symTableGrow     = "w2o.table_grow"
	symInterruptCall = "w2o.interrupt_call"
	symStr           = "str."
)

var typeLetters = map[ValType]byte{I32: 'i', I64: 'I', F32: 'f', F64: 'F', V128: 'v', FuncRef: 'r', ExternRef: 'e'}

func parseTypes(s string) ([]ValType, bool) {
	ts := make([]ValType, len(s))

	for i := range s {
		found := false

		for t, c := range typeLetters {
			if c == s[i] {
				ts[i], found = t, true
			}
		}

		if !found {
			return nil, false
		}
	}

	return ts, true
}

func parseSymbolPart(s string) (string, bool) {
	b := make([]byte, 0, len(s))

	for i := 0; i < len(s); i++ {
		if s[i] != '_' {
			b = append(b, s[i])

			continue
		}

		if i+3 > len(s) {
			return "", false
		}

		x, err := strconv.ParseUint(s[i+1:i+3], 16, 8)

		if err != nil {
			return "", false
		}

		b = append(b, byte(x))
		i += 2
	}

	return string(b), true
}

// parseFuncSymbol takes an import or export symbol apart: its names, then
// its type; ok is false for another symbol.
func parseFuncSymbol(sym, prefix string, names int) ([]string, *FuncType, bool) {
	if !strings.HasPrefix(sym, prefix) {
		return nil, nil, false
	}

	parts := strings.Split(sym[len(prefix):], ".")

	if len(parts) != names+2 {
		return nil, nil, false
	}

	out := make([]string, names)

	for i := range out {
		name, ok := parseSymbolPart(parts[i])

		if !ok {
			return nil, nil, false
		}

		out[i] = name
	}

	params, ok1 := parseTypes(parts[names])
	results, ok2 := parseTypes(parts[names+1])

	return out, &FuncType{params: params, results: results}, ok1 && ok2
}
