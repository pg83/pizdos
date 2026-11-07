//go:build linux && amd64

package main

import (
	"encoding/binary"
	"fmt"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
)

// Time of the C locale. The zone is the guest's TZ at each call: a zoneinfo
// name, or a POSIX zone whose standard offset is taken; without TZ it is
// the host's.

var zones sync.Map

func zoneOf(tz string) *time.Location {
	if loc, ok := zones.Load(tz); ok {
		return loc.(*time.Location)
	}

	loc := parseZone(tz)
	zones.Store(tz, loc)

	return loc
}

func parseZone(tz string) *time.Location {
	name := strings.TrimPrefix(tz, ":")

	if name == "" {
		return time.UTC
	}

	if loc, err := time.LoadLocation(name); err == nil {
		return loc
	}

	// std offset[dst...]: the name, <quoted> or letters, then hh[:mm[:ss]]
	// west of UTC
	i := 0

	if strings.HasPrefix(name, "<") {
		i = strings.IndexByte(name, '>') + 1

		if i == 0 {
			return time.UTC
		}
	} else {
		for i < len(name) && (name[i] >= 'A' && name[i] <= 'Z' || name[i] >= 'a' && name[i] <= 'z') {
			i++
		}
	}

	std := strings.Trim(name[:i], "<>")
	rest := name[i:]
	sign := 1

	if rest != "" && (rest[0] == '+' || rest[0] == '-') {
		if rest[0] == '-' {
			sign = -1
		}

		rest = rest[1:]
	}

	offset := 0

	for k, unit := range []int{3600, 60, 1} {
		j := 0

		for j < len(rest) && isDigit(rest[j]) {
			j++
		}

		if j == 0 {
			if k == 0 {
				return time.UTC
			}

			break
		}

		n, _ := strconv.Atoi(rest[:j])
		offset += n * unit
		rest = rest[j:]

		if !strings.HasPrefix(rest, ":") {
			break
		}

		rest = rest[1:]
	}

	return time.FixedZone(std, -sign*offset)
}

func (p *Process) location() *time.Location {
	tz, ok := p.getenv("TZ")

	if !ok {
		return time.Local
	}

	return zoneOf(tz)
}

// tmFields is a guest struct tm.
type tmFields struct {
	sec, min, hour, mday, mon, year, wday, yday, isdst, gmtoff int
	zone                                                       string
}

func (p *Process) readTm(at uint32) tmFields {
	b := p.span(at, 44)
	f := func(i int) int {
		return int(int32(binary.LittleEndian.Uint32(b[4*i:])))
	}
	tm := tmFields{f(0), f(1), f(2), f(3), f(4), f(5), f(6), f(7), f(8), f(9), ""}

	if zone := binary.LittleEndian.Uint32(b[40:]); zone != 0 {
		tm.zone = p.str(zone)
	}

	return tm
}

func tmOf(t time.Time) tmFields {
	name, offset := t.Zone()
	tm := tmFields{t.Second(), t.Minute(), t.Hour(), t.Day(), int(t.Month()) - 1, t.Year() - 1900, int(t.Weekday()), t.YearDay() - 1, 0, offset, name}

	if t.IsDST() {
		tm.isdst = 1
	}

	return tm
}

// writeTm writes a time as a struct tm; its zone name is a string of the
// heap the process keeps.
func (p *Process) writeTm(at uint32, t time.Time) {
	tm := tmOf(t)
	b := p.span(at, 44)

	for i, v := range []int{tm.sec, tm.min, tm.hour, tm.mday, tm.mon, tm.year, tm.wday, tm.yday, tm.isdst, tm.gmtoff} {
		binary.LittleEndian.PutUint32(b[4*i:], uint32(int32(v)))
	}

	zone, ok := p.zoneNames[tm.zone]

	if !ok {
		zone = p.newString(tm.zone)
		p.zoneNames[tm.zone] = zone
	}

	p.put32(at+40, zone)
}

