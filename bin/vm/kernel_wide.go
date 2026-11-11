//go:build linux && amd64

package main

import (
	"bytes"
	"encoding/binary"
	"math"
	"slices"
	"strings"
	"syscall"
	"unicode"
	"unicode/utf8"
)

// Wide strings. A wchar_t is a 32-bit int, compared signed as musl does.
// Like the byte strings they are not system calls; a string that runs past
// the end of memory traps.

// wide is the memory of n wide characters at at.
func (p *Process) wide(at, n uint32) []byte {
	m := p.mem()
	end := uint64(at) + 4*uint64(n)

	if end > uint64(len(m)) {
		oob()
	}

	return m[at:end]
}

// wcharAt is the i-th wide character from at.
func (p *Process) wcharAt(at, i uint32) uint32 {
	m := p.mem()
	o := uint64(at) + 4*uint64(i)

	if o+4 > uint64(len(m)) {
		oob()
	}

	return binary.LittleEndian.Uint32(m[o:])
}

func (p *Process) wcsnlen(at, n uint32) uint32 {
	for i := uint32(0); i < n; i++ {
		if p.wcharAt(at, i) == 0 {
			return i
		}
	}

	return n
}

func (p *Process) wcslen(at uint32) uint32 {
	return p.wcsnlen(at, math.MaxUint32)
}

// wstring is the wide string at at, without its terminator.
func (p *Process) wstring(at uint32) []uint32 {
	n := p.wcslen(at)
	b := p.wide(at, n)
	out := make([]uint32, n)

	for i := range out {
		out[i] = binary.LittleEndian.Uint32(b[4*i:])
	}

	return out
}

// wcmp is a C comparison of two wide characters.
func wcmp(a, b uint32) uint32 {
	switch {
	case int32(a) < int32(b):
		return math.MaxUint32
	case int32(a) > int32(b):
		return 1
	}

	return 0
}

// wcsspan counts the leading characters of the string at at that are in
// the string set, or with reject that are not.
func (p *Process) wcsspan(at, set uint32, reject bool) uint32 {
	chars := p.wstring(set)

	for i := uint32(0); ; i++ {
		c := p.wcharAt(at, i)

		if c == 0 || slices.Contains(chars, c) == reject {
			return i
		}
	}
}

func (p *Process) wcscpy(d, s uint32) uint32 {
	n := p.wcslen(s) + 1
	copy(p.wide(d, n), p.wide(s, n))

	return d
}

// wcsftime is strftime of a wide format, its result wide.
func (p *Process) wcsftime(s, max, format, tm uint32) uint32 {
	var f strings.Builder

	for _, c := range p.wstring(format) {
		r := rune(c)

		if c > unicode.MaxRune || !utf8.ValidRune(r) {
			r = utf8.RuneError
		}

		f.WriteRune(r)
	}

	out := []rune(strftime(f.String(), p.readTm(tm)))

	if uint32(len(out)) >= max {
		return 0
	}

	b := p.wide(s, uint32(len(out))+1)

	for i, r := range out {
		binary.LittleEndian.PutUint32(b[4*i:], uint32(r))
	}

	binary.LittleEndian.PutUint32(b[4*len(out):], 0)

	return uint32(len(out))
}

func (p *Process) libcWcsncpy(d, s, n uint32) uint32 {
	l := p.wcsnlen(s, n)
	dst := p.wide(d, n)
	copy(dst, p.wide(s, l))
	clear(dst[4*l:])

	return d
}

func (p *Process) libcWcscat(d, s uint32) uint32 {
	p.wcscpy(d+4*p.wcslen(d), s)

	return d
}

func (p *Process) libcWcscmp(a, b uint32) uint32 {
	for i := uint32(0); ; i++ {
		x, y := p.wcharAt(a, i), p.wcharAt(b, i)

		if x != y || x == 0 {
			return wcmp(x, y)
		}
	}
}

func (p *Process) libcWcsncmp(a, b, n uint32) uint32 {
	for i := uint32(0); i < n; i++ {
		x, y := p.wcharAt(a, i), p.wcharAt(b, i)

		if x != y || x == 0 {
			return wcmp(x, y)
		}
	}

	return 0
}

// wcscoll and wcsxfrm are the C locale's: the order of the values.
func (p *Process) libcWcscoll(a, b uint32) uint32 {
	for i := uint32(0); ; i++ {
		x, y := p.wcharAt(a, i), p.wcharAt(b, i)

		if x != y || x == 0 {
			return wcmp(x, y)
		}
	}
}

