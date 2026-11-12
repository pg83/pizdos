//go:build linux && amd64

package main

import (
	"encoding/binary"
	"fmt"
	"math"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
	"unsafe"
)

// clockids of the guest's time.h
const (
	clockRealtime       = 0
	clockMonotonic      = 1
	clockProcessCputime = 2
	clockThreadCputime  = 3
	clockBoottime       = 7
	timerAbstime        = 1
)

// hostClock is the host's clock of a guest's. The processes of vm have
// no CPU clocks of their own: both CPU clocks are vm's.
func hostClock(clock uint32) (uintptr, bool) {
	switch {
	case clock == clockThreadCputime:
		return clockProcessCputime, true
	case clock <= clockBoottime:
		return uintptr(clock), true
	}

	return 0, false
}

func clockGettime(clock uintptr) time.Duration {
	var t syscall.Timespec
	syscall.Syscall(syscall.SYS_CLOCK_GETTIME, clock, uintptr(unsafe.Pointer(&t)), 0)

	return time.Duration(t.Sec)*time.Second + time.Duration(t.Nsec)
}

func (p *Process) putTimespec(at uint32, d time.Duration) {
	p.put64(at, uint64(d/time.Second))
	p.put32(at+8, uint32(d%time.Second))
}

// timespec reads a struct timespec as a duration, which it bounds.
func (p *Process) timespec(at uint32) (time.Duration, error) {
	sec, nsec := int64(p.u64(at)), int64(int32(p.u32(at+8)))

	if sec < 0 || nsec < 0 || nsec >= 1e9 {
		return 0, syscall.EINVAL
	}

	return time.Duration(min(sec, 1<<33))*time.Second + time.Duration(nsec), nil
}

// timeval reads a struct timeval as a duration, which it bounds.
func (p *Process) timeval(at uint32) (time.Duration, error) {
	sec, usec := int64(p.u64(at)), int64(p.u64(at+8))

	if sec < 0 || usec < 0 || usec >= 1e6 {
		return 0, syscall.EINVAL
	}

	return time.Duration(min(sec, 1<<33))*time.Second + time.Duration(usec)*time.Microsecond, nil
}

func (p *Process) putTimeval(at uint32, d time.Duration) {
	p.put64(at, uint64(d/time.Second))
	p.put64(at+8, uint64(d%time.Second/time.Microsecond))
}

// The interval timers of setitimer: each sends its signal when its value
// passes, then every interval. All of them count real time, vm having
// no CPU clocks of the processes. They live through exec; a child has none.
const (
	itimerReal = 0
	itimerProf = 2
)

var itimerSignals = [3]int{int(syscall.SIGALRM), int(syscall.SIGVTALRM), int(syscall.SIGPROF)}

type itimers struct {
	mu sync.Mutex
	t  [3]itimer
}

// itimer is armed while timer is not nil, to fire at end; gen tells a
// firing that it was set again meanwhile.
type itimer struct {
	timer    *time.Timer
	end      time.Time
	interval time.Duration
	gen      uint64
}

// left is what remains of an armed timer, at least a microsecond, or 0.
func (t *itimer) left() time.Duration {
	if t.timer == nil {
		return 0
	}

	return max(time.Until(t.end), time.Microsecond)
}

// setTimer arms a timer, or disarms it for a zero value, and returns what
// was left of it and its interval.
func (p *Process) setTimer(which int, value, interval time.Duration) (time.Duration, time.Duration) {
	ts := &p.timers
	ts.mu.Lock()
	defer ts.mu.Unlock()

	t := &ts.t[which]
	left, every := t.left(), t.interval

	if t.timer != nil {
		t.timer.Stop()
	}

	t.gen++
	t.timer, t.interval = nil, 0

	if value > 0 {
		t.end, t.interval = time.Now().Add(value), interval
		gen := t.gen
		t.timer = time.AfterFunc(value, func() { p.timerFired(which, gen) })
	}

	return left, every
}

