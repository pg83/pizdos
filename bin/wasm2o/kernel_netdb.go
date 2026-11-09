//go:build linux && amd64

package main

import (
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"net"
	"net/netip"
	"strconv"
	"strings"
	"syscall"
	"time"
)

// Names of hosts and services. A host is looked up in the root's /etc/hosts,
// then by Go's resolver, which is the host's: its /etc/hosts and DNS. A
// service is the root's /etc/services' or, without that file, one of a few
// well known; protocols are musl's table. The results of the functions
// without _r are heap blocks the static area points at, as the user
// databases' (kernel_users.go). The inet functions are musl's.

const (
	aiPassive     = 0x01
	aiCanonname   = 0x02
	aiNumerichost = 0x04
	aiV4mapped    = 0x08
	aiAll         = 0x10
	aiAddrconfig  = 0x20
	aiNumericserv = 0x400
	niNumerichost = 0x01
	niNumericserv = 0x02
	niNofqdn      = 0x04
	niNamereqd    = 0x08
	niDgram       = 0x10
	eaiBadflags   = -1
	eaiNoname     = -2
	eaiAgain      = -3
	eaiFail       = -4
	eaiFamily     = -6
	eaiSocktype   = -7
	eaiService    = -8
	eaiMemory     = -10
	eaiSystem     = -11
	eaiOverflow   = -12
	hostNotFound  = 1
	tryAgain      = 2
	noRecovery    = 3
	sockStream    = 1
	sockDgram     = 2
	sockRaw       = 3
	ipprotoTCP    = 6
	ipprotoUDP    = 17
	resolveWait   = 10 * time.Second
)

var gaiMessages = []string{"Invalid flags", "Name does not resolve", "Try again", "Non-recoverable error", "Name has no usable address",
	"Unrecognized address family or invalid length", "Unrecognized socket type", "Unrecognized service", "Unknown error",
	"Out of memory", "System error", "Overflow"}

var hostMessages = []string{"Host not found", "Try again", "Non-recoverable error", "Address not available"}

// protocols is musl's table.
var protocols = []struct {
	number int
	name   string
}{
	{0, "ip"}, {1, "icmp"}, {2, "igmp"}, {3, "ggp"}, {4, "ipencap"}, {5, "st"}, {6, "tcp"}, {8, "egp"}, {12, "pup"}, {17, "udp"},
	{20, "hmp"}, {22, "xns-idp"}, {27, "rdp"}, {29, "iso-tp4"}, {36, "xtp"}, {37, "ddp"}, {38, "idpr-cmtp"}, {41, "ipv6"},
	{43, "ipv6-route"}, {44, "ipv6-frag"}, {45, "idrp"}, {46, "rsvp"}, {47, "gre"}, {50, "esp"}, {51, "ah"}, {57, "skip"},
	{58, "ipv6-icmp"}, {59, "ipv6-nonxt"}, {60, "ipv6-opts"}, {73, "rspf"}, {81, "vmtp"}, {89, "ospf"}, {94, "ipip"},
	{98, "encap"}, {103, "pim"}, {255, "raw"},
}

// wellKnown are the services without the root's /etc/services.
var wellKnown = []serviceEntry{
	{"echo", 7, "tcp"}, {"echo", 7, "udp"}, {"discard", 9, "tcp"}, {"discard", 9, "udp"}, {"daytime", 13, "tcp"},
	{"ftp-data", 20, "tcp"}, {"ftp", 21, "tcp"}, {"ssh", 22, "tcp"}, {"telnet", 23, "tcp"}, {"smtp", 25, "tcp"},
	{"time", 37, "tcp"}, {"domain", 53, "tcp"}, {"domain", 53, "udp"}, {"tftp", 69, "udp"}, {"finger", 79, "tcp"},
	{"http", 80, "tcp"}, {"pop3", 110, "tcp"}, {"sunrpc", 111, "tcp"}, {"sunrpc", 111, "udp"}, {"nntp", 119, "tcp"},
	{"ntp", 123, "udp"}, {"imap", 143, "tcp"}, {"snmp", 161, "udp"}, {"ldap", 389, "tcp"}, {"https", 443, "tcp"},
	{"syslog", 514, "udp"}, {"submission", 587, "tcp"}, {"imaps", 993, "tcp"}, {"pop3s", 995, "tcp"},
}