func (k *Kernel) timeFunctions() map[string]any {
	return map[string]any{
		"time": func(ctx uintptr, at uint32) uint64 {
			p := proc(ctx)
			now := time.Now().Unix()

			if at != 0 {
				p.put64(at, uint64(now))
			}

			return uint64(now)
		},
		"gettimeofday": func(ctx uintptr, tv, tz uint32) uint32 {
			p := proc(ctx)
			now := time.Now()

			if tv != 0 {
				p.put64(tv, uint64(now.Unix()))
				p.put64(tv+8, uint64(now.Nanosecond()/1000))
			}

			return 0
		},
		"clock_settime": func(ctx uintptr, clock, ts uint32) uint32 {
			return proc(ctx).result(0, syscall.EPERM)
		},
		"localtime": func(ctx uintptr, t uint32) uint32 {
			p := proc(ctx)
			at := p.staticAt(staticTm)
			p.writeTm(at, time.Unix(int64(p.u64(t)), 0).In(p.location()))

			return at
		},
		"localtime_r": func(ctx uintptr, t, at uint32) uint32 {
			p := proc(ctx)
			p.writeTm(at, time.Unix(int64(p.u64(t)), 0).In(p.location()))

			return at
		},
		"mktime": func(ctx uintptr, at uint32) uint64 {
			p := proc(ctx)
			tm := p.readTm(at)
			t := time.Date(tm.year+1900, time.Month(tm.mon+1), tm.mday, tm.hour, tm.min, tm.sec, 0, p.location())
			p.writeTm(at, t)

			return uint64(t.Unix())
		},
		"ctime": func(ctx uintptr, t uint32) uint32 {
			p := proc(ctx)
			at := p.staticAt(staticCtime)
			p.putString(at, time.Unix(int64(p.u64(t)), 0).In(p.location()).Format("Mon Jan _2 15:04:05 2006\n"))

			return at
		},
		"strftime": func(ctx uintptr, s, max, format, tm uint32) uint32 {
			p := proc(ctx)
			out := strftime(string(p.cstr(format)), p.readTm(tm))

			if uint32(len(out)) >= max {
				return 0
			}

			p.putString(s, out)

			return uint32(len(out))
		},
		"strptime": func(ctx uintptr, s, format, tm uint32) uint32 {
			p := proc(ctx)
			fields := p.readTm(tm)
			n, ok := strptime(p.cstr(s), string(p.cstr(format)), &fields)

			if !ok {
				return 0
			}

			b := p.span(tm, 36)

			for i, v := range []int{fields.sec, fields.min, fields.hour, fields.mday, fields.mon, fields.year, fields.wday, fields.yday, fields.isdst} {
				binary.LittleEndian.PutUint32(b[4*i:], uint32(int32(v)))
			}

			return s + uint32(n)
		},
		"times": func(ctx uintptr, at uint32) uint64 {
			p := proc(ctx)
			var t syscall.Tms
			ticks, err := syscall.Times(&t)

			if err != nil {
				return p.result64(0, err)
			}

			if at != 0 {
				for i, v := range []int64{t.Utime, t.Stime, t.Cutime, t.Cstime} {
					p.put64(at+uint32(8*i), uint64(v))
				}
			}

			return p.result64(int64(ticks), nil)
		},
		"sleep": func(ctx uintptr, seconds uint32) uint32 {
			p := proc(ctx)
			left := p.sleep(time.Duration(seconds) * time.Second)
			p.deliver()

			return uint32((left + time.Second - 1) / time.Second)
		},
	}
}

// sleep waits for d or an interrupting signal and returns what is left.
func (p *Process) sleep(d time.Duration) time.Duration {
	end := time.Now().Add(d)

	for {
		left := time.Until(end)

		if left <= 0 {
			return 0
		}

		ms := min(left.Milliseconds()+1, 1<<30)

		if _, err := p.waitFds(nil, int(ms), false); err != nil {
			return max(time.Until(end), 0)
		}
	}
}

var weekdays = []string{"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"}
var months = []string{"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"}

func calendarName(list []string, i int, short bool) string {
	if i < 0 || i >= len(list) {
		return "?"
	}

	if short {
		return list[i][:3]
	}

	return list[i]
}

