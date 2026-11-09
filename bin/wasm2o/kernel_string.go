//go:build linux && amd64

package main

import (
	"bytes"
	"fmt"
	"math"
	"math/bits"
	"syscall"
)

// String and character functions of the C locale. They are not system
// calls: they leave errno and pending signals alone.

func lower(c byte) byte {
	if c >= 'A' && c <= 'Z' {
		return c + 'a' - 'A'
	}

	return c
}

func isDigit(c byte) bool {
	return c >= '0' && c <= '9'
}

// cmp is a C comparison result.
func cmp(a, b int) uint32 {
	return uint32(int32(a - b))
}

// bounded is up to n bytes of memory at at, all the memory has; past its
// end a search that found nothing traps.
func (p *Process) bounded(at, n uint32) []byte {
	m := p.mem()

	if uint64(at) > uint64(len(m)) {
		oob()
	}

	end := min(uint64(at)+uint64(n), uint64(len(m)))

	return m[at:end]
}

func (p *Process) search(at, n uint32, c byte) (uint32, bool) {
	b := p.bounded(at, n)
	i := bytes.IndexByte(b, c)

	if i < 0 {
		if uint32(len(b)) < n {
			oob()
		}

		return 0, false
	}

	return at + uint32(i), true
}

func (p *Process) strnlen(at, n uint32) uint32 {
	end, ok := p.search(at, n, 0)

	if !ok {
		return n
	}

	return end - at
}

func (p *Process) strncmp(a, b, n uint32, fold bool) uint32 {
	for i := uint32(0); i < n; i++ {
		x, y := p.u8(a+i), p.u8(b+i)

		if fold {
			x, y = lower(x), lower(y)
		}

		if x != y || x == 0 {
			return cmp(int(x), int(y))
		}
	}

	return 0
}

// set is the bytes of the string at at.
func (p *Process) set(at uint32) [256]bool {
	var s [256]bool

	for _, c := range p.cstr(at) {
		s[c] = true
	}

	return s
}

// span counts the leading bytes of the string at at that are in set, or
// with reject that are not.
func (p *Process) spanSet(at, set uint32, reject bool) uint32 {
	in := p.set(set)
	s := p.cstr(at)

	for i, c := range s {
		if in[c] == reject {
			return uint32(i)
		}
	}

	return uint32(len(s))
}

func (p *Process) strstr(h, n uint32, fold bool) uint32 {
	hay, needle := p.cstr(h), p.cstr(n)

	if fold {
		hay, needle = bytes.ToLower(hay), bytes.ToLower(needle)
	}

	i := bytes.Index(hay, needle)

	if i < 0 {
		return 0
	}

	return h + uint32(i)
}

func (p *Process) strcpy(d, s uint32) uint32 {
	src := p.cstr(s)
	n := uint32(len(src))
	copy(p.span(d, n+1), p.span(s, n+1))

	return d + n
}

func (p *Process) stpncpy(d, s, n uint32) uint32 {
	l := p.strnlen(s, n)
	dst := p.span(d, n)
	copy(dst, p.span(s, l))
	clear(dst[l:])

	return d + l
}

func (p *Process) strndup(s, n uint32) uint32 {
	l := p.strnlen(s, n)
	at := p.malloc(l + 1)

	if at == 0 {
		p.setErrno(syscall.ENOMEM)

		return 0
	}

	copy(p.span(at, l), p.span(s, l))
	p.span(at+l, 1)[0] = 0

	return at
}

// strtok is strtok_r with the save pointer's value.
func (p *Process) strtok(s, delim uint32, save uint32) (token, next uint32) {
	if s == 0 {
		s = save
	}

	if s == 0 {
		return 0, 0
	}

	s += p.spanSet(s, delim, false)

	if p.u8(s) == 0 {
		return 0, 0
	}

	end := s + p.spanSet(s, delim, true)

	if p.u8(end) == 0 {
		return s, 0
	}

	p.span(end, 1)[0] = 0

	return s, end + 1
}

// divide is C's division of two ints: as wasm's, it traps by zero and on
// overflow.
func divide(a, b, minimum int64) (int64, int64) {
	if b == 0 {
		panic(&Trap{trapDivByZero})
	}

	if a == minimum && b == -1 {
		panic(&Trap{trapIntOverflow})
	}

	return a / b, a % b
}