type serviceEntry struct {
	name  string
	port  int
	proto string
}

type hostEntry struct {
	addr  netip.Addr
	names []string
}

// fileLines are the lines of a file of the root, comments cut.
func (p *Process) fileLines(path string) ([]string, bool) {
	fd, err := p.openat2(path, syscall.O_RDONLY, 0)

	if err != nil {
		return nil, false
	}

	defer syscall.Close(fd)

	var data []byte
	buf := make([]byte, 4096)

	for len(data) < databaseMax {
		n, err := syscall.Read(fd, buf)

		if n <= 0 || err != nil {
			break
		}

		data = append(data, buf[:n]...)
	}

	var out []string

	for _, line := range strings.Split(string(data), "\n") {
		line, _, _ = strings.Cut(line, "#")

		if line = strings.TrimSpace(line); line != "" {
			out = append(out, line)
		}
	}

	return out, true
}

func (p *Process) hostEntries() []hostEntry {
	lines, _ := p.fileLines("/etc/hosts")
	var out []hostEntry

	for _, line := range lines {
		fields := strings.Fields(line)

		if addr, err := netip.ParseAddr(fields[0]); err == nil && len(fields) > 1 {
			out = append(out, hostEntry{addr.WithZone(""), fields[1:]})
		}
	}

	return out
}

func (p *Process) services() []serviceEntry {
	lines, ok := p.fileLines("/etc/services")

	if !ok {
		return wellKnown
	}

	var out []serviceEntry

	for _, line := range lines {
		fields := strings.Fields(line)

		if len(fields) < 2 {
			continue
		}

		port, proto, _ := strings.Cut(fields[1], "/")
		n, err := strconv.Atoi(port)

		if err != nil {
			continue
		}

		for _, name := range append([]string{fields[0]}, fields[2:]...) {
			out = append(out, serviceEntry{name, n, proto})
		}
	}

	return out
}

// numericHost parses a numeric address as musl does: IPv4 by inet_aton,
// IPv6 with a scope of a number or an interface.
func numericHost(s string) (netip.Addr, uint32, bool) {
	if a, ok := inetAton(s); ok {
		return netip.AddrFrom4(a), 0, true
	}

	host, zone, _ := strings.Cut(s, "%")
	addr, err := netip.ParseAddr(host)

	if err != nil || !addr.Is6() {
		return netip.Addr{}, 0, false
	}

	scope := uint32(0)

	if zone != "" {
		if n, err := strconv.ParseUint(zone, 10, 32); err == nil {
			scope = uint32(n)
		} else if ifc, err := net.InterfaceByName(zone); err == nil {
			scope = uint32(ifc.Index)
		} else {
			return netip.Addr{}, 0, false
		}
	}

	return addr, scope, true
}

// resolve looks a name up: the root's /etc/hosts, then Go's resolver. The
// error is an EAI_ code.
func (p *Process) resolve(name string) ([]netip.Addr, string, int) {
	var out []netip.Addr

	for _, e := range p.hostEntries() {
		for _, n := range e.names {
			if strings.EqualFold(n, name) {
				out = append(out, e.addr)

				break
			}
		}
	}

	if len(out) > 0 {
		return out, name, 0
	}

	ctx, cancel := context.WithTimeout(context.Background(), resolveWait)
	defer cancel()

	found, err := net.DefaultResolver.LookupNetIP(ctx, "ip", name)

	if err != nil {
		var dns *net.DNSError

		if errors.As(err, &dns) && (dns.IsTemporary || dns.IsTimeout) {
			return nil, "", eaiAgain
		}

		if errors.As(err, &dns) && dns.IsNotFound {
			return nil, "", eaiNoname
		}

		return nil, "", eaiFail
	}

	canonical := name

	if cname, err := net.DefaultResolver.LookupCNAME(ctx, name); err == nil && cname != "" {
		canonical = strings.TrimSuffix(cname, ".")
	}

	for _, a := range found {
		out = append(out, a.Unmap())
	}

	return out, canonical, 0
}

