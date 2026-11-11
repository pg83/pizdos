//go:build linux && amd64

package main

import (
	"bytes"
	"math/rand"
	"slices"
	"testing"
	"unsafe"
)

// lib/str links once: str_choose names one of its configurations, one the
// host runs by what str_host reports, every public slot of its table is in
// the text, and memmove through the table copies as copy does, every
// length up to 300 and some far longer, from every alignment, overlapping
// either way.
func TestStrLibrary(t *testing.T) {
	lib := strLib()

	if !slices.Contains([]string{"sse2", "sse42", "avx2", "evex", "evex512", "evex512r"}, lib.config) {
		t.Fatalf("str_choose chose %q", lib.config)
	}

	if !lib.runs[lib.config] {
		t.Fatalf("str_choose chose %s, which str_host says the host does not run", lib.config)
	}

	if len(lib.slots) != len(strSlots) {
		t.Fatalf("%d slots, want %d", len(lib.slots), len(strSlots))
	}

	memmove := strFunction(symStr + "memmove")
	buf := make([]byte, 4<<20)
	random := rand.New(rand.NewSource(1))
	random.Read(buf)
	want := slices.Clone(buf)
	lengths := []uint32{511, 512, 513, 4095, 4096, 4097, 65536, 100003, 1<<20 + 77}

	for n := range uint32(301) {
		lengths = append(lengths, n)
	}

	for _, n := range lengths {
		for _, pair := range [][2]uint32{{8192, 1 << 21}, {8195, 1<<21 + 5}, {1<<21 + 1, 8199}, {8192, 8193}, {8193, 8192}, {8192, 8192 + 33}, {8192 + 31, 8192}} {
			d, s := pair[0], pair[1]

			if n > 0 {
				strMove(memmove, unsafe.Pointer(&buf[d]), unsafe.Pointer(&buf[s]), uintptr(n))
				copy(want[d:d+n], want[s:s+n])
			}

			if !bytes.Equal(buf, want) {
				t.Fatalf("memmove of %d bytes from %d to %d (%s) differs", n, s, d, lib.config)
			}
		}
	}
}

// Every configuration of lib/str the host runs copies, fills, compares and
// searches as Go does, at random lengths up to 5000 and some up to 2 MiB,
// from random alignments: memmove overlapping either way, memcpy, memset
// with random bytes, memcmp of equal ranges with a byte changed or not (its
// sign), memchr of a byte placed or not, strlen of a string ended within
// the length.
func TestStrConfigurations(t *testing.T) {
	lib := strLib()
	random := rand.New(rand.NewSource(2))
	buf := make([]byte, 8<<20)
	random.Read(buf)
	want := slices.Clone(buf)
	at := uintptr(unsafe.Pointer(&buf[0]))
	ran := 0

	for name, slots := range lib.configs {
		if !lib.runs[name] {
			continue
		}

		ran++

		for k := range 4000 {
			n := random.Intn(5000)

			if k%100 == 0 {
				n = random.Intn(2 << 20)
			}

			s := random.Intn(len(buf) - n)
			d := random.Intn(len(buf) - n)

			switch k % 6 {
			case 0:
				if k%2 == 0 {
					d = max(0, min(len(buf)-n, s+random.Intn(130)-65))
				}

				strCall3(slots["memmove"], at+uintptr(d), at+uintptr(s), uintptr(n))
				copy(want[d:d+n], want[s:s+n])
			case 1:
				if d < s+n && s < d+n {
					continue
				}

				strCall3(slots["memcpy"], at+uintptr(d), at+uintptr(s), uintptr(n))
				copy(want[d:d+n], want[s:s+n])
			case 2:
				c := random.Intn(1 << 16)
				strCall3(slots["memset"], at+uintptr(d), uintptr(c), uintptr(n))

				for j := d; j < d+n; j++ {
					want[j] = byte(c)
				}
			case 3:
				copy(buf[d:d+n], buf[s:s+n])
				copy(want[d:d+n], want[s:s+n])

				if n > 0 && random.Intn(3) != 0 {
					j, v := d+random.Intn(n), byte(random.Intn(256))
					buf[j], want[j] = v, v
				}

				got := int32(strCall3(slots["memcmp"], at+uintptr(s), at+uintptr(d), uintptr(n)))

				if sign := bytes.Compare(buf[s:s+n], buf[d:d+n]); min(max(got, -1), 1) != int32(sign) {
					t.Fatalf("%s, step %d: memcmp of %d bytes at %d and %d is %d, want the sign of %d", name, k, n, s, d, got, sign)
				}
			case 4:
				c := random.Intn(1 << 16)

				if n > 0 && random.Intn(2) == 0 {
					j := s + random.Intn(n)
					buf[j], want[j] = byte(c), byte(c)
				}

				found := uintptr(0)

				if j := bytes.IndexByte(buf[s:s+n], byte(c)); j >= 0 {
					found = at + uintptr(s+j)
				}

				if got := strCall3(slots["memchr"], at+uintptr(s), uintptr(c), uintptr(n)); got != found {
					t.Fatalf("%s, step %d: memchr of %#x in %d bytes at %d is %#x, want %#x", name, k, c, n, s, got, found)
				}
			case 5:
				buf[s+n], want[s+n] = 0, 0

				if got, length := strCall3(slots["strlen"], at+uintptr(s), 0, 0), bytes.IndexByte(buf[s:], 0); got != uintptr(length) {
					t.Fatalf("%s, step %d: strlen at %d is %d, want %d", name, k, s, got, length)
				}
			}

			if !bytes.Equal(buf, want) {
				i := 0

				for buf[i] == want[i] {
					i++
				}

				t.Fatalf("%s, step %d (%d bytes from %d to %d): byte %d differs", name, k, n, s, d, i)
			}
		}
	}

	if ran == 0 {
		t.Fatal("the host runs no configuration")
	}
}
