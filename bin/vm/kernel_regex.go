//go:build linux && amd64

package main

import (
	"bytes"
	"encoding/binary"
	"regexp"
	"strconv"
	"strings"
)

// POSIX regular expressions, basic and extended with GNU's escapes,
// translated to Go's syntax and matched leftmost-longest. Back-references
// have no translation: such a pattern fails to compile. A compiled
// expression is the process's; regex_t holds its number.

const (
	regExtended = 1
	regIcase    = 2
	regNewline  = 4
	regNosub    = 8

	regNotbol   = 1
	regNoteol   = 2
	regStartend = 4

	regNomatch = 1
	regBadpat  = 2
	regEescape = 5
	regEsubreg = 6
	regEbrack  = 7
	regEparen  = 8
	regEbrace  = 9
	regBadbr   = 10
	regBadrpt  = 13
)

var regMessages = map[uint32]string{
	0: "No error", regNomatch: "No match", regBadpat: "Invalid regexp", 3: "Unknown collating element",
	4: "Unknown character class name", regEescape: "Trailing backslash", regEsubreg: "Invalid back reference",
	regEbrack: "Missing ']'", regEparen: "Missing ')'", regEbrace: "Missing '}'", regBadbr: "Invalid contents of {}",
	11: "Invalid character range", 12: "Out of memory", regBadrpt: "Repetition not preceded by valid expression",
}

type posixRegex struct {
	pattern []byte
	flags   uint32
	nsub    int
	// variants are compiled by their anchors: bit 0 when ^ cannot match,
	// bit 1 when $ cannot.
	variants [4]*regexp.Regexp
}

// variant is the expression with REG_NOTBOL and REG_NOTEOL applied.
func (r *posixRegex) variant(eflags uint32) *regexp.Regexp {
	v := eflags & (regNotbol | regNoteol)

	if r.variants[v] == nil {
		src, _, _ := translateRegex(r.pattern, r.flags, v&regNotbol != 0, v&regNoteol != 0)

		if r.flags&regNewline != 0 && v != 0 {
			// Consume the synthetic boundary characters outside a capture of
			// the user's expression. Multiline anchors still see real newlines,
			// while ^/$ cannot see the artificial string boundaries. Keeping
			// the match in a capture also preserves byte offsets after UTF-8.
			src = "(" + src + ")"

			if v&regNotbol != 0 {
				src = "(?s:.)" + src
			}

			if v&regNoteol != 0 {
				src += "(?s:.)"
			}
		}

		re := regexp.MustCompile(src)
		re.Longest()
		r.variants[v] = re
	}

	return r.variants[v]
}

// regexWriter builds the translation: atom is where the last atom starts
// in out, so a repetition of a repetition can wrap it.
type regexWriter struct {
	out      strings.Builder
	atom     int
	repeated bool
	groups   []int
	nsub     int
}

func (w *regexWriter) atomic(s string) {
	w.atom = w.out.Len()
	w.repeated = false
	w.out.WriteString(s)
}

func (w *regexWriter) repeat(op string) {
	if w.repeated {
		s := w.out.String()
		w.out.Reset()
		w.out.WriteString(s[:w.atom] + "(?:" + s[w.atom:] + ")")
	}

	w.out.WriteString(op)
	w.repeated = true
}

func quoteByte(c byte) string {
	return regexp.QuoteMeta(string([]byte{c}))
}