// strftime formats a struct tm, with glibc's flags _ - 0 ^ and widths.
func strftime(format string, tm tmFields) string {
	var out strings.Builder

	for i := 0; i < len(format); i++ {
		c := format[i]

		if c != '%' || i+1 == len(format) {
			out.WriteByte(c)

			continue
		}

		i++
		pad := byte(0)
		up := false

		for ; i < len(format) && strings.IndexByte("_-0^#", format[i]) >= 0; i++ {
			switch format[i] {
			case '_':
				pad = ' '
			case '-':
				pad = '-'
			case '0':
				pad = '0'
			case '^':
				up = true
			}
		}

		width := 0

		for ; i < len(format) && isDigit(format[i]); i++ {
			width = width*10 + int(format[i]-'0')
		}

		for i < len(format) && (format[i] == 'E' || format[i] == 'O') {
			i++
		}

		if i == len(format) {
			break
		}

		s := conversion(format[i], tm, pad, width)

		if up {
			s = strings.ToUpper(s)
		}

		out.WriteString(s)
	}

	return out.String()
}

// number formats a number in at least width digits, padded with def or the
// flag's pad.
func number(v, width int, def, pad byte) string {
	if pad == '-' {
		return strconv.Itoa(v)
	}

	if pad != 0 {
		def = pad
	}

	s := strconv.Itoa(v)

	for len(s) < width {
		s = string(def) + s
	}

	return s
}

func conversion(c byte, tm tmFields, pad byte, width int) string {
	num := func(v, w int, def byte) string {
		return number(v, max(w, width), def, pad)
	}
	hour12 := tm.hour % 12

	if hour12 == 0 {
		hour12 = 12
	}

	isoYear, isoWeek := time.Date(tm.year+1900, time.January, 1+tm.yday, 12, 0, 0, 0, time.UTC).ISOWeek()
	str := func(s string) string {
		for len(s) < width {
			s = " " + s
		}

		return s
	}

	switch c {
	case 'a':
		return str(calendarName(weekdays, tm.wday, true))
	case 'A':
		return str(calendarName(weekdays, tm.wday, false))
	case 'b', 'h':
		return str(calendarName(months, tm.mon, true))
	case 'B':
		return str(calendarName(months, tm.mon, false))
	case 'c':
		return strftime("%a %b %e %H:%M:%S %Y", tm)
	case 'C':
		return num((tm.year+1900)/100, 2, '0')
	case 'd':
		return num(tm.mday, 2, '0')
	case 'D', 'x':
		return strftime("%m/%d/%y", tm)
	case 'e':
		return num(tm.mday, 2, ' ')
	case 'F':
		return strftime("%Y-%m-%d", tm)
	case 'g':
		return num(isoYear%100, 2, '0')
	case 'G':
		return num(isoYear, 1, '0')
	case 'H':
		return num(tm.hour, 2, '0')
	case 'I':
		return num(hour12, 2, '0')
	case 'j':
		return num(tm.yday+1, 3, '0')
	case 'k':
		return num(tm.hour, 2, ' ')
	case 'l':
		return num(hour12, 2, ' ')
	case 'm':
		return num(tm.mon+1, 2, '0')
	case 'M':
		return num(tm.min, 2, '0')
	case 'n':
		return "\n"
	case 'p':
		if tm.hour < 12 {
			return "AM"
		}

		return "PM"
	case 'P':
		if tm.hour < 12 {
			return "am"
		}

		return "pm"
	case 'r':
		return strftime("%I:%M:%S %p", tm)
	case 'R':
		return strftime("%H:%M", tm)
	case 's':
		t := time.Date(tm.year+1900, time.Month(tm.mon+1), tm.mday, tm.hour, tm.min, tm.sec, 0, time.UTC)

		return num(int(t.Unix())-tm.gmtoff, 1, '0')
	case 'S':
		return num(tm.sec, 2, '0')
	case 't':
		return "\t"
	case 'T', 'X':
		return strftime("%H:%M:%S", tm)
	case 'u':
		return num((tm.wday+6)%7+1, 1, '0')
	case 'U':
		return num((tm.yday+7-tm.wday)/7, 2, '0')
	case 'V':
		return num(isoWeek, 2, '0')
	case 'w':
		return num(tm.wday, 1, '0')
	case 'W':
		return num((tm.yday+7-(tm.wday+6)%7)/7, 2, '0')
	case 'y':
		return num((tm.year+1900)%100, 2, '0')
	case 'Y':
		return num(tm.year+1900, 1, '0')
	case 'z':
		off := tm.gmtoff
		sign := '+'

		if off < 0 {
			sign, off = '-', -off
		}

		return fmt.Sprintf("%c%02d%02d", sign, off/3600, off/60%60)
	case 'Z':
		return str(tm.zone)
	case '%':
		return "%"
	}

	return "%" + string(c)
}

