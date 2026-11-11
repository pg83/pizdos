//go:build linux && amd64

package main

import (
	"bytes"
	"math/rand"
	"strings"
	"testing"
)

// The imports lib/c's memory and string functions call, __kernel_memcpy and
// the like, are compiled at each call into lib/str's functions, the ranges
// and strings checked there: each agrees with what the kernel's function
// did, kernel below, on what it returns, whether it traps and the memory it
// leaves, for ranges and strings anywhere in the memory, at its end and past
// it, with and without the accessible tail, at every size up to the maximum
// and with its result unused. Nothing is written past the accessible end,
// and no import is bound: the code calls none.
func TestStringFunctions(t *testing.T) {
	type function struct {
		name  string
		arity int
	}

	functions := []function{{"memcpy", 3}, {"memmove", 3}, {"memset", 3}, {"memcmp", 3}, {"memchr", 3}, {"strlen", 1}, {"strcpy", 2}, {"strcat", 2}, {"strcmp", 2}, {"strspn", 2}, {"strcspn", 2}}
	m := &testModule{memMin: 1, memMax: 3}

	for k, f := range functions {
		params := make([]ValType, f.arity)
		var args []byte

		for j := range params {
			params[j] = I32
			args = append(args, get(j)...)
		}

		m.imports = append(m.imports, testImport{module: "kernel", name: "__kernel_" + f.name, params: params, results: i32})
		m.funcs = append(m.funcs, testFunc{params: params, results: i32, export: f.name, code: cat(args, call(k))},
			testFunc{params: params, export: f.name + "_drop", code: cat(args, call(k), []byte{opDrop})})
	}

	m.funcs = append(m.funcs, testFunc{params: []ValType{I32}, results: i32, export: "grow", code: cat(get(0), []byte{opMemoryGrow, 0})})

	for _, options := range []CompileOptions{{memoryTail: true, tailRanges: true}, {}} {
		i := instantiate(t.Name(), readObject(compileModuleWithOptions(m.encode(), options)), testImports{}, 8<<20)
		i.start()
		p := &Process{inst: i}

		// exported calls an export: what it returns, 0 for a _drop one,
		// and its trap.
		exported := func(name string, args []uint32) (r uint32, trap int) {
			f, _ := i.export(name)
			trap = trapOf(func() {
				switch fn := f.Interface().(type) {
				case func(uintptr, uint32) uint32:
					r = fn(i.ctx(), args[0])
				case func(uintptr, uint32, uint32) uint32:
					r = fn(i.ctx(), args[0], args[1])
				case func(uintptr, uint32, uint32, uint32) uint32:
					r = fn(i.ctx(), args[0], args[1], args[2])
				case func(uintptr, uint32):
					fn(i.ctx(), args[0])
				case func(uintptr, uint32, uint32):
					fn(i.ctx(), args[0], args[1])
				case func(uintptr, uint32, uint32, uint32):
					fn(i.ctx(), args[0], args[1], args[2])
				}
			})

			return r, trap
		}

		// kernel is what the kernel's functions did, which the code keeps:
		// a range past the memory's end, or a string not ended within it,
		// traps, as does a search that the end cut short.
		kernel := func(name string, a []uint32) (r uint32, trap int) {
			trap = trapOf(func() {
				switch name {
				case "memcpy", "memmove":
					copy(p.span(a[0], a[2]), p.span(a[1], a[2]))
					r = a[0]
				case "memset":
					d := p.span(a[0], a[2])

					for k := range d {
						d[k] = byte(a[1])
					}

					r = a[0]
				case "memcmp":
					r = uint32(int32(bytes.Compare(p.span(a[0], a[2]), p.span(a[1], a[2]))))
				case "memchr":
					r, _ = p.search(a[0], a[2], byte(a[1]))
				case "strlen":
					r = uint32(len(p.cstr(a[0])))
				case "strcpy":
					p.strcpy(a[0], a[1])
					r = a[0]
				case "strcat":
					p.strcpy(a[0]+uint32(len(p.cstr(a[0]))), a[1])
					r = a[0]
				case "strcmp":
					r = uint32(int32(bytes.Compare(p.cstr(a[0]), p.cstr(a[1]))))
				case "strspn":
					r = p.spanSet(a[0], a[1], false)
				case "strcspn":
					r = p.spanSet(a[0], a[1], true)
				}
			})

			return r, trap
		}

		// check runs the export, its _drop one and the kernel's function
		// from the same memory.
		check := func(name string, args ...uint32) {
			mem := i.memoryBytes()
			before := bytes.Clone(mem)
			want, wantTrap := kernel(name, args)
			wantMem := bytes.Clone(mem)

			for _, export := range []string{name, name + "_drop"} {
				copy(mem, before)
				got, trap := exported(export, args)

				if export != name {
					got = want
				}

				if got != want || trap != wantTrap || !bytes.Equal(mem, wantMem) {
					t.Fatalf("%+v, %d bytes: %s%v is %#x, trap %d, the kernel's %#x, trap %d; memory equal: %v", options, len(mem), export, args, got, trap, want, wantTrap, bytes.Equal(mem, wantMem))
				}
			}
		}

		f, _ := i.export("grow")
		grow := f.Interface().(func(uintptr, uint32) uint32)
		random := rand.New(rand.NewSource(1))

		for pages := uint32(1); pages <= 3; pages++ {
			if pages > 1 && grow(i.ctx(), 1) != pages-1 {
				t.Fatalf("%+v: no growth to %d pages", options, pages)
			}

			mem := i.memoryBytes()
			end := uint32(len(mem))
			// base has neither NUL nor 0xff: a case places them
			base := make([]byte, end)

			for k := range base {
				base[k] = byte(1 + random.Intn(0xfe))
			}

			edges := []uint32{0, 1, 1023, 1024, 4095, 4096, 4097, pageSize - 1, pageSize, end - 4096, end - 300, end - 65, end - 64, end - 63, end - 17, end - 16, end - 2, end - 1, end, end + 1, end + 4096, 1 << 31, ^uint32(0)}
			at := func() uint32 {
				if random.Intn(3) == 0 {
					return uint32(random.Intn(int(end)))
				}

				return edges[random.Intn(len(edges))]
			}

			// length is a length of a range or a string at a.
			length := func(a uint32) uint32 {
				room := end - a
				lengths := []uint32{0, 1, 2, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129, 300, 4096, 70000, room - 1, room, room + 1, ^uint32(0)}

				if random.Intn(4) == 0 {
					return uint32(random.Intn(300))
				}

				return lengths[random.Intn(len(lengths))]
			}

			// put writes c at a + n when that is within the memory.
			put := func(a, n uint32, c byte) {
				if uint64(a)+uint64(n) < uint64(end) {
					mem[a+n] = c
				}
			}

			// str ends a string at a, unless it runs to the memory's end.
			str := func(a uint32) {
				put(a, length(a), 0)
			}

			near := func(a uint32) uint32 {
				return a + uint32(random.Intn(65)) - 32
			}

			for range 1000 {
				copy(mem, base)

				switch random.Intn(10) {
				case 0:
					a, b := at(), at()

					if random.Intn(2) == 0 {
						b = near(a)
					}

					n := length(a)

					if uint64(a)+uint64(n) <= uint64(end) && uint64(b)+uint64(n) <= uint64(end) {
						copy(mem[b:b+n], mem[a:a+n])

						if n > 0 && random.Intn(3) != 0 {
							k := uint32(random.Intn(int(n)))
							x, y := byte(0x80), byte(0x01)

							if random.Intn(2) == 0 {
								x, y = y, x
							}

							mem[a+k], mem[b+k] = x, y
						}
					}

					check("memcmp", a, b, n)
				case 1:
					s := at()
					c := []uint32{0xff, 0x1ff, 0, 0x100, 'a', 0xffffff61}[random.Intn(6)]

					if random.Intn(4) != 0 {
						put(s, length(s), byte(c))
					}

					check("memchr", s, c, length(s))
				case 2:
					s := at()
					str(s)
					check("strlen", s)
				case 3:
					s := at()
					str(s)
					d := at()

					if random.Intn(2) == 0 {
						d = near(s)
					}

					check("strcpy", d, s)
				case 4:
					d, s := at(), at()

					if random.Intn(2) == 0 {
						s = near(d)
					}

					str(d)
					str(s)
					check("strcat", d, s)
				case 5:
					a, b := at(), at()

					if random.Intn(2) == 0 {
						b = near(a)
					}

					n := length(a)

					if uint64(a)+uint64(n) <= uint64(end) && uint64(b)+uint64(n) <= uint64(end) {
						copy(mem[b:b+n], mem[a:a+n])

						if n > 0 && random.Intn(2) == 0 {
							k := uint32(random.Intn(int(n)))
							x, y := byte(0x80), byte(0x01)

							if random.Intn(2) == 0 {
								x, y = y, x
							}

							mem[a+k], mem[b+k] = x, y
						}
					}

					if random.Intn(2) == 0 {
						k := length(a)
						put(a, k, 0)
						put(b, k, 0)
					} else {
						str(a)
						str(b)
					}

					check("strcmp", a, b)
				case 6:
					// a set of up to 8 bytes at set, and at s up to 300
					// bytes of it for strspn, of none of it for strcspn
					name := []string{"strspn", "strcspn"}[random.Intn(2)]
					s, set := at(), at()
					k := uint32(random.Intn(9))

					if uint64(set)+uint64(k) < uint64(end) {
						mem[set+k] = 0

						for j := range min(length(s), 300) {
							if k == 0 || uint64(s)+uint64(j) >= uint64(end) {
								break
							}

							c := mem[set+uint32(random.Intn(int(k)))]

							if name == "strcspn" {
								c = 0xff
							}

							mem[s+j] = c
						}
					}

					str(s)
					check(name, s, set)
				case 7:
					s, d := at(), at()

					if random.Intn(2) == 0 {
						d = near(s)
					}

					check("memmove", d, s, length(s))
				case 8:
					// memcpy's ranges do not overlap
					s, d := at(), at()
					n := length(s)

					if uint64(d) < uint64(s)+uint64(n) && uint64(s) < uint64(d)+uint64(n) {
						continue
					}

					check("memcpy", d, s, n)
				case 9:
					d := at()
					check("memset", d, uint32(random.Intn(1<<16)), length(d))
				}
			}

			// The ends of the memory.
			copy(mem, base)
			mem[end-1] = 0
			check("strlen", end-1)
			check("strlen", end-2)
			check("strcpy", end-2, end-1)
			check("strcpy", end-1, end-2)
			check("strcat", end-3, end-1)
			mem[end-1] = 1
			check("strlen", end-1)
			check("strlen", end)
			check("strlen", ^uint32(0))
			check("memchr", end, 'a', 0)
			check("memchr", end, 'a', 1)
			check("memchr", end+1, 'a', 0)
			check("memchr", end-1, 1, ^uint32(0))
			check("memchr", 0, 0, ^uint32(0))
			check("memchr", 0, 0, end)
			check("memcmp", end, end, 0)
			check("memcmp", end-1, 0, 2)
			check("memcmp", 0, end-1, 1)
			check("memmove", end-1, 0, 1)
			check("memmove", end-1, 0, 2)
			check("memcpy", 0, end-1, 1)
			check("memcpy", 0, end, 1)
			check("memset", end-1, 7, 1)
			check("memset", end, 7, 0)
			check("memset", end, 7, 1)
			check("strcmp", end-1, end-1)
			check("strcmp", 0, end-1)
			check("strspn", end-1, 0)
			check("strcspn", 0, end-1)
			mem[end-1] = 0
			check("strcmp", end-1, end-1)
			check("strcmp", end-2, end-1)
			check("strspn", end-2, end-1)
			check("strcspn", end-2, end-1)

			for k, b := range i.memory.data[end:] {
				if b != 0 {
					t.Fatalf("%+v, %d pages: byte %d past the accessible end is %#x", options, pages, k, b)
				}
			}
		}

		i.release()
	}
}

// Nothing may take the address of an import compiled at its calls, which has
// no code of its own: lib/c's pointers are to its own functions. A module
// exporting one or returning a reference to one is refused.
func TestStringFunctionAddressRefused(t *testing.T) {
	strlen := []testImport{{module: "kernel", name: "__kernel_strlen", params: []ValType{I32}, results: i32}}
	const opRefFunc = 0xd2

	for what, m := range map[string]*testModule{
		"an export":   {imports: strlen, importExports: map[int]string{0: "strlen"}},
		"a reference": {imports: strlen, funcs: []testFunc{{results: []ValType{FuncRef}, export: "f", code: []byte{opRefFunc, 0}}}},
	} {
		if e := try(func() { compileModule(m.encode()) }); e == nil || !strings.Contains(e.message(), "compiled at its calls") {
			t.Fatalf("%s of __kernel_strlen: %v", what, e)
		}
	}
}
