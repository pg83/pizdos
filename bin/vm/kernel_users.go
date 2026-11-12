//go:build linux && amd64

package main

import (
	"bytes"
	"slices"
	"strconv"
	"strings"
	"syscall"
)

// The user and group databases are the root's /etc/passwd and /etc/group,
// read at every call: a missing file is an empty database. The results of
// the functions without _r are heap blocks the static area points at, each
// freed by the next; so are the positions of getpwent and getgrent.

type passwdEntry struct {
	name, passwd      string
	uid, gid          uint32
	gecos, dir, shell string
}

type groupEntry struct {
	name, passwd string
	gid          uint32
	members      []string
}

// databaseMax bounds the files the databases read.
const databaseMax = 16 << 20

// databaseLines are the lines of a database, split in fields, those of
// the field count only.
func (p *Process) databaseLines(path string, fields int) [][]string {
	data, err := p.readPath(path, databaseMax)

	if err != nil {
		return nil
	}

	var out [][]string

	for _, line := range bytes.Split(data, []byte("\n")) {
		if parts := strings.Split(string(line), ":"); len(parts) == fields {
			out = append(out, parts)
		}
	}

	return out
}

func parseId(s string) (uint32, bool) {
	n, err := strconv.ParseUint(s, 10, 32)

	return uint32(n), err == nil
}

func (p *Process) passwdEntries() []passwdEntry {
	var out []passwdEntry

	for _, f := range p.databaseLines("/etc/passwd", 7) {
		uid, ok1 := parseId(f[2])
		gid, ok2 := parseId(f[3])

		if ok1 && ok2 {
			out = append(out, passwdEntry{f[0], f[1], uid, gid, f[4], f[5], f[6]})
		}
	}

	return out
}

func (p *Process) groupEntries() []groupEntry {
	var out []groupEntry

	for _, f := range p.databaseLines("/etc/group", 4) {
		gid, ok := parseId(f[2])

		if !ok {
			continue
		}

		var members []string

		if f[3] != "" {
			members = strings.Split(f[3], ",")
		}

		out = append(out, groupEntry{f[0], f[1], gid, members})
	}

	return out
}

// passwdSize is the room an entry's strings take.
func (e *passwdEntry) size() uint32 {
	return uint32(len(e.name) + len(e.passwd) + len(e.gecos) + len(e.dir) + len(e.shell) + 5)
}

// putPasswd writes a struct passwd at at, its strings at buf.
func (p *Process) putPasswd(at, buf uint32, e *passwdEntry) {
	ptrs := make([]uint32, 0, 5)

	for _, s := range []string{e.name, e.passwd, e.gecos, e.dir, e.shell} {
		p.putString(buf, s)
		ptrs = append(ptrs, buf)
		buf += uint32(len(s) + 1)
	}

	p.put32(at, ptrs[0])
	p.put32(at+4, ptrs[1])
	p.put32(at+8, e.uid)
	p.put32(at+12, e.gid)
	p.put32(at+16, ptrs[2])
	p.put32(at+20, ptrs[3])
	p.put32(at+24, ptrs[4])
}

// size is the room an entry's member pointers, aligned at align, and its
// strings take.
func (e *groupEntry) size(align uint32) uint32 {
	n := (4-align%4)%4 + 4*uint32(len(e.members)+1) + uint32(len(e.name)+len(e.passwd)+2)

	for _, m := range e.members {
		n += uint32(len(m) + 1)
	}

	return n
}

// putGroup writes a struct group at at, its members and strings at buf.
func (p *Process) putGroup(at, buf uint32, e *groupEntry) {
	mem := buf + (4-buf%4)%4
	s := mem + 4*uint32(len(e.members)+1)
	str := func(v string) uint32 {
		at := s
		p.putString(at, v)
		s += uint32(len(v) + 1)

		return at
	}

	p.put32(at, str(e.name))
	p.put32(at+4, str(e.passwd))
	p.put32(at+8, e.gid)
	p.put32(at+12, mem)

	for i, m := range e.members {
		p.put32(mem+4*uint32(i), str(m))
	}

	p.put32(mem+4*uint32(len(e.members)), 0)
}

// passwdResult is the reentrant functions' work: it returns an error
// number, and puts the entry at pwd or nothing at result.
func (p *Process) passwdResult(e *passwdEntry, pwd, buf, size, result uint32) uint32 {
	p.put32(result, 0)

	if e == nil {
		return 0
	}

	if e.size() > size {
		return uint32(syscall.ERANGE)
	}

	p.putPasswd(pwd, buf, e)
	p.put32(result, pwd)

	return 0
}

func (p *Process) groupResult(e *groupEntry, grp, buf, size, result uint32) uint32 {
	p.put32(result, 0)

	if e == nil {
		return 0
	}

	if e.size(buf) > size {
		return uint32(syscall.ERANGE)
	}

	p.putGroup(grp, buf, e)
	p.put32(result, grp)

	return 0
}