// reverse is a name of an address: the root's /etc/hosts', then the
// resolver's.
func (p *Process) reverse(addr netip.Addr) (string, bool) {
	for _, e := range p.hostEntries() {
		if e.addr == addr {
			return e.names[0], true
		}
	}

	ctx, cancel := context.WithTimeout(context.Background(), resolveWait)
	defer cancel()

	if names, err := net.DefaultResolver.LookupAddr(ctx, addr.String()); err == nil && len(names) > 0 {
		return strings.TrimSuffix(names[0], "."), true
	}

	return "", false
}

// configured tells whether a family has an address: as musl's AI_ADDRCONFIG,
// a datagram socket of it connects to its loopback.
func configured(family int) bool {
	fd, err := syscall.Socket(family, syscall.SOCK_DGRAM|syscall.SOCK_CLOEXEC, ipprotoUDP)

	if err != nil {
		return false
	}

	defer syscall.Close(fd)

	if family == afInet {
		return syscall.Connect(fd, &syscall.SockaddrInet4{Port: 65535, Addr: [4]byte{127, 0, 0, 1}}) == nil
	}

	return syscall.Connect(fd, &syscall.SockaddrInet6{Port: 65535, Addr: [16]byte{15: 1}}) == nil
}

// sockaddr is the bytes of an address's struct sockaddr_in or _in6.
func sockaddr(addr netip.Addr, port int, scope uint32) []byte {
	if addr.Is4() {
		b := make([]byte, 16)
		binary.LittleEndian.PutUint16(b, afInet)
		binary.BigEndian.PutUint16(b[2:], uint16(port))
		a := addr.As4()
		copy(b[4:], a[:])

		return b
	}

	b := make([]byte, 28)
	binary.LittleEndian.PutUint16(b, afInet6)
	binary.BigEndian.PutUint16(b[2:], uint16(port))
	a := addr.As16()
	copy(b[8:], a[:])
	binary.LittleEndian.PutUint32(b[24:], scope)

	return b
}

// servicePort is a service's port for a protocol, "" for any.
func (p *Process) servicePort(name, proto string) (int, bool) {
	for _, s := range p.services() {
		if s.name == name && (proto == "" || s.proto == proto) {
			return s.port, true
		}
	}

	return 0, false
}

func (p *Process) serviceName(port int, proto string) (serviceEntry, bool) {
	for _, s := range p.services() {
		if s.port == port && (proto == "" || s.proto == proto) {
			return s, true
		}
	}

	return serviceEntry{}, false
}

type socketKind struct {
	socktype, protocol int
}