func ffs(x uint64) uint32 {
	if x == 0 {
		return 0
	}

	return uint32(bits.TrailingZeros64(x) + 1)
}

func (k *Kernel) stringFunctions() map[string]any {
	t := map[string]any{
		"strlen": func(ctx uintptr, s uint32) uint32 {
			return uint32(len(proc(ctx).cstr(s)))
		},
		"strnlen": func(ctx uintptr, s, n uint32) uint32 {
			return proc(ctx).strnlen(s, n)
		},
		"strcmp": func(ctx uintptr, a, b uint32) uint32 {
			p := proc(ctx)

			return uint32(int32(bytes.Compare(p.cstr(a), p.cstr(b))))
		},
		"strncmp": func(ctx uintptr, a, b, n uint32) uint32 {
			return proc(ctx).strncmp(a, b, n, false)
		},
		"strcasecmp": func(ctx uintptr, a, b uint32) uint32 {
			return proc(ctx).strncmp(a, b, ^uint32(0), true)
		},
		"strncasecmp": func(ctx uintptr, a, b, n uint32) uint32 {
			return proc(ctx).strncmp(a, b, n, true)
		},
		"memcmp": func(ctx uintptr, a, b, n uint32) uint32 {
			p := proc(ctx)

			return uint32(int32(bytes.Compare(p.span(a, n), p.span(b, n))))
		},
		"memchr": func(ctx uintptr, s, c, n uint32) uint32 {
			at, _ := proc(ctx).search(s, n, byte(c))

			return at
		},
		"strchr": func(ctx uintptr, s, c uint32) uint32 {
			p := proc(ctx)
			str := p.cstr(s)

			if byte(c) == 0 {
				return s + uint32(len(str))
			}

			if i := bytes.IndexByte(str, byte(c)); i >= 0 {
				return s + uint32(i)
			}

			return 0
		},
		"strchrnul": func(ctx uintptr, s, c uint32) uint32 {
			p := proc(ctx)
			str := p.cstr(s)

			if i := bytes.IndexByte(str, byte(c)); i >= 0 && byte(c) != 0 {
				return s + uint32(i)
			}

			return s + uint32(len(str))
		},
		"strrchr": func(ctx uintptr, s, c uint32) uint32 {
			p := proc(ctx)
			str := p.cstr(s)

			if byte(c) == 0 {
				return s + uint32(len(str))
			}

			if i := bytes.LastIndexByte(str, byte(c)); i >= 0 {
				return s + uint32(i)
			}

			return 0
		},
		"strspn": func(ctx uintptr, s, accept uint32) uint32 {
			return proc(ctx).spanSet(s, accept, false)
		},
		"strcspn": func(ctx uintptr, s, reject uint32) uint32 {
			return proc(ctx).spanSet(s, reject, true)
		},
		"strpbrk": func(ctx uintptr, s, accept uint32) uint32 {
			p := proc(ctx)
			n := p.spanSet(s, accept, true)

			if p.u8(s+n) == 0 {
				return 0
			}

			return s + n
		},
		"strstr": func(ctx uintptr, h, n uint32) uint32 {
			return proc(ctx).strstr(h, n, false)
		},
		"strcasestr": func(ctx uintptr, h, n uint32) uint32 {
			return proc(ctx).strstr(h, n, true)
		},
		"strcpy": func(ctx uintptr, d, s uint32) uint32 {
			proc(ctx).strcpy(d, s)

			return d
		},
		"stpcpy": func(ctx uintptr, d, s uint32) uint32 {
			return proc(ctx).strcpy(d, s)
		},
		"strncpy": func(ctx uintptr, d, s, n uint32) uint32 {
			proc(ctx).stpncpy(d, s, n)

			return d
		},
		"stpncpy": func(ctx uintptr, d, s, n uint32) uint32 {
			return proc(ctx).stpncpy(d, s, n)
		},
		"strcat": func(ctx uintptr, d, s uint32) uint32 {
			p := proc(ctx)
			p.strcpy(d+uint32(len(p.cstr(d))), s)

			return d
		},
		"mempcpy": func(ctx uintptr, d, s, n uint32) uint32 {
			p := proc(ctx)
			copy(p.span(d, n), p.span(s, n))

			return d + n
		},
		"strdup": func(ctx uintptr, s uint32) uint32 {
			return proc(ctx).strndup(s, ^uint32(0))
		},
		"strndup": func(ctx uintptr, s, n uint32) uint32 {
			return proc(ctx).strndup(s, n)
		},
		"strtok_r": func(ctx uintptr, s, delim, save uint32) uint32 {
			p := proc(ctx)
			token, next := p.strtok(s, delim, p.u32(save))
			p.put32(save, next)

			return token
		},
		"strsep": func(ctx uintptr, at, delim uint32) uint32 {
			p := proc(ctx)
			s := p.u32(at)

			if s == 0 {
				return 0
			}

			end := s + p.spanSet(s, delim, true)

			if p.u8(end) == 0 {
				p.put32(at, 0)
			} else {
				p.span(end, 1)[0] = 0
				p.put32(at, end+1)
			}

			return s
		},
		"strverscmp": func(ctx uintptr, a, b uint32) uint32 {
			p := proc(ctx)

			return uint32(int32(strverscmp(p.cstr(a), p.cstr(b))))
		},
		"isspace": func(ctx uintptr, c uint32) uint32 {
			switch c {
			case ' ', '\t', '\n', '\v', '\f', '\r':
				return 1
			}

			return 0
		},
		"isdigit": func(ctx uintptr, c uint32) uint32 {
			if c >= '0' && c <= '9' {
				return 1
			}

			return 0
		},
		"tolower": func(ctx uintptr, c uint32) uint32 {
			if c >= 'A' && c <= 'Z' {
				return c + ('a' - 'A')
			}

			return c
		},
		"toupper": func(ctx uintptr, c uint32) uint32 {
			if c >= 'a' && c <= 'z' {
				return c - ('a' - 'A')
			}

			return c
		},
		"abs": func(ctx uintptr, n uint32) uint32 {
			if int32(n) < 0 {
				return uint32(-int32(n))
			}

			return n
		},
		"strerror": func(ctx uintptr, e uint32) uint32 {
			p := proc(ctx)
			at := p.staticAt(staticStrerror)
			p.putString(at, truncate(errorMessage(syscall.Errno(e)), staticStrsignal-staticStrerror-1))

			return at
		},
		"strsignal": func(ctx uintptr, sig uint32) uint32 {
			p := proc(ctx)
			at := p.staticAt(staticStrsignal)
			p.putString(at, truncate(signalMessage(int(sig)), staticLconv-staticStrsignal-1))

			return at
		},
		"dirname": func(ctx uintptr, s uint32) uint32 {
			p := proc(ctx)

			if s == 0 || p.u8(s) == 0 {
				return p.staticAt(staticDot)
			}

			b := p.cstr(s)
			i := len(b) - 1

			for ; b[i] == '/'; i-- {
				if i == 0 {
					return p.staticAt(staticSlash)
				}
			}

			for ; b[i] != '/'; i-- {
				if i == 0 {
					return p.staticAt(staticDot)
				}
			}

			for ; b[i] == '/'; i-- {
				if i == 0 {
					return p.staticAt(staticSlash)
				}
			}

			p.span(s+uint32(i)+1, 1)[0] = 0

			return s
		},
		"fnmatch": func(ctx uintptr, pattern, s, flags uint32) uint32 {
			p := proc(ctx)

			if fnmatch(p.cstr(pattern), p.cstr(s), int(flags), true) {
				return 0
			}

			return 1
		},
		"memrchr": func(ctx uintptr, s, c, n uint32) uint32 {
			p := proc(ctx)

			if i := bytes.LastIndexByte(p.span(s, n), byte(c)); i >= 0 {
				return s + uint32(i)
			}

			return 0
		},
		"rawmemchr": func(ctx uintptr, s, c uint32) uint32 {
			at, _ := proc(ctx).search(s, math.MaxUint32, byte(c))

			return at
		},
		"memmem": func(ctx uintptr, h, hn, n, nn uint32) uint32 {
			p := proc(ctx)

			if i := bytes.Index(p.span(h, hn), p.span(n, nn)); i >= 0 {
				return h + uint32(i)
			}

			return 0
		},
		"memccpy": func(ctx uintptr, d, s, c, n uint32) uint32 {
			p := proc(ctx)
			end, found := p.search(s, n, byte(c))
			l := n

			if found {
				l = end - s + 1
			}

			copy(p.span(d, l), p.span(s, l))

			if found {
				return d + l
			}

			return 0
		},
		"bcopy": func(ctx uintptr, s, d, n uint32) {
			p := proc(ctx)
			copy(p.span(d, n), p.span(s, n))
		},
		"bzero": func(ctx uintptr, d, n uint32) {
			clear(proc(ctx).span(d, n))
		},
		"explicit_bzero": func(ctx uintptr, d, n uint32) {
			clear(proc(ctx).span(d, n))
		},
		"swab": func(ctx uintptr, s, d, n uint32) {
			p := proc(ctx)

			if int32(n) < 2 {
				return
			}

			n &^= 1
			src, dst := p.span(s, n), p.span(d, n)

			for i := uint32(0); i < n; i += 2 {
				dst[i], dst[i+1] = src[i+1], src[i]
			}
		},
		"strncat": func(ctx uintptr, d, s, n uint32) uint32 {
			p := proc(ctx)
			end := d + uint32(len(p.cstr(d)))
			l := p.strnlen(s, n)
			copy(p.span(end, l), p.span(s, l))
			p.span(end+l, 1)[0] = 0

			return d
		},
		"strlcpy": func(ctx uintptr, d, s, n uint32) uint32 {
			p := proc(ctx)
			src := p.cstr(s)

			if n > 0 {
				k := min(uint32(len(src)), n-1)
				copy(p.span(d, k), src[:k])
				p.span(d+k, 1)[0] = 0
			}

			return uint32(len(src))
		},
		"strlcat": func(ctx uintptr, d, s, n uint32) uint32 {
			p := proc(ctx)
			l := p.strnlen(d, n)
			src := p.cstr(s)

			if l == n {
				return l + uint32(len(src))
			}

			k := min(uint32(len(src)), n-l-1)
			copy(p.span(d+l, k), src[:k])
			p.span(d+l+k, 1)[0] = 0

			return l + uint32(len(src))
		},
		"strtok": func(ctx uintptr, s, delim uint32) uint32 {
			p := proc(ctx)
			save := p.staticAt(staticStrtok)
			token, next := p.strtok(s, delim, p.u32(save))
			p.put32(save, next)

			return token
		},
		// strcoll and strxfrm are the C locale's: the order of the bytes.
		"strcoll": func(ctx uintptr, a, b uint32) uint32 {
			p := proc(ctx)

			return uint32(int32(bytes.Compare(p.cstr(a), p.cstr(b))))
		},
		"strxfrm": func(ctx uintptr, d, s, n uint32) uint32 {
			p := proc(ctx)
			l := uint32(len(p.cstr(s)))

			if n > l {
				p.strcpy(d, s)
			}

			return l
		},
		"strerror_r": func(ctx uintptr, e, buf, n uint32) uint32 {
			p := proc(ctx)
			msg := errorMessage(syscall.Errno(e))

			if uint32(len(msg)) >= n {
				if n > 0 {
					p.putString(buf, msg[:n-1])
				}

				return uint32(syscall.ERANGE)
			}

			p.putString(buf, msg)

			return 0
		},
		"basename": func(ctx uintptr, s uint32) uint32 {
			p := proc(ctx)

			if s == 0 || p.u8(s) == 0 {
				return p.staticAt(staticDot)
			}

			b := p.cstr(s)
			i := len(b) - 1

			for ; i > 0 && b[i] == '/'; i-- {
				b[i] = 0
			}

			for ; i > 0 && b[i-1] != '/'; i-- {
			}

			return s + uint32(i)
		},
		"ffs": func(ctx uintptr, x uint32) uint32 {
			return ffs(uint64(x))
		},
		"ffsl": func(ctx uintptr, x uint32) uint32 {
			return ffs(uint64(x))
		},
		"ffsll": func(ctx uintptr, x uint64) uint32 {
			return ffs(x)
		},
		"labs": func(ctx uintptr, n uint32) uint32 {
			if int32(n) < 0 {
				return uint32(-int32(n))
			}

			return n
		},
		"llabs": func(ctx uintptr, n uint64) uint64 {
			if int64(n) < 0 {
				return uint64(-int64(n))
			}

			return n
		},
		"div": func(ctx uintptr, ret, a, b uint32) {
			p := proc(ctx)
			q, r := divide(int64(int32(a)), int64(int32(b)), math.MinInt32)
			p.put32(ret, uint32(q))
			p.put32(ret+4, uint32(r))
		},
		"lldiv": func(ctx uintptr, ret uint32, a, b uint64) {
			p := proc(ctx)
			q, r := divide(int64(a), int64(b), math.MinInt64)
			p.put64(ret, uint64(q))
			p.put64(ret+8, uint64(r))
		},
	}

	t["index"], t["rindex"] = t["strchr"], t["strrchr"]
	t["ldiv"], t["imaxdiv"], t["imaxabs"] = t["div"], t["lldiv"], t["llabs"]

	return t
}