// staticResult replaces the heap block the static area keeps at slot with
// one of size bytes.
func (p *Process) staticResult(slot, size uint32) uint32 {
	at := p.staticAt(slot)
	p.free(p.u32(at))
	p.put32(at, 0)
	block := p.malloc(size)

	if block == 0 {
		p.setErrno(syscall.ENOMEM)

		return 0
	}

	p.put32(at, block)

	return block
}

func (p *Process) staticPasswd(e *passwdEntry) uint32 {
	if e == nil {
		return 0
	}

	block := p.staticResult(staticPasswd, 28+e.size())

	if block != 0 {
		p.putPasswd(block, block+28, e)
	}

	return block
}

func (p *Process) staticGroup(e *groupEntry) uint32 {
	if e == nil {
		return 0
	}

	block := p.staticResult(staticGroup, 16+e.size(16))

	if block != 0 {
		p.putGroup(block, block+16, e)
	}

	return block
}

func (p *Process) passwdBy(match func(*passwdEntry) bool) *passwdEntry {
	entries := p.passwdEntries()

	if i := slices.IndexFunc(entries, func(e passwdEntry) bool { return match(&e) }); i >= 0 {
		return &entries[i]
	}

	return nil
}

func (p *Process) groupBy(match func(*groupEntry) bool) *groupEntry {
	entries := p.groupEntries()

	if i := slices.IndexFunc(entries, func(e groupEntry) bool { return match(&e) }); i >= 0 {
		return &entries[i]
	}

	return nil
}

// nextEntry is the entry at the position the static area keeps at slot,
// which moves past it.
func nextEntry[E any](p *Process, slot uint32, entries []E) *E {
	at := p.staticAt(slot)
	i := p.u32(at)

	if i >= uint32(len(entries)) {
		return nil
	}

	p.put32(at, i+1)

	return &entries[i]
}

func byName(p *Process, name uint32) func(*passwdEntry) bool {
	s := p.str(name)

	return func(e *passwdEntry) bool { return e.name == s }
}

func byUid(uid uint32) func(*passwdEntry) bool {
	return func(e *passwdEntry) bool { return e.uid == uid }
}

func groupName(p *Process, name uint32) func(*groupEntry) bool {
	s := p.str(name)

	return func(e *groupEntry) bool { return e.name == s }
}

func byGid(gid uint32) func(*groupEntry) bool {
	return func(e *groupEntry) bool { return e.gid == gid }
}

// rewind starts the entries of a static slot again.
func (p *Process) rewind(slot uint32) {
	p.put32(p.staticAt(slot), 0)
}

func (p *Process) libcGetpwnam(name uint32) uint32 {
	return p.staticPasswd(p.passwdBy(byName(p, name)))
}

func (p *Process) libcGetpwuid(uid uint32) uint32 {
	return p.staticPasswd(p.passwdBy(byUid(uid)))
}

func (p *Process) libcGetpwnam_r(name, pwd, buf, size, result uint32) uint32 {
	return p.passwdResult(p.passwdBy(byName(p, name)), pwd, buf, size, result)
}

func (p *Process) libcGetpwuid_r(uid, pwd, buf, size, result uint32) uint32 {
	return p.passwdResult(p.passwdBy(byUid(uid)), pwd, buf, size, result)
}

func (p *Process) libcGetpwent() uint32 {
	return p.staticPasswd(nextEntry(p, staticPwent, p.passwdEntries()))
}

func (p *Process) libcSetpwent() {
	p.rewind(staticPwent)
}

func (p *Process) libcEndpwent() {
	p.rewind(staticPwent)
}

func (p *Process) libcGetgrnam(name uint32) uint32 {
	return p.staticGroup(p.groupBy(groupName(p, name)))
}

func (p *Process) libcGetgrgid(gid uint32) uint32 {
	return p.staticGroup(p.groupBy(byGid(gid)))
}

func (p *Process) libcGetgrnam_r(name, grp, buf, size, result uint32) uint32 {
	return p.groupResult(p.groupBy(groupName(p, name)), grp, buf, size, result)
}

func (p *Process) libcGetgrgid_r(gid, grp, buf, size, result uint32) uint32 {
	return p.groupResult(p.groupBy(byGid(gid)), grp, buf, size, result)
}

func (p *Process) libcGetgrent() uint32 {
	return p.staticGroup(nextEntry(p, staticGrent, p.groupEntries()))
}

func (p *Process) libcSetgrent() {
	p.rewind(staticGrent)
}

func (p *Process) libcEndgrent() {
	p.rewind(staticGrent)
}

// getgrouplist lists group and the groups that name user a member;
// when they do not fit it returns -1 and how many there are.
func (p *Process) libcGetgrouplist(user, group, groups, count uint32) uint32 {
	name := p.str(user)
	list := []uint32{group}

	for _, e := range p.groupEntries() {
		if slices.Contains(e.members, name) && !slices.Contains(list, e.gid) {
			list = append(list, e.gid)
		}
	}

	room := min(int(int32(p.u32(count))), len(list))

	for i := range max(room, 0) {
		p.put32(groups+4*uint32(i), list[i])
	}

	p.put32(count, uint32(len(list)))

	if room < len(list) {
		return ^uint32(0)
	}

	return uint32(len(list))
}