func (p *Process) getaddrinfo(node, service, hints, res uint32) int {
	flags, family, socktype, protocol := 0, 0, 0, 0

	if hints != 0 {
		flags, family = int(int32(p.u32(hints))), int(int32(p.u32(hints+4)))
		socktype, protocol = int(int32(p.u32(hints+8))), int(int32(p.u32(hints+12)))
	}

	switch {
	case node == 0 && service == 0:
		return eaiNoname
	case flags&^(aiPassive|aiCanonname|aiNumerichost|aiV4mapped|aiAll|aiAddrconfig|aiNumericserv) != 0:
		return eaiBadflags
	case family != 0 && family != afInet && family != afInet6:
		return eaiFamily
	case socktype != 0 && socktype != sockStream && socktype != sockDgram && socktype != sockRaw:
		return eaiSocktype
	}

	kinds := []socketKind{{sockStream, ipprotoTCP}, {sockDgram, ipprotoUDP}}

	if socktype != 0 || protocol != 0 {
		kinds = nil

		for _, k := range []socketKind{{sockStream, ipprotoTCP}, {sockDgram, ipprotoUDP}, {sockRaw, protocol}} {
			if (socktype == 0 || k.socktype == socktype) && (protocol == 0 || k.protocol == protocol) {
				kinds = append(kinds, k)
			}
		}

		if len(kinds) == 0 {
			return eaiSocktype
		}
	}

	port := 0

	if service != 0 {
		name := p.str(service)
		n, err := strconv.ParseUint(name, 10, 16)

		switch {
		case err == nil:
			port = int(n)
		case flags&aiNumericserv != 0:
			return eaiNoname
		default:
			var kept []socketKind

			for _, k := range kinds {
				proto := "tcp"

				if k.socktype == sockDgram {
					proto = "udp"
				}

				if found, ok := p.servicePort(name, proto); ok && k.socktype != sockRaw {
					port = found
					kept = append(kept, k)
				}
			}

			if len(kept) == 0 {
				return eaiService
			}

			kinds = kept
		}
	}

	type address struct {
		addr  netip.Addr
		scope uint32
	}

	var addrs []address
	canonical := ""

	if node == 0 {
		any4, any6 := netip.IPv4Unspecified(), netip.IPv6Unspecified()

		if flags&aiPassive == 0 {
			any4, any6 = netip.AddrFrom4([4]byte{127, 0, 0, 1}), netip.IPv6Loopback()
		}

		addrs = []address{{any4, 0}, {any6, 0}}
	} else {
		name := p.str(node)

		if a, scope, ok := numericHost(name); ok {
			addrs, canonical = []address{{a, scope}}, name
		} else if flags&aiNumerichost != 0 {
			return eaiNoname
		} else {
			found, canon, code := p.resolve(name)

			if code != 0 {
				return code
			}

			canonical = canon

			for _, a := range found {
				addrs = append(addrs, address{a, 0})
			}
		}
	}

	var kept []address
	have6 := false

	for _, a := range addrs {
		have6 = have6 || a.addr.Is6()
	}

	usable4, usable6 := true, true

	if flags&aiAddrconfig != 0 {
		usable4, usable6 = configured(afInet), configured(afInet6)
	}

	for _, a := range addrs {
		switch {
		case family == afInet && !a.addr.Is4():
		case family == afInet6 && a.addr.Is4():
			if flags&aiV4mapped != 0 && (!have6 || flags&aiAll != 0) {
				kept = append(kept, address{netip.AddrFrom16(a.addr.As16()), 0})
			}
		case a.addr.Is4() && !usable4, a.addr.Is6() && !usable6:
		default:
			kept = append(kept, a)
		}
	}

	if len(kept) == 0 {
		return eaiNoname
	}

	first, prev := uint32(0), uint32(0)

	for _, a := range kept {
		for _, k := range kinds {
			addr := sockaddr(a.addr, port, a.scope)
			name := ""

			if first == 0 && flags&aiCanonname != 0 {
				name = canonical
			}

			size := 32 + uint32(len(addr))

			if name != "" {
				size += uint32(len(name)) + 1
			}

			node := p.malloc(size)

			if node == 0 {
				p.freeaddrinfo(first)

				return eaiMemory
			}

			clear(p.span(node, size))
			family := afInet6

			if a.addr.Is4() {
				family = afInet
			}

			for i, v := range []uint32{uint32(flags), uint32(family), uint32(k.socktype), uint32(k.protocol), uint32(len(addr)), node + 32} {
				p.put32(node+4*uint32(i), v)
			}

			copy(p.span(node+32, uint32(len(addr))), addr)

			if name != "" {
				p.putString(node+32+uint32(len(addr)), name)
				p.put32(node+24, node+32+uint32(len(addr)))
			}

			if first == 0 {
				first = node
			} else {
				p.put32(prev+28, node)
			}

			prev = node
		}
	}

	p.put32(res, first)

	return 0
}

func (p *Process) freeaddrinfo(node uint32) {
	for node != 0 {
		next := p.u32(node + 28)
		p.free(node)
		node = next
	}
}