func truncate(s string, n int) string {
	if len(s) > n {
		return s[:n]
	}

	return s
}

func capitalize(s string) string {
	if s != "" && s[0] >= 'a' && s[0] <= 'z' {
		return string(s[0]-'a'+'A') + s[1:]
	}

	return s
}

func errorMessage(e syscall.Errno) string {
	if e == 0 {
		return "No error information"
	}

	s := e.Error()

	if len(s) > 6 && s[:6] == "errno " {
		return fmt.Sprintf("Unknown error %d", int(e))
	}

	return capitalize(s)
}

func signalMessage(sig int) string {
	if sig >= 34 && sig <= 64 {
		return fmt.Sprintf("Real-time signal %d", sig-34)
	}

	s := syscall.Signal(sig).String()

	if len(s) > 7 && s[:7] == "signal " {
		return fmt.Sprintf("Unknown signal %d", sig)
	}

	return capitalize(s)
}

// strverscmp is glibc's: digit runs compare as numbers, those with leading
// zeros as fractions.
func strverscmp(a, b []byte) int {
	const (
		sN = 0
		sI = 3
		sF = 6
		sZ = 9
		rC = 2
		rL = 3
	)
	next := [...]int{sN, sI, sZ, sN, sI, sI, sN, sF, sF, sN, sF, sZ}
	result := [...]int{
		rC, rC, rC, rC, rL, rC, rC, rC, rC,
		rC, -1, -1, 1, rL, rL, 1, rL, rL,
		rC, rC, rC, rC, rC, rC, rC, rC, rC,
		rC, 1, 1, -1, rC, rC, -1, rC, rC,
	}
	at := func(s []byte, i int) byte {
		if i < len(s) {
			return s[i]
		}

		return 0
	}
	kind := func(c byte) int {
		if c == '0' {
			return 2
		}

		if isDigit(c) {
			return 1
		}

		return 0
	}
	i := 0
	c1, c2 := at(a, 0), at(b, 0)
	state := sN + kind(c1)

	for c1 == c2 {
		if c1 == 0 {
			return 0
		}

		state = next[state]
		i++
		c1, c2 = at(a, i), at(b, i)
		state += kind(c1)
	}

	diff := int(c1) - int(c2)

	switch r := result[state*3+kind(c2)]; r {
	case rC:
		return diff
	case rL:
		j := i + 1

		for ; isDigit(at(a, j)); j++ {
			if !isDigit(at(b, j)) {
				return 1
			}
		}

		if isDigit(at(b, j)) {
			return -1
		}

		return diff
	default:
		return r
	}
}

