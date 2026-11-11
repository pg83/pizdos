//go:build linux && amd64

package main

import "testing"

// An import has an adapter, code a pointer can reach, only when the module
// names it, by an export here: one that is only called has none, and both
// still run.
func TestImportAdapters(t *testing.T) {
	m := &testModule{
		imports:       []testImport{{"env", "called", i32, i32}, {"env", "named", i32, i32}},
		funcs:         []testFunc{{params: i32, results: i32, export: "call", code: cat(get(0), call(0))}},
		importExports: map[int]string{1: "named"},
	}
	obj := readObject(compileModuleWithOptions(m.encode(), CompileOptions{}))

	if _, ok := obj.defined["w2o.adapter.env.called"]; ok {
		t.Fatal("an import that is only called has an adapter")
	}

	if _, ok := obj.defined["w2o.adapter.env.named"]; !ok {
		t.Fatal("an exported import has no adapter")
	}

	imports := testImports{"env.called": func(_ uintptr, x uint32) uint32 { return 2 * x }, "env.named": func(_ uintptr, x uint32) uint32 { return 3 * x }}
	i := instantiate(t.Name(), obj, imports, 8<<20)
	i.start()
	defer i.release()

	for name, want := range map[string]uint32{"call": 14, "named": 21} {
		f, _ := i.export(name)

		if got := f.Interface().(func(uintptr, uint32) uint32)(i.ctx(), 7); got != want {
			t.Fatalf("%s(7) is %d, want %d", name, got, want)
		}
	}
}