func (p *Process) getnameinfo(sa, salen, host, hostlen, serv, servlen, flags uint32) int {
	if salen < 2 {
		return eaiFamily
	}

	var addr netip.Addr
	scope := uint32(0)

	switch p.u16(sa) {
	case afInet:
		if salen < 16 {
			return eaiFamily
		}

		addr = netip.AddrFrom4([4]byte(p.span(sa+4, 4)))
	case afInet6:
		if salen < 28 {
			return eaiFamily
		}

		addr, scope = netip.AddrFrom16([16]byte(p.span(sa+8, 16))), p.u32(sa+24)
	default:
		return eaiFamily
	}

	if host != 0 && hostlen > 0 {
		name, found := "", false

		if flags&niNumerichost == 0 {
			name, found = p.reverse(addr)

			if found && flags&niNofqdn != 0 {
				name, _, _ = strings.Cut(name, ".")
			}
		}

		if !found {
			if flags&niNamereqd != 0 {
				return eaiNoname
			}

			name = addr.String()

			if scope != 0 {
				name += "%" + strconv.Itoa(int(scope))
			}
		}

		if uint32(len(name)) >= hostlen {
			return eaiOverflow
		}

		p.putString(host, name)
	}

	if serv != 0 && servlen > 0 {
		port := int(binary.BigEndian.Uint16(p.span(sa+2, 2)))
		name := strconv.Itoa(port)

		if flags&niNumericserv == 0 {
			proto := "tcp"

			if flags&niDgram != 0 {
				proto = "udp"
			}

			if s, ok := p.serviceName(port, proto); ok {
				name = s.name
			}
		}

		if uint32(len(name)) >= servlen {
			return eaiOverflow
		}

		p.putString(serv, name)
	}

	return 0
}

// setHerrno sets h_errno, which __kernel_start told the kernel of.
func (p *Process) setHerrno(code int) {
	if p.herrnoAt != 0 {
		p.put32(p.herrnoAt, uint32(code))
	}
}

// hostent writes the static struct hostent of a name and its addresses of
// one family.
func (p *Process) hostent(name string, family int, addrs []netip.Addr) uint32 {
	length := 4

	if family == afInet6 {
		length = 16
	}

	size := 20 + 4 + 4*uint32(len(addrs)+1) + uint32(length*len(addrs)) + uint32(len(name)) + 1
	block := p.staticResult(staticHostent, size)

	if block == 0 {
		p.setHerrno(noRecovery)

		return 0
	}

	aliases := block + 20
	list := aliases + 4
	data := list + 4*uint32(len(addrs)+1)
	str := data + uint32(length*len(addrs))
	p.put32(aliases, 0)

	for i, a := range addrs {
		raw := a.As16()
		at := data + uint32(length*i)

		if family == afInet {
			v4 := a.As4()
			copy(p.span(at, 4), v4[:])
		} else {
			copy(p.span(at, 16), raw[:])
		}

		p.put32(list+4*uint32(i), at)
	}

	p.put32(list+4*uint32(len(addrs)), 0)
	p.putString(str, name)

	for i, v := range []uint32{str, aliases, uint32(family), uint32(length), list} {
		p.put32(block+4*uint32(i), v)
	}

	return block
}

func (p *Process) gethostbyname(name string, family int) uint32 {
	var found []netip.Addr
	canonical := name

	if a, _, ok := numericHost(name); ok {
		found = []netip.Addr{a}
	} else {
		addrs, canon, code := p.resolve(name)

		switch code {
		case 0:
		case eaiAgain:
			p.setHerrno(tryAgain)

			return 0
		default:
			p.setHerrno(hostNotFound)

			return 0
		}

		found, canonical = addrs, canon
	}

	var kept []netip.Addr

	for _, a := range found {
		if a.Is4() == (family == afInet) {
			kept = append(kept, a)
		}
	}

	if len(kept) == 0 {
		p.setHerrno(hostNotFound)

		return 0
	}

	return p.hostent(canonical, family, kept)
}

// servent writes the static struct servent of a service.
func (p *Process) servent(s serviceEntry) uint32 {
	size := 16 + 4 + uint32(len(s.name)+len(s.proto)) + 2
	block := p.staticResult(staticServent, size)

	if block == 0 {
		return 0
	}

	name := block + 20
	proto := name + uint32(len(s.name)) + 1
	p.putString(name, s.name)
	p.putString(proto, s.proto)
	p.put32(block+16, 0)
	var port [2]byte
	binary.BigEndian.PutUint16(port[:], uint16(s.port))

	for i, v := range []uint32{name, block + 16, uint32(binary.LittleEndian.Uint16(port[:])), proto} {
		p.put32(block+4*uint32(i), v)
	}

	return block
}