// translateRegex returns Go's syntax for a POSIX pattern and its number of
// groups, or a REG_ error code.
func translateRegex(pat []byte, flags uint32, bolNever, eolNever bool) (string, int, uint32) {
	ere := flags&regExtended != 0
	newline := flags&regNewline != 0
	w := &regexWriter{}

	switch {
	case newline:
		w.out.WriteString("(?m)")
	default:
		w.out.WriteString("(?s)")
	}

	if flags&regIcase != 0 {
		w.out.WriteString("(?i)")
	}

	w.atom = w.out.Len()
	// start tells that no atom precedes: a repetition is literal there and
	// a basic ^ an anchor.
	start := true
	never := `[^\x00-\x{10FFFF}]`

	for i := 0; i < len(pat); i++ {
		c := pat[i]
		wasStart := start
		start = false

		switch {
		case c == '\\':
			if i+1 == len(pat) {
				return "", 0, regEescape
			}

			i++
			e := pat[i]

			switch {
			case !ere && e == '(':
				w.groups = append(w.groups, w.out.Len())
				w.nsub++
				w.out.WriteString("(")
				start = true
			case !ere && e == ')':
				if len(w.groups) == 0 {
					return "", 0, regEparen
				}

				w.atom = w.groups[len(w.groups)-1]
				w.groups = w.groups[:len(w.groups)-1]
				w.out.WriteString(")")
				w.repeated = false
			case !ere && e == '|':
				w.out.WriteString("|")
				start = true
			case !ere && e == '{':
				end := bytes.Index(pat[i:], []byte(`\}`))

				if end < 0 {
					return "", 0, regEbrace
				}

				if wasStart {
					return "", 0, regBadrpt
				}

				interval, ok := parseInterval(string(pat[i+1 : i+end]))

				if !ok {
					return "", 0, regBadbr
				}

				w.repeat(interval)
				i += end + 1
			case !ere && (e == '+' || e == '?'):
				if wasStart {
					w.atomic(quoteByte(e))
				} else {
					w.repeat(string(e))
				}
			case e >= '1' && e <= '9':
				return "", 0, regEsubreg
			case e == '<' || e == '>':
				w.atomic(`\b`)
			case e == 'b' || e == 'B' || e == 'w' || e == 'W' || e == 's' || e == 'S':
				w.atomic(`\` + string(e))
			case e == '`':
				w.atomic(`\A`)
			case e == '\'':
				w.atomic(`\z`)
			case e == 'n':
				w.atomic(`\n`)
			case e == 't':
				w.atomic(`\t`)
			default:
				w.atomic(quoteByte(e))
			}
		case c == '[':
			class, n, code := translateBracket(pat[i:], newline)

			if code != 0 {
				return "", 0, code
			}

			w.atomic(class)
			i += n - 1
		case c == '*' || ere && (c == '+' || c == '?'):
			if wasStart {
				w.atomic(quoteByte(c))
			} else {
				w.repeat(string(c))
			}
		case ere && c == '{':
			end := bytes.IndexByte(pat[i:], '}')
			interval, ok := "", false

			if end > 0 {
				interval, ok = parseInterval(string(pat[i+1 : i+end]))
			}

			if !ok || wasStart {
				w.atomic(`\{`)
			} else {
				w.repeat(interval)
				i += end
			}
		case ere && c == '(':
			w.groups = append(w.groups, w.out.Len())
			w.nsub++
			w.out.WriteString("(")
			start = true
		case ere && c == ')':
			if len(w.groups) == 0 {
				w.atomic(`\)`)

				continue
			}

			w.atom = w.groups[len(w.groups)-1]
			w.groups = w.groups[:len(w.groups)-1]
			w.out.WriteString(")")
			w.repeated = false
		case ere && c == '|':
			w.out.WriteString("|")
			start = true
		case c == '^':
			switch {
			case !ere && !wasStart:
				w.atomic(`\^`)
			case bolNever && !newline:
				w.atomic(never)
				start = true
			default:
				w.atomic("^")
				start = true
			}
		case c == '$':
			last := i+1 == len(pat) || !ere && i+2 < len(pat) && pat[i+1] == '\\' && (pat[i+2] == ')' || pat[i+2] == '|')

			switch {
			case !ere && !last:
				w.atomic(`\$`)
			case eolNever && !newline:
				w.atomic(never)
			default:
				w.atomic("$")
			}
		case c == '.':
			w.atomic(".")
		default:
			w.atomic(quoteByte(c))
		}
	}

	if len(w.groups) > 0 {
		return "", 0, regEparen
	}

	return w.out.String(), w.nsub, 0
}

// parseInterval checks the m, m, or m,n of an interval.
func parseInterval(s string) (string, bool) {
	lo, hi, comma := s, "", false

	if at := strings.IndexByte(s, ','); at >= 0 {
		lo, hi, comma = s[:at], s[at+1:], true
	}

	m, err := strconv.Atoi(lo)

	if err != nil || m > 1000 {
		return "", false
	}

	if !comma {
		return "{" + lo + "}", true
	}

	if hi == "" {
		return "{" + lo + ",}", true
	}

	n, err := strconv.Atoi(hi)

	if err != nil || n < m || n > 1000 {
		return "", false
	}

	return "{" + lo + "," + hi + "}", true
}

// translateBracket translates the bracket expression pat starts with and
// returns its length.
func translateBracket(pat []byte, newline bool) (string, int, uint32) {
	var out strings.Builder
	i := 1
	out.WriteString("[")

	if i < len(pat) && pat[i] == '^' {
		out.WriteString("^")
		i++

		if newline {
			out.WriteString(`\n`)
		}
	}

	first := true

	for {
		if i >= len(pat) {
			return "", 0, regEbrack
		}

		c := pat[i]

		if c == ']' && !first {
			break
		}

		first = false

		if c == '[' && i+1 < len(pat) && (pat[i+1] == ':' || pat[i+1] == '=' || pat[i+1] == '.') {
			kind := pat[i+1]
			end := bytes.Index(pat[i+2:], []byte{kind, ']'})

			if end < 0 {
				return "", 0, regEbrack
			}

			body := pat[i+2 : i+2+end]

			if kind == ':' {
				if !validClass(string(body)) {
					return "", 0, 4
				}

				out.WriteString("[:" + string(body) + ":]")
			} else {
				for _, b := range body {
					out.WriteString(`\` + string(b))
				}
			}

			i += end + 4

			continue
		}

		out.WriteString(classByte(c))
		i++

		if i+1 < len(pat) && pat[i] == '-' && pat[i+1] != ']' {
			out.WriteString("-" + classByte(pat[i+1]))
			i += 2
		}
	}

	out.WriteString("]")

	return out.String(), i + 1, 0
}

func classByte(c byte) string {
	if c >= '0' && c <= '9' || c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= 0x80 {
		return string([]byte{c})
	}

	return `\` + string([]byte{c})
}

func validClass(name string) bool {
	switch name {
	case "alnum", "alpha", "blank", "cntrl", "digit", "graph", "lower", "print", "punct", "space", "upper", "xdigit":
		return true
	}

	return false
}

func (p *Process) libcRegcomp(preg, pattern, flags uint32) uint32 {
	pat := bytes.Clone(p.cstr(pattern))
	src, nsub, code := translateRegex(pat, flags, false, false)

	if code != 0 {
		return code
	}

	re, err := regexp.Compile(src)

	if err != nil {
		return regBadpat
	}

	re.Longest()
	p.nextRegex++
	r := &posixRegex{pattern: pat, flags: flags, nsub: nsub}
	r.variants[0] = re
	p.regexes[p.nextRegex] = r
	p.put32(preg, uint32(nsub))
	p.put32(preg+4, p.nextRegex)

	return 0
}

func (p *Process) libcRegexec(preg, s, nmatch, pmatch, eflags uint32) uint32 {
	r, ok := p.regexes[p.u32(preg+4)]

	if !ok {
		return regBadpat
	}

	var subject []byte
	base := uint32(0)

	if eflags&regStartend != 0 {
		so, eo := p.u32(pmatch), p.u32(pmatch+4)

		if eo < so {
			return regBadpat
		}

		subject, base = p.span(s+so, eo-so), so
	} else {
		subject = p.cstr(s)
	}

	boundary := r.flags&regNewline != 0 && eflags&(regNotbol|regNoteol) != 0
	prefix := 0

	if boundary {
		suffix := 0

		if eflags&regNotbol != 0 {
			prefix = 1
		}

		if eflags&regNoteol != 0 {
			suffix = 1
		}

		padded := make([]byte, prefix+len(subject)+suffix)
		copy(padded[prefix:], subject)
		subject = padded
	}

	m := r.variant(eflags).FindSubmatchIndex(subject)

	if m == nil {
		return regNomatch
	}

	if boundary {
		m = m[2:]

		for i, at := range m {
			if at >= 0 {
				m[i] = at - prefix
			}
		}
	}

	if r.flags&regNosub == 0 {
		out := p.span(pmatch, 8*nmatch)

		for i := range int(nmatch) {
			so, eo := int32(-1), int32(-1)

			if 2*i < len(m) && m[2*i] >= 0 {
				so, eo = int32(m[2*i])+int32(base), int32(m[2*i+1])+int32(base)
			}

			binary.LittleEndian.PutUint32(out[8*i:], uint32(so))
			binary.LittleEndian.PutUint32(out[8*i+4:], uint32(eo))
		}
	}

	return 0
}

func (p *Process) libcRegfree(preg uint32) {
	delete(p.regexes, p.u32(preg+4))
}

func (p *Process) libcRegerror(code, preg, buf, size uint32) uint32 {
	msg, ok := regMessages[code]

	if !ok {
		msg = "Unknown error"
	}

	if size > 0 {
		p.putString(buf, truncate(msg, int(size)-1))
	}

	return uint32(len(msg) + 1)
}