func (p *Process) timerFired(which int, gen uint64) {
	ts := &p.timers
	ts.mu.Lock()
	t := &ts.t[which]

	if t.gen != gen {
		ts.mu.Unlock()

		return
	}

	if t.interval > 0 {
		t.end = t.end.Add(t.interval)

		if now := time.Now(); t.end.Before(now) {
			t.end = now.Add(t.interval)
		}

		t.timer = time.AfterFunc(time.Until(t.end), func() { p.timerFired(which, gen) })
	} else {
		t.timer = nil
	}

	ts.mu.Unlock()
	p.send(itimerSignals[which])
}

// stopTimers disarms the timers of an ending process.
func (p *Process) stopTimers() {
	ts := &p.timers
	ts.mu.Lock()
	defer ts.mu.Unlock()

	for i := range ts.t {
		if ts.t[i].timer != nil {
			ts.t[i].timer.Stop()
		}

		ts.t[i].gen++
		ts.t[i].timer = nil
	}
}

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

// tzset sets tzname, timezone and daylight for the zone: its standard
// time's name and offset, and its daylight saving time's name, as this
// year has them.
func (p *Process) tzset() {
	if p.tzAt[0] == 0 {
		return
	}

	loc := p.location()
	year := time.Now().In(loc).Year()
	std, stdOffset := time.Date(year, time.January, 1, 0, 0, 0, 0, loc).Zone()
	dst, dstOffset := time.Date(year, time.July, 1, 0, 0, 0, 0, loc).Zone()

	if time.Date(year, time.January, 1, 0, 0, 0, 0, loc).IsDST() {
		std, stdOffset, dst, dstOffset = dst, dstOffset, std, stdOffset
	}

	p.put32(p.tzAt[0], p.constant(std))
	p.put32(p.tzAt[0]+4, p.constant(dst))
	p.put32(p.tzAt[1], uint32(int32(-stdOffset)))
	p.put32(p.tzAt[2], bit(stdOffset != dstOffset))
}