func (p *Process) libcWcsxfrm(d, s, n uint32) uint32 {
	l := p.wcslen(s)

	if l < n {
		copy(p.wide(d, l+1), p.wide(s, l+1))
	} else if n > 0 {
		copy(p.wide(d, n-1), p.wide(s, n-1))
		binary.LittleEndian.PutUint32(p.wide(d+4*(n-1), 1), 0)
	}

	return l
}

func (p *Process) libcWcschr(s, c uint32) uint32 {
	for i := uint32(0); ; i++ {
		x := p.wcharAt(s, i)

		if x == c {
			return s + 4*i
		}

		if x == 0 {
			return 0
		}
	}
}

func (p *Process) libcWcsrchr(s, c uint32) uint32 {
	for i := p.wcslen(s); ; i-- {
		if p.wcharAt(s, i) == c {
			return s + 4*i
		}

		if i == 0 {
			return 0
		}
	}
}

func (p *Process) libcWcsstr(h, n uint32) uint32 {
	needle := p.wide(n, p.wcslen(n))
	hay := p.wide(h, p.wcslen(h))

	for from := 0; ; {
		i := bytes.Index(hay[from:], needle)

		if i < 0 {
			return 0
		}

		if (from+i)%4 == 0 {
			return h + uint32(from+i)
		}

		from += i + 1
	}
}

func (p *Process) libcWcspbrk(s, set uint32) uint32 {
	at := s + 4*p.wcsspan(s, set, true)

	if p.wcharAt(at, 0) == 0 {
		return 0
	}

	return at
}

func (p *Process) libcWcstok(s, sep, save uint32) uint32 {
	if s == 0 {
		if s = p.u32(save); s == 0 {
			return 0
		}
	}

	s += 4 * p.wcsspan(s, sep, false)

	if p.wcharAt(s, 0) == 0 {
		p.put32(save, 0)

		return 0
	}

	end := s + 4*p.wcsspan(s, sep, true)

	if p.wcharAt(end, 0) == 0 {
		p.put32(save, 0)
	} else {
		p.put32(end, 0)
		p.put32(save, end+4)
	}

	return s
}

func (p *Process) libcWcsdup(s uint32) uint32 {
	n := p.wcslen(s) + 1

	if uint64(n)*4 > heapMax {
		p.setErrno(syscall.ENOMEM)

		return 0
	}

	at := p.malloc(4 * n)

	if at == 0 {
		p.setErrno(syscall.ENOMEM)

		return 0
	}

	copy(p.wide(at, n), p.wide(s, n))

	return at
}

func (p *Process) libcWmemchr(s, c, n uint32) uint32 {
	for i := uint32(0); i < n; i++ {
		if p.wcharAt(s, i) == c {
			return s + 4*i
		}
	}

	return 0
}

func (p *Process) libcWmemcmp(a, b, n uint32) uint32 {
	for i := uint32(0); i < n; i++ {
		if x, y := p.wcharAt(a, i), p.wcharAt(b, i); x != y {
			return wcmp(x, y)
		}
	}

	return 0
}

func (p *Process) libcWmemcpy(d, s, n uint32) uint32 {
	copy(p.wide(d, n), p.wide(s, n))

	return d
}

func (p *Process) libcWmemmove(d, s, n uint32) uint32 {
	copy(p.wide(d, n), p.wide(s, n))

	return d
}

func (p *Process) libcWmemset(d, c, n uint32) uint32 {
	b := p.wide(d, n)

	for i := 0; i < len(b); i += 4 {
		binary.LittleEndian.PutUint32(b[i:], c)
	}

	return d
}

func (p *Process) libcWcwidth(c uint32) uint32 {
	return uint32(wcwidth(c))
}

func (p *Process) libcWcswidth(s, n uint32) uint32 {
	total := int32(0)

	for i := uint32(0); i < n; i++ {
		c := p.wcharAt(s, i)

		if c == 0 {
			break
		}

		w := wcwidth(c)

		if w < 0 {
			return uint32(w)
		}

		total += w
	}

	return uint32(total)
}

func (p *Process) libcWcslen(s uint32) uint32 {
	return p.wcslen(s)
}

func (p *Process) libcWcsnlen(s, n uint32) uint32 {
	return p.wcsnlen(s, n)
}

func (p *Process) libcWcscpy(d, s uint32) uint32 {
	return p.wcscpy(d, s)
}

func (p *Process) libcWcsspn(s, set uint32) uint32 {
	return p.wcsspan(s, set, false)
}

func (p *Process) libcWcscspn(s, set uint32) uint32 {
	return p.wcsspan(s, set, true)
}

func (p *Process) libcWcsftime(s, max, format, tm uint32) uint32 {
	return p.wcsftime(s, max, format, tm)
}