func (p *Process) protoent(number int, name string) uint32 {
	block := p.staticResult(staticProtoent, 12+4+uint32(len(name))+1)

	if block == 0 {
		return 0
	}

	p.putString(block+16, name)
	p.put32(block+12, 0)

	for i, v := range []uint32{block + 16, block + 12, uint32(number)} {
		p.put32(block+4*uint32(i), v)
	}

	return block
}

// strtoulPart is strtoul's number of base 0 that a part of an address
// is: hexadecimal after 0x, octal after 0, decimal else, its overflow
// 32-bit unsigned long's largest.
func strtoulPart(s string) (uint64, bool) {
	base, digits := uint64(10), s

	switch {
	case len(s) > 2 && (s[:2] == "0x" || s[:2] == "0X"):
		base, digits = 16, s[2:]
	case len(s) > 1 && s[0] == '0':
		base, digits = 8, s[1:]
	}

	v := uint64(0)

	for _, c := range []byte(digits) {
		d := uint64(16)

		switch {
		case c >= '0' && c <= '9':
			d = uint64(c - '0')
		case c >= 'a' && c <= 'f':
			d = uint64(c-'a') + 10
		case c >= 'A' && c <= 'F':
			d = uint64(c-'A') + 10
		}

		if d >= base {
			return 0, false
		}

		v = min(v*base+d, 0xffffffff)
	}

	return v, true
}

// inetAton is musl's: one to four numbers of strtoul's bases, the last
// filling the bytes left.
func inetAton(s string) ([4]byte, bool) {
	var parts [4]uint64
	n := 0

	for {
		if n == 4 || s == "" || s[0] < '0' || s[0] > '9' {
			return [4]byte{}, false
		}

		end := 1

		for end < len(s) && s[end] != '.' {
			end++
		}

		v, ok := strtoulPart(s[:end])

		if !ok {
			return [4]byte{}, false
		}

		parts[n] = v
		n++

		if end == len(s) {
			break
		}

		s = s[end+1:]
	}

	var out [4]byte
	last := parts[n-1]
	room := 4 - (n - 1)

	if room < 4 && last>>(8*room) != 0 || room == 4 && last > 0xffffffff {
		return [4]byte{}, false
	}

	for i := range n - 1 {
		if parts[i] > 255 {
			return [4]byte{}, false
		}

		out[i] = byte(parts[i])
	}

	for i := 0; i < room; i++ {
		out[3-i] = byte(last >> (8 * i))
	}

	return out, true
}