const (
	fnmPathname   = 0x1
	fnmNoescape   = 0x2
	fnmPeriod     = 0x4
	fnmLeadingDir = 0x8
	fnmCasefold   = 0x10
)

// fnmatch matches a string against a shell pattern; start tells that s
// begins where a leading period must be matched explicitly.
func fnmatch(pat, s []byte, flags int, start bool) bool {
	hidden := func() bool {
		return start && flags&fnmPeriod != 0 && len(s) > 0 && s[0] == '.'
	}

	for len(pat) > 0 {
		var c byte

		switch pat[0] {
		case '*':
			for len(pat) > 0 && pat[0] == '*' {
				pat = pat[1:]
			}

			if hidden() {
				return false
			}

			for i := 0; i <= len(s); i++ {
				if fnmatch(pat, s[i:], flags, false) {
					return true
				}

				if i < len(s) && flags&fnmPathname != 0 && s[i] == '/' {
					return false
				}
			}

			return false
		case '?':
			if len(s) == 0 || flags&fnmPathname != 0 && s[0] == '/' || hidden() {
				return false
			}

			c, pat = s[0], pat[1:]
		case '[':
			if len(s) == 0 || flags&fnmPathname != 0 && s[0] == '/' || hidden() {
				return false
			}

			n, ok := bracket(pat, s[0], flags)

			if n == 0 {
				if s[0] != '[' {
					return false
				}

				pat = pat[1:]
			} else if !ok {
				return false
			} else {
				pat = pat[n:]
			}

			c = s[0]
		default:
			lit := pat[0]
			pat = pat[1:]

			if lit == '\\' && flags&fnmNoescape == 0 && len(pat) > 0 {
				lit, pat = pat[0], pat[1:]
			}

			if len(s) == 0 || lit != s[0] && (flags&fnmCasefold == 0 || lower(lit) != lower(s[0])) {
				return false
			}

			c = s[0]
		}

		s = s[1:]
		start = flags&fnmPathname != 0 && c == '/'
	}

	return len(s) == 0 || flags&fnmLeadingDir != 0 && s[0] == '/'
}

