//go:build linux && amd64

package main

import (
	"strings"
	"unicode"
)

// Locales and character classes. The multibyte functions of lib/c are
// UTF-8's in every locale, so a locale is a name per category and nothing
// more: its codeset is UTF-8 and its items are the C locale's. Names follow
// musl: "" takes the environment's, C and POSIX are the C locale, any
// other name of at most 23 bytes without a slash or a leading dot is kept,
// and an invalid one is C.UTF-8, which only LC_CTYPE keeps by that name.
// Unlike musl's, a name of another codeset is refused, as one the system
// does not have.

const (
	lcCtype    = 0
	lcNumeric  = 1
	lcTime     = 2
	lcMonetary = 4
	lcMessages = 5
	lcAll      = 6
	// localeNameMax is musl's LOCALE_NAME_MAX.
	localeNameMax = 23
)

var localeVars = [lcAll]string{"LC_CTYPE", "LC_NUMERIC", "LC_TIME", "LC_COLLATE", "LC_MONETARY", "LC_MESSAGES"}

// utf8Codeset tells whether a locale name's codeset, between its dot and
// any @modifier, is UTF-8's, or it names none.
func utf8Codeset(name string) bool {
	_, codeset, ok := strings.Cut(name, ".")

	if !ok {
		return true
	}

	codeset, _, _ = strings.Cut(codeset, "@")
	codeset = strings.ToLower(strings.ReplaceAll(codeset, "-", ""))

	return codeset == "utf8"
}

// localeName is the locale a category takes for a name: "" for C.
func (p *Process) localeName(cat int, name string) string {
	if name == "" {
		for _, v := range []string{"LC_ALL", localeVars[cat], "LANG"} {
			if s, ok := p.getenv(v); ok && s != "" {
				name = s

				break
			}
		}
	}

	if name == "" || len(name) > localeNameMax || name[0] == '.' || strings.Contains(name, "/") {
		name = "C.UTF-8"
	}

	switch {
	case name == "C" || name == "POSIX":
		return ""
	case name == "C.UTF-8" && cat != lcCtype:
		return ""
	}

	return name
}

func localeShown(name string) string {
	if name == "" {
		return "C"
	}

	return name
}

// setlocale sets a category's locale, or all of them from one name or
// from the "a;b;c;d;e;f" a query of LC_ALL returns when they differ, and
// returns the name.
func (p *Process) setlocale(cat uint32, name uint32) uint32 {
	if cat > lcAll {
		return 0
	}

	if name != 0 {
		s := p.str(name)
		parts := strings.Split(s, ";")
		var names [lcAll]string

		for i := range lcAll {
			part := s

			if cat == lcAll {
				part = parts[min(i, len(parts)-1)]
			} else if i != int(cat) {
				continue
			}

			names[i] = p.localeName(i, part)

			if !utf8Codeset(names[i]) {
				return 0
			}
		}

		for i := range lcAll {
			if cat == lcAll || i == int(cat) {
				p.locale[i] = names[i]
			}
		}
	}

	if cat < lcAll {
		return p.constant(localeShown(p.locale[cat]))
	}

	names := make([]string, lcAll)

	for i, n := range p.locale {
		names[i] = localeShown(n)
	}

	for _, n := range names {
		if n != names[0] {
			return p.constant(strings.Join(names, ";"))
		}
	}

	return p.constant(names[0])
}

// The items of nl_langinfo are a category and an index.
var langItems = map[int][]string{
	lcNumeric: {".", ""},
	lcTime: {
		"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat",
		"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
		"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
		"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December",
		"AM", "PM", "%a %b %e %T %Y", "%m/%d/%y", "%H:%M:%S", "%I:%M:%S %p",
		"", "", "%m/%d/%y", "0123456789", "%a %b %e %T %Y", "%H:%M:%S",
	},
	lcMessages: {"^[yY]", "^[nN]", "yes", "no"},
}

const (
	codeset = 14
	// localeNameItem is the index of musl's _NL_LOCALE_NAME.
	localeNameItem = 0xffff
)

func (p *Process) langinfo(item uint32) string {
	cat, idx := int(item>>16), int(item&0xffff)

	switch {
	case item == codeset:
		return "UTF-8"
	case idx == localeNameItem && cat < lcAll:
		return localeShown(p.locale[cat])
	case idx < len(langItems[cat]):
		return langItems[cat][idx]
	}

	return ""
}

// lconv offsets of the guest's locale.h: ten strings, then the chars.
const (
	lconvStrings = 10
	lconvChars   = 14
	charMax      = 127
)

func (p *Process) localeconv() uint32 {
	at := p.staticAt(staticLconv)
	p.put32(at, p.staticAt(staticDot))

	for i := uint32(1); i < lconvStrings; i++ {
		p.put32(at+4*i, p.staticAt(staticEmpty))
	}

	chars := p.span(at+4*lconvStrings, lconvChars)

	for i := range chars {
		chars[i] = charMax
	}

	return at
}

// Wide character classes, by number as wctype returns them.
const (
	wcAlnum = 1 + iota
	wcAlpha
	wcBlank
	wcCntrl
	wcDigit
	wcGraph
	wcLower
	wcPrint
	wcPunct
	wcSpace
	wcUpper
	wcXdigit
)