// asctime is the 26 characters of asctime of a struct tm, or false when the
// year needs more.
func asctime(tm tmFields) (string, bool) {
	s := fmt.Sprintf("%.3s %.3s%3d %.2d:%.2d:%.2d %d\n", calendarName(weekdays, tm.wday, true), calendarName(months, tm.mon, true),
		tm.mday, tm.hour, tm.min, tm.sec, tm.year+1900)

	return s, len(s) < 26
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

// writeTm writes a representable struct tm; its zone name is a constant.
func (p *Process) writeTm(at uint32, t time.Time) bool {
	tm := tmOf(t)

	if tm.year != int(int32(tm.year)) {
		return false
	}

	b := p.span(at, 44)

	for i, v := range []int{tm.sec, tm.min, tm.hour, tm.mday, tm.mon, tm.year, tm.wday, tm.yday, tm.isdst, tm.gmtoff} {
		binary.LittleEndian.PutUint32(b[4*i:], uint32(int32(v)))
	}

	p.put32(at+40, p.constant(tm.zone))

	return true
}

func (p *Process) libcTime(at uint32) uint64 {
	now := time.Now().Unix()

	if at != 0 {
		p.put64(at, uint64(now))
	}

	return uint64(now)
}

func (p *Process) libcGettimeofday(tv, tz uint32) uint32 {
	now := time.Now()

	if tv != 0 {
		p.put64(tv, uint64(now.Unix()))
		p.put64(tv+8, uint64(now.Nanosecond()/1000))
	}

	return 0
}

func (p *Process) libcLocaltime(t uint32) uint32 {
	return p.localtime(t, p.staticAt(staticTm))
}

func (p *Process) libcLocaltime_r(t, at uint32) uint32 {
	return p.localtime(t, at)
}

func (p *Process) localtime(t, at uint32) uint32 {
	p.tzset()

	if !p.writeTm(at, time.Unix(int64(p.u64(t)), 0).In(p.location())) {
		p.setErrno(syscall.EOVERFLOW)

		return 0
	}

	return at
}

func (p *Process) libcAsctime(tm uint32) uint32 {
	s, ok := asctime(p.readTm(tm))

	if !ok {
		p.setErrno(syscall.EOVERFLOW)

		return 0
	}

	at := p.staticAt(staticCtime)
	p.putString(at, s)

	return at
}

func (p *Process) libcAsctime_r(tm, buf uint32) uint32 {
	s, ok := asctime(p.readTm(tm))

	if !ok {
		p.setErrno(syscall.EOVERFLOW)

		return 0
	}

	p.putString(buf, s)

	return buf
}

func (p *Process) libcCtime_r(t, buf uint32) uint32 {
	s, ok := asctime(tmOf(time.Unix(int64(p.u64(t)), 0).In(p.location())))

	if !ok {
		p.setErrno(syscall.EOVERFLOW)

		return 0
	}

	p.putString(buf, s)

	return buf
}

func (p *Process) libcDifftime(t1, t0 uint64) float64 {
	return float64(int64(t1)) - float64(int64(t0))
}

// timegm is mktime in UTC: it normalizes the struct tm.
func (p *Process) libcTimegm(at uint32) uint64 {
	tm := p.readTm(at)
	t := time.Date(tm.year+1900, time.Month(tm.mon+1), tm.mday, tm.hour, tm.min, tm.sec, 0, time.UTC)

	if !p.writeTm(at, t) {
		p.setErrno(syscall.EOVERFLOW)

		return math.MaxUint64
	}

	return uint64(t.Unix())
}

func (p *Process) libcUsleep(us uint32) uint32 {
	left := p.sleep(time.Duration(us) * time.Microsecond)
	p.deliver()

	if left > 0 {
		return p.result(0, syscall.EINTR)
	}

	return p.result(0, nil)
}

func (p *Process) libcUalarm(value, interval uint32) uint32 {
	left, _ := p.setTimer(itimerReal, time.Duration(value)*time.Microsecond, time.Duration(interval)*time.Microsecond)

	return uint32(min(left/time.Microsecond, math.MaxUint32))
}

func (p *Process) libcGmtime(t uint32) uint32 {
	return p.gmtime(t, p.staticAt(staticTm))
}

func (p *Process) libcClock() uint64 {
	return uint64(clockGettime(clockProcessCputime) / time.Microsecond)
}

func (p *Process) libcClock_getres(clock, res uint32) uint32 {
	host, ok := hostClock(clock)

	if !ok {
		return p.result(0, syscall.EINVAL)
	}

	var t syscall.Timespec
	syscall.Syscall(syscall.SYS_CLOCK_GETRES, host, uintptr(unsafe.Pointer(&t)), 0)

	if res != 0 {
		p.put64(res, uint64(t.Sec))
		p.put32(res+8, uint32(t.Nsec))
	}

	return p.result(0, nil)
}

// clock_nanosleep returns an error number; it sleeps on the real time
// clocks.
func (p *Process) libcClock_nanosleep(clock, flags, req, rem uint32) uint32 {
	switch clock {
	case clockRealtime, clockMonotonic, clockBoottime:
	case clockProcessCputime:
		return uint32(syscall.ENOTSUP)
	default:
		return uint32(syscall.EINVAL)
	}

	d, err := p.timespec(req)

	if err != nil {
		return uint32(syscall.EINVAL)
	}

	if flags&timerAbstime != 0 {
		d -= clockGettime(uintptr(clock))
	}

	left := p.sleep(d)
	p.deliver()

	if left > 0 {
		if rem != 0 && flags&timerAbstime == 0 {
			p.putTimespec(rem, left)
		}

		return uint32(syscall.EINTR)
	}

	return 0
}

func (p *Process) libcSetitimer(which, value, old uint32) uint32 {
	if which > itimerProf {
		return p.result(0, syscall.EINVAL)
	}

	interval, err := p.timeval(value)

	if err != nil {
		return p.result(0, err)
	}

	v, err := p.timeval(value + 16)

	if err != nil {
		return p.result(0, err)
	}

	left, every := p.setTimer(int(which), v, interval)

	if old != 0 {
		p.putTimeval(old, every)
		p.putTimeval(old+16, left)
	}

	return p.result(0, nil)
}

func (p *Process) libcGetitimer(which, at uint32) uint32 {
	if which > itimerProf {
		return p.result(0, syscall.EINVAL)
	}

	p.timers.mu.Lock()
	t := &p.timers.t[which]
	left, every := t.left(), t.interval
	p.timers.mu.Unlock()
	p.putTimeval(at, every)
	p.putTimeval(at+16, left)

	return p.result(0, nil)
}

func (p *Process) libcAlarm(seconds uint32) uint32 {
	left, _ := p.setTimer(itimerReal, time.Duration(seconds)*time.Second, 0)

	return uint32((left + time.Second - 1) / time.Second)
}

func (p *Process) libcMktime(at uint32) uint64 {
	p.tzset()
	tm := p.readTm(at)
	t := time.Date(tm.year+1900, time.Month(tm.mon+1), tm.mday, tm.hour, tm.min, tm.sec, 0, p.location())

	if !p.writeTm(at, t) {
		p.setErrno(syscall.EOVERFLOW)

		return math.MaxUint64
	}

	return uint64(t.Unix())
}

func (p *Process) libcCtime(t uint32) uint32 {
	return p.libcCtime_r(t, p.staticAt(staticCtime))
}

func (p *Process) libcStrftime(s, max, format, tm uint32) uint32 {
	out := strftime(string(p.cstr(format)), p.readTm(tm))

	if uint32(len(out)) >= max {
		return 0
	}

	p.putString(s, out)

	return uint32(len(out))
}

func (p *Process) libcStrptime(s, format, tm uint32) uint32 {
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
}

func (p *Process) libcTimes(at uint32) uint64 {
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
}

func (p *Process) libcSleep(seconds uint32) uint32 {
	left := p.sleep(time.Duration(seconds) * time.Second)
	p.deliver()

	return uint32((left + time.Second - 1) / time.Second)
}

func (p *Process) libcClock_gettime(clock, ts uint32) uint32 {
	host, ok := hostClock(clock)

	if !ok {
		return p.result(0, syscall.EINVAL)
	}

	var t syscall.Timespec
	syscall.Syscall(syscall.SYS_CLOCK_GETTIME, host, uintptr(unsafe.Pointer(&t)), 0)
	p.put64(ts, uint64(t.Sec))
	p.put32(ts+8, uint32(t.Nsec))

	return p.result(0, nil)
}

func (p *Process) libcNanosleep(req, rem uint32) uint32 {
	d, err := p.timespec(req)

	if err != nil {
		return p.result(0, err)
	}

	left := p.sleep(d)
	p.deliver()

	if left > 0 {
		if rem != 0 {
			p.put64(rem, uint64(left/time.Second))
			p.put32(rem+8, uint32(left%time.Second/time.Nanosecond))
		}

		return p.result(0, syscall.EINTR)
	}

	return p.result(0, nil)
}

// gmtime writes a time as UTC's struct tm, or fails with EOVERFLOW when
// its year is no int.
func (p *Process) gmtime(t, at uint32) uint32 {
	sec := int64(p.u64(t))
	u := time.Unix(sec, 0).UTC()

	if !p.writeTm(at, u) {
		p.setErrno(syscall.EOVERFLOW)

		return 0
	}

	return at
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

		if p.idle(int(ms), false) != nil {
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
	century := -1
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

		if i == len(format) {
			return 0, false
		}

		var ok bool
		var v int

		switch format[i] {
		case 'Y':
			v, ok = num(4, 0, 9999)
			tm.year = v - 1900
			century = -1
		case 'y':
			v, ok = num(2, 0, 99)
			tm.year = v

			if v < 69 {
				tm.year += 100
			}
		case 'C':
			v, ok = num(2, 0, 99)
			century = v
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

	if century >= 0 {
		tm.year = century*100 - 1900 + (tm.year%100+100)%100
	}

	return at, true
}

func (p *Process) libcClock_settime(clock, ts uint32) uint32 {
	return p.result(0, syscall.EPERM)
}

func (p *Process) libcTzset() {
	p.tzset()
}

// The clock is the host's, which no process may set.
func (p *Process) libcSettimeofday(tv, tz uint32) uint32 {
	return p.result(0, syscall.EPERM)
}

func (p *Process) libcStime(t uint32) uint32 {
	return p.result(0, syscall.EPERM)
}

func (p *Process) libcAdjtime(delta, old uint32) uint32 {
	return p.result(0, syscall.EPERM)
}

func (p *Process) libcGmtime_r(t, at uint32) uint32 {
	return p.gmtime(t, at)
}