// bracket matches a byte against the bracket expression pat starts with:
// it returns the expression's length, 0 when it is not one, and whether
// it matched.
func bracket(pat []byte, c byte, flags int) (int, bool) {
	i := 1
	negate := false

	if i < len(pat) && (pat[i] == '!' || pat[i] == '^') {
		negate = true
		i++
	}

	fold := flags&fnmCasefold != 0
	matched := false
	first := true

	for {
		if i >= len(pat) {
			return 0, false
		}

		if pat[i] == ']' && !first {
			break
		}

		first = false

		if pat[i] == '[' && i+1 < len(pat) && pat[i+1] == ':' {
			end := bytes.Index(pat[i+2:], []byte(":]"))

			if end >= 0 {
				if inClass(string(pat[i+2:i+2+end]), c) || fold && (inClass(string(pat[i+2:i+2+end]), lower(c)) || inClass(string(pat[i+2:i+2+end]), upper(c))) {
					matched = true
				}

				i += end + 4

				continue
			}
		}

		lo := pat[i]

		if lo == '\\' && flags&fnmNoescape == 0 && i+1 < len(pat) {
			i++
			lo = pat[i]
		}

		i++
		hi := lo

		if i+1 < len(pat) && pat[i] == '-' && pat[i+1] != ']' {
			hi = pat[i+1]
			i += 2

			if hi == '\\' && flags&fnmNoescape == 0 && i < len(pat) {
				hi = pat[i]
				i++
			}
		}

		if lo <= c && c <= hi || fold && (lo <= lower(c) && lower(c) <= hi || lo <= upper(c) && upper(c) <= hi) {
			matched = true
		}
	}

	return i + 1, matched != negate
}

func upper(c byte) byte {
	if c >= 'a' && c <= 'z' {
		return c - 'a' + 'A'
	}

	return c
}

// inClass tells whether a byte is in a character class of the C locale.
func inClass(class string, c byte) bool {
	switch class {
	case "alnum":
		return isDigit(c) || lower(c) >= 'a' && lower(c) <= 'z'
	case "alpha":
		return lower(c) >= 'a' && lower(c) <= 'z'
	case "blank":
		return c == ' ' || c == '\t'
	case "cntrl":
		return c < 0x20 || c == 0x7f
	case "digit":
		return isDigit(c)
	case "graph":
		return c > 0x20 && c < 0x7f
	case "lower":
		return c >= 'a' && c <= 'z'
	case "print":
		return c >= 0x20 && c < 0x7f
	case "punct":
		return c > 0x20 && c < 0x7f && !isDigit(c) && !(lower(c) >= 'a' && lower(c) <= 'z')
	case "space":
		return c == ' ' || c >= '\t' && c <= '\r'
	case "upper":
		return c >= 'A' && c <= 'Z'
	case "xdigit":
		return isDigit(c) || lower(c) >= 'a' && lower(c) <= 'f'
	}

	return false
}