var wcClassNames = []string{"alnum", "alpha", "blank", "cntrl", "digit", "graph", "lower", "print", "punct", "space", "upper", "xdigit"}

// noBreakSpace tells the spaces that are not where a line breaks, which
// are no blanks nor spaces.
func noBreakSpace(c rune) bool {
	return c == 0xa0 || c == 0x2007 || c == 0x202f
}

func towlower(c uint32) uint32 {
	if c > unicode.MaxRune {
		return c
	}

	return uint32(unicode.ToLower(rune(c)))
}

func towupper(c uint32) uint32 {
	if c > unicode.MaxRune {
		return c
	}

	return uint32(unicode.ToUpper(rune(c)))
}

// iswprint is musl's: everything but controls, separators of lines,
// surrogates and noncharacters.
func iswprint(c uint32) bool {
	switch {
	case c < 0xff:
		return (c+1)&0x7f >= 0x21
	case c < 0x2028 || c-0x202a < 0xd800-0x202a || c-0xe000 < 0xfff9-0xe000:
		return true
	case c-0xfffc > 0x10ffff-0xfffc || c&0xfffe == 0xfffe:
		return false
	}

	return true
}

// iswClass tells whether a wide character is in a class: alphabetic by
// Unicode's property, digits only the ASCII ones, spaces and blanks those
// a line breaks at, cased by the case mappings, others as musl's.
func iswClass(class int, c uint32) bool {
	r := rune(c)
	valid := c <= unicode.MaxRune

	switch class {
	case wcAlnum:
		return c-'0' < 10 || valid && unicode.In(r, unicode.L, unicode.Nl, unicode.Other_Alphabetic)
	case wcAlpha:
		return valid && unicode.In(r, unicode.L, unicode.Nl, unicode.Other_Alphabetic)
	case wcBlank:
		return c == '\t' || valid && unicode.Is(unicode.Zs, r) && !noBreakSpace(r)
	case wcCntrl:
		return c < 32 || c-0x7f < 33 || c-0x2028 < 2 || c-0xfff9 < 3
	case wcDigit:
		return c-'0' < 10
	case wcGraph:
		return iswprint(c) && !iswClass(wcSpace, c)
	case wcLower:
		return towupper(c) != c || c == 0xdf
	case wcPrint:
		return iswprint(c)
	case wcPunct:
		return iswprint(c) && !iswClass(wcSpace, c) && !iswClass(wcAlnum, c)
	case wcSpace:
		return c != 0 && valid && unicode.Is(unicode.White_Space, r) && !noBreakSpace(r)
	case wcUpper:
		return towlower(c) != c
	case wcXdigit:
		return c-'0' < 10 || c|32-'a' < 6
	}

	return false
}

// ctypeClass is a class of the C locale's ctype functions: only ASCII is
// in one.
func ctypeClass(class string) func(uintptr, uint32) uint32 {
	return func(ctx uintptr, c uint32) uint32 {
		if c < 0x80 && inClass(class, byte(c)) {
			return 1
		}

		return 0
	}
}

func bit(b bool) uint32 {
	if b {
		return 1
	}

	return 0
}

func (k *Kernel) localeFunctions() map[string]any {
	t := map[string]any{
		"setlocale": func(ctx uintptr, cat, name uint32) uint32 {
			return proc(ctx).setlocale(cat, name)
		},
		"localeconv": func(ctx uintptr) uint32 {
			return proc(ctx).localeconv()
		},
		"nl_langinfo": func(ctx uintptr, item uint32) uint32 {
			p := proc(ctx)

			return p.constant(p.langinfo(item))
		},
		"isascii": func(ctx uintptr, c uint32) uint32 {
			return bit(c < 0x80)
		},
		"toascii": func(ctx uintptr, c uint32) uint32 {
			return c & 0x7f
		},
		"towlower": func(ctx uintptr, c uint32) uint32 {
			return towlower(c)
		},
		"towupper": func(ctx uintptr, c uint32) uint32 {
			return towupper(c)
		},
		"wctype": func(ctx uintptr, name uint32) uint32 {
			s := proc(ctx).str(name)

			for i, n := range wcClassNames {
				if n == s {
					return uint32(i + 1)
				}
			}

			return 0
		},
		"iswctype": func(ctx uintptr, c, class uint32) uint32 {
			return bit(iswClass(int(class), c))
		},
		"wctrans": func(ctx uintptr, name uint32) uint32 {
			switch proc(ctx).str(name) {
			case "toupper":
				return 1
			case "tolower":
				return 2
			}

			return 0
		},
		"towctrans": func(ctx uintptr, c, trans uint32) uint32 {
			switch trans {
			case 1:
				return towupper(c)
			case 2:
				return towlower(c)
			}

			return c
		},
	}

	for _, class := range []string{"alnum", "alpha", "blank", "cntrl", "graph", "lower", "print", "punct", "upper", "xdigit"} {
		t["is"+class] = ctypeClass(class)
	}

	for i, name := range wcClassNames {
		t["isw"+name] = func(ctx uintptr, c uint32) uint32 {
			return bit(iswClass(i+1, c))
		}
	}

	return t
}