func (k *Kernel) netdbFunctions() map[string]any {
	return map[string]any{
		"getaddrinfo": func(ctx uintptr, node, service, hints, res uint32) uint32 {
			return uint32(int32(proc(ctx).getaddrinfo(node, service, hints, res)))
		},
		"freeaddrinfo": func(ctx uintptr, res uint32) {
			proc(ctx).freeaddrinfo(res)
		},
		"gai_strerror": func(ctx uintptr, code uint32) uint32 {
			p := proc(ctx)
			msg := "Unknown error"

			if c := -int(int32(code)); c >= 1 && c <= len(gaiMessages) {
				msg = gaiMessages[c-1]
			}

			return p.constant(msg)
		},
		"getnameinfo": func(ctx uintptr, sa, salen, host, hostlen, serv, servlen, flags uint32) uint32 {
			return uint32(int32(proc(ctx).getnameinfo(sa, salen, host, hostlen, serv, servlen, flags)))
		},
		"gethostbyname": func(ctx uintptr, name uint32) uint32 {
			p := proc(ctx)

			return p.gethostbyname(p.str(name), afInet)
		},
		"gethostbyname2": func(ctx uintptr, name, family uint32) uint32 {
			p := proc(ctx)

			if family != afInet && family != afInet6 {
				p.setHerrno(noRecovery)

				return 0
			}

			return p.gethostbyname(p.str(name), int(family))
		},
		"gethostbyaddr": func(ctx uintptr, addr, n, family uint32) uint32 {
			p := proc(ctx)
			var a netip.Addr

			switch {
			case family == afInet && n == 4:
				a = netip.AddrFrom4([4]byte(p.span(addr, 4)))
			case family == afInet6 && n == 16:
				a = netip.AddrFrom16([16]byte(p.span(addr, 16)))
			default:
				p.setHerrno(noRecovery)

				return 0
			}

			name, ok := p.reverse(a)

			if !ok {
				p.setHerrno(hostNotFound)

				return 0
			}

			return p.hostent(name, int(family), []netip.Addr{a})
		},
		"gethostent": func(ctx uintptr) uint32 {
			p := proc(ctx)
			e := nextEntry(p, staticHostIndex, p.hostEntries())

			if e == nil {
				return 0
			}

			family := afInet6

			if e.addr.Is4() {
				family = afInet
			}

			return p.hostent(e.names[0], family, []netip.Addr{e.addr})
		},
		"sethostent": func(ctx uintptr, stay uint32) {
			p := proc(ctx)
			p.put32(p.staticAt(staticHostIndex), 0)
		},
		"endhostent": func(ctx uintptr) {
			p := proc(ctx)
			p.put32(p.staticAt(staticHostIndex), 0)
		},
		"getservbyname": func(ctx uintptr, name, proto uint32) uint32 {
			p := proc(ctx)
			want := ""

			if proto != 0 {
				want = p.str(proto)
			}

			port, ok := p.servicePort(p.str(name), want)

			if !ok {
				return 0
			}

			s, _ := p.serviceName(port, want)

			return p.servent(serviceEntry{p.str(name), port, s.proto})
		},
		"getservbyport": func(ctx uintptr, port, proto uint32) uint32 {
			p := proc(ctx)
			want := ""

			if proto != 0 {
				want = p.str(proto)
			}

			var b [2]byte
			binary.LittleEndian.PutUint16(b[:], uint16(port))
			s, ok := p.serviceName(int(binary.BigEndian.Uint16(b[:])), want)

			if !ok {
				return 0
			}

			return p.servent(s)
		},
		"getprotobyname": func(ctx uintptr, name uint32) uint32 {
			p := proc(ctx)
			s := p.str(name)

			for _, e := range protocols {
				if e.name == s {
					return p.protoent(e.number, e.name)
				}
			}

			return 0
		},
		"getprotobynumber": func(ctx uintptr, number uint32) uint32 {
			p := proc(ctx)

			for _, e := range protocols {
				if e.number == int(int32(number)) {
					return p.protoent(e.number, e.name)
				}
			}

			return 0
		},
		// There is no networks database, as musl has none.
		"getnetbyname": func(ctx uintptr, name uint32) uint32 {
			return 0
		},
		"getnetbyaddr": func(ctx uintptr, net, family uint32) uint32 {
			return 0
		},
		"hstrerror": func(ctx uintptr, code uint32) uint32 {
			p := proc(ctx)
			msg := "Unknown error"

			if c := int(int32(code)); c >= 1 && c <= len(hostMessages) {
				msg = hostMessages[c-1]
			}

			return p.constant(msg)
		},
		"herror": func(ctx uintptr, s uint32) {
			p := proc(ctx)
			msg := "Unknown error"

			if p.herrnoAt != 0 {
				if c := int(int32(p.u32(p.herrnoAt))); c >= 1 && c <= len(hostMessages) {
					msg = hostMessages[c-1]
				}
			}

			p.complain(s, msg)
		},
		"inet_aton": func(ctx uintptr, s, at uint32) uint32 {
			p := proc(ctx)
			a, ok := inetAton(p.str(s))

			if ok {
				copy(p.span(at, 4), a[:])
			}

			return bit(ok)
		},
		"inet_addr": func(ctx uintptr, s uint32) uint32 {
			a, ok := inetAton(proc(ctx).str(s))

			if !ok {
				return 0xffffffff
			}

			return binary.LittleEndian.Uint32(a[:])
		},
		"inet_network": func(ctx uintptr, s uint32) uint32 {
			a, ok := inetAton(proc(ctx).str(s))

			if !ok {
				return 0xffffffff
			}

			return binary.BigEndian.Uint32(a[:])
		},
		"inet_ntoa": func(ctx uintptr, in uint32) uint32 {
			p := proc(ctx)
			var b [4]byte
			binary.LittleEndian.PutUint32(b[:], in)
			at := p.staticAt(staticNtoa)
			p.putString(at, fmt.Sprintf("%d.%d.%d.%d", b[0], b[1], b[2], b[3]))

			return at
		},
		"inet_pton": func(ctx uintptr, family, s, dst uint32) uint32 {
			p := proc(ctx)
			text := p.str(s)

			switch family {
			case afInet:
				a, err := netip.ParseAddr(text)

				if err != nil || !a.Is4() {
					return 0
				}

				v := a.As4()
				copy(p.span(dst, 4), v[:])
			case afInet6:
				a, err := netip.ParseAddr(text)

				if err != nil || !a.Is6() || a.Zone() != "" {
					return 0
				}

				v := a.As16()
				copy(p.span(dst, 16), v[:])
			default:
				p.setErrno(syscall.EAFNOSUPPORT)

				return ^uint32(0)
			}

			return 1
		},
		"inet_ntop": func(ctx uintptr, family, src, dst, size uint32) uint32 {
			p := proc(ctx)
			var text string

			switch family {
			case afInet:
				text = netip.AddrFrom4([4]byte(p.span(src, 4))).String()
			case afInet6:
				text = netip.AddrFrom16([16]byte(p.span(src, 16))).String()
			default:
				p.setErrno(syscall.EAFNOSUPPORT)

				return 0
			}

			if uint32(len(text)) >= size {
				p.setErrno(syscall.ENOSPC)

				return 0
			}

			p.putString(dst, text)

			return dst
		},
		"inet_makeaddr": func(ctx uintptr, n, h uint32) uint32 {
			switch {
			case n < 256:
				h |= n << 24
			case n < 65536:
				h |= n << 16
			default:
				h |= n << 8
			}

			return h
		},
		"inet_lnaof": func(ctx uintptr, in uint32) uint32 {
			switch {
			case in>>24 < 128:
				return in & 0xffffff
			case in>>24 < 192:
				return in & 0xffff
			}

			return in & 0xff
		},
		"inet_netof": func(ctx uintptr, in uint32) uint32 {
			switch {
			case in>>24 < 128:
				return in >> 24
			case in>>24 < 192:
				return in >> 16
			}

			return in >> 8
		},
		"if_nametoindex": func(ctx uintptr, name uint32) uint32 {
			p := proc(ctx)
			ifc, err := net.InterfaceByName(p.str(name))

			if err != nil {
				p.setErrno(syscall.ENXIO)

				return 0
			}

			return uint32(ifc.Index)
		},
		"if_indextoname": func(ctx uintptr, index, buf uint32) uint32 {
			p := proc(ctx)
			ifc, err := net.InterfaceByIndex(int(index))

			if err != nil {
				p.setErrno(syscall.ENXIO)

				return 0
			}

			p.putString(buf, truncate(ifc.Name, 15))

			return buf
		},
		// if_nameindex's array and names are one block, which
		// if_freenameindex frees.
		"if_nameindex": func(ctx uintptr) uint32 {
			p := proc(ctx)
			list, err := net.Interfaces()

			if err != nil {
				p.setErrno(errnoOf(err))

				return 0
			}

			size := 8 * uint32(len(list)+1)

			for _, ifc := range list {
				size += uint32(len(ifc.Name)) + 1
			}

			block := p.malloc(size)

			if block == 0 {
				p.setErrno(syscall.ENOBUFS)

				return 0
			}

			names := block + 8*uint32(len(list)+1)

			for i, ifc := range list {
				p.put32(block+8*uint32(i), uint32(ifc.Index))
				p.put32(block+8*uint32(i)+4, names)
				p.putString(names, ifc.Name)
				names += uint32(len(ifc.Name)) + 1
			}

			p.put32(block+8*uint32(len(list)), 0)
			p.put32(block+8*uint32(len(list))+4, 0)

			return block
		},
		"if_freenameindex": func(ctx uintptr, block uint32) {
			proc(ctx).free(block)
		},
	}
}