// strptime parses s by format into the fields it names and returns how much
// of s it took.
func strptime(s []byte, format string, tm *tmFields) (int, bool) {
	at := 0
	space := func() {
		for at < len(s) && (s[at] == ' ' || s[at] >= '\t' && s[at] <= '\r') {
			at++
		}
	}
	num := func(digits, lo, hi int) (int, bool) {
		space()
		sign := 1

		if at < len(s) && (s[at] == '-' || s[at] == '+') && digits > 4 {
			if s[at] == '-' {
				sign = -1
			}

			at++
		}

		start, v := at, 0

		for at < len(s) && at-start < digits && isDigit(s[at]) {
			v = v*10 + int(s[at]-'0')
			at++
		}

		v *= sign

		return v, at > start && v >= lo && v <= hi
	}
	word := func(list []string) (int, bool) {
		space()

		for i, full := range list {
			for _, w := range []string{full, full[:3]} {
				if at+len(w) <= len(s) && strings.EqualFold(string(s[at:at+len(w)]), w) {
					at += len(w)

					return i, true
				}
			}
		}

		return 0, false
	}

	for i := 0; i < len(format); i++ {
		c := format[i]

		if c == ' ' || c >= '\t' && c <= '\r' {
			space()

			continue
		}

		if c != '%' || i+1 == len(format) {
			if at >= len(s) || s[at] != c {
				return 0, false
			}

			at++

			continue
		}

		i++

		for i < len(format) && (format[i] == 'E' || format[i] == 'O') {
			i++
		}

		var ok bool
		var v int

		switch format[i] {
		case 'Y':
			v, ok = num(4, 0, 9999)
			tm.year = v - 1900
		case 'y':
			v, ok = num(2, 0, 99)
			tm.year = v

			if v < 69 {
				tm.year += 100
			}
		case 'C':
			v, ok = num(2, 0, 99)
			tm.year = v*100 - 1900 + tm.year%100
		case 'm':
			v, ok = num(2, 1, 12)
			tm.mon = v - 1
		case 'd', 'e':
			v, ok = num(2, 1, 31)
			tm.mday = v
		case 'H', 'k':
			v, ok = num(2, 0, 23)
			tm.hour = v
		case 'I', 'l':
			v, ok = num(2, 1, 12)
			tm.hour = v % 12
		case 'M':
			v, ok = num(2, 0, 59)
			tm.min = v
		case 'S':
			v, ok = num(2, 0, 61)
			tm.sec = v
		case 'j':
			v, ok = num(3, 1, 366)
			tm.yday = v - 1
		case 'b', 'B', 'h':
			v, ok = word(months)
			tm.mon = v
		case 'a', 'A':
			v, ok = word(weekdays)
			tm.wday = v
		case 'p':
			space()
			ok = at+2 <= len(s)

			if ok {
				switch strings.ToUpper(string(s[at : at+2])) {
				case "AM":
				case "PM":
					tm.hour += 12
				default:
					ok = false
				}

				at += 2
			}
		case 's':
			v, ok = num(20, -1<<62, 1<<62)
			*tm = tmOf(time.Unix(int64(v), 0))
		case 'n', 't':
			space()
			ok = true
		case 'D':
			n, good := strptime(s[at:], "%m/%d/%y", tm)
			at, ok = at+n, good
		case 'T':
			n, good := strptime(s[at:], "%H:%M:%S", tm)
			at, ok = at+n, good
		case 'R':
			n, good := strptime(s[at:], "%H:%M", tm)
			at, ok = at+n, good
		case 'F':
			n, good := strptime(s[at:], "%Y-%m-%d", tm)
			at, ok = at+n, good
		case '%':
			ok = at < len(s) && s[at] == '%'
			at++
		}

		if !ok {
			return 0, false
		}
	}

	return at, true
}
