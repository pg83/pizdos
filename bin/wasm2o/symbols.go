package main

import (
	"fmt"
	"strconv"
	"strings"
)

// Symbols of an object. A function's import or export carries its wasm
// type in its name, so an ordinary linker can match them and a loader needs
// nothing else:
//
//	w2o.import.<module>.<name>.<params>.<results>  a Go function value the code calls
//	w2o.export.<name>.<params>.<results>           a thunk Go calls
//	w2o.init                                       the thunk that starts an instance
//
// Names are spelled by symbolPart, types by a letter each: i i32, I i64,
// f f32, F f64, v v128, r funcref, e externref. The code also calls Go's
// runtime.morestack_noctxt and the Go function values w2o.trap,
// w2o.memory_grow and w2o.table_grow, through the GOT.
const (
	symInit       = "w2o.init"
	symImport     = "w2o.import."
	symExport     = "w2o.export."
	symMorestack  = "runtime.morestack_noctxt"
	symTrap       = "w2o.trap"
	symMemoryGrow = "w2o.memory_grow"
	symTableGrow  = "w2o.table_grow"
)

var typeLetters = map[ValType]byte{I32: 'i', I64: 'I', F32: 'f', F64: 'F', V128: 'v', FuncRef: 'r', ExternRef: 'e'}

func typesPart(ts []ValType) string {
	b := make([]byte, len(ts))

	for i, t := range ts {
		c, ok := typeLetters[t]

		if !ok {
			throwFmt("unsupported: a %s at the boundary", typeName(t))
		}

		b[i] = c
	}

	return string(b)
}

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

// symbolPart spells a wasm name in a symbol: letters and digits as they
// are, every other byte, underscores too, as _XX. The spelling is one to
// one and leaves "." to join parts.
func symbolPart(s string) string {
	b := make([]byte, 0, len(s))

	for i := 0; i < len(s); i++ {
		c := s[i]

		switch {
		case c >= 'a' && c <= 'z', c >= 'A' && c <= 'Z', c >= '0' && c <= '9':
			b = append(b, c)
		default:
			b = append(b, fmt.Sprintf("_%02X", c)...)
		}
	}

	return string(b)
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

// displayName keeps a name readable in tracebacks: only control bytes,
// which a symbol table cannot hold or show, become _XX.
func displayName(s string) string {
	b := make([]byte, 0, len(s))

	for i := 0; i < len(s); i++ {
		if c := s[i]; c < 0x20 || c == 0x7f {
			b = append(b, fmt.Sprintf("_%02X", c)...)
		} else {
			b = append(b, c)
		}
	}

	return string(b)
}

func importSymbol(module, name string, t *FuncType) string {
	return symImport + symbolPart(module) + "." + symbolPart(name) + "." + typesPart(t.params) + "." + typesPart(t.results)
}

func exportSymbol(name string, t *FuncType) string {
	return symExport + symbolPart(name) + "." + typesPart(t.params) + "." + typesPart(t.results)
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
