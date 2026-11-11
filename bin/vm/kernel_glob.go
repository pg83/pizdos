//go:build linux && amd64

package main

import (
	"os"
	"slices"
	"strings"
	"syscall"
)

// glob, as musl's: a pattern's components with wildcards match the names
// of the directories the others lead to, by fnmatch; . and .. are names
// too. A directory that cannot be read goes to the error function, through
// the crt's __call_glob_error, and ends the search when it asks or with
// GLOB_ERR.

const (
	globErr      = 0x01
	globMark     = 0x02
	globNosort   = 0x04
	globDooffs   = 0x08
	globNocheck  = 0x10
	globAppend   = 0x20
	globNoescape = 0x40
	globPeriod   = 0x80
	globTilde    = 0x1000
	globTildeChk = 0x4000
	globNospace  = 1
	globAborted  = 2
	globNomatch  = 3
)

type globber struct {
	p       *Process
	flags   uint32
	errfunc uint32
	found   []string
	aborted bool
}

// magic tells whether a component has a wildcard.
func (g *globber) magic(comp string) bool {
	for i := 0; i < len(comp); i++ {
		switch comp[i] {
		case '\\':
			if g.flags&globNoescape == 0 {
				i++
			}
		case '*', '?', '[':
			return true
		}
	}

	return false
}

// literal is a component without wildcards, its escapes taken.
func (g *globber) literal(comp string) string {
	if g.flags&globNoescape != 0 {
		return comp
	}

	var b strings.Builder

	for i := 0; i < len(comp); i++ {
		if comp[i] == '\\' && i+1 < len(comp) {
			i++
		}

		b.WriteByte(comp[i])
	}

	return b.String()
}

// failed tells the error function of a directory that cannot be read and
// whether the search goes on.
func (g *globber) failed(dir string, err error) bool {
	if g.errfunc != 0 {
		p := g.p
		path := p.newString(dir)

		if path != 0 {
			f, _ := p.inst.export("__call_glob_error")
			stop := f.Interface().(func(uintptr, uint32, uint32, uint32) uint32)(p.inst.ctx(), g.errfunc, path, uint32(errnoOf(err)))
			p.free(path)

			if stop != 0 {
				g.aborted = true
			}
		}
	}

	if g.flags&globErr != 0 {
		g.aborted = true
	}

	return !g.aborted
}

// names are the entries of a directory, . and .. among them.
func (g *globber) names(dir string) ([]string, error) {
	path := dir

	if path == "" {
		path = "."
	}

	fd, err := g.p.openat2(path, syscall.O_RDONLY|syscall.O_DIRECTORY, 0)

	if err != nil {
		return nil, err
	}

	f := os.NewFile(uintptr(fd), path)
	defer f.Close()

	names, err := f.Readdirnames(-1)

	return append([]string{".", ".."}, names...), err
}

func (g *globber) exists(path string) bool {
	return g.p.onPath(path, false, func(int) error { return nil }) == nil
}

// walk matches the components left under dir, which ends with a slash or
// is empty; a pattern ending with a slash matches directories, with it.
func (g *globber) walk(dir string, comps []string) {
	if g.aborted {
		return
	}

	comp, rest := comps[0], comps[1:]

	if comp == "" {
		if len(rest) > 0 {
			g.walk(dir+"/", rest)
		} else if g.exists(dir + ".") {
			g.found = append(g.found, dir)
		}

		return
	}

	if !g.magic(comp) {
		name := dir + g.literal(comp)

		if len(rest) == 0 {
			if g.exists(name) {
				g.found = append(g.found, name)
			}

			return
		}

		g.walk(name+"/", rest)

		return
	}

	names, err := g.names(dir)

	if err != nil && err != syscall.ENOENT && err != syscall.ENOTDIR && !g.failed(strings.TrimSuffix(dir, "/"), err) {
		return
	}

	slices.Sort(names)
	flags := 0

	if g.flags&globPeriod == 0 {
		flags |= fnmPeriod
	}

	if g.flags&globNoescape != 0 {
		flags |= fnmNoescape
	}

	for _, name := range names {
		if (name == "." || name == "..") && !strings.HasPrefix(comp, ".") {
			continue
		}

		if !fnmatch([]byte(comp), []byte(name), flags, true) {
			continue
		}

		if len(rest) == 0 {
			g.found = append(g.found, dir+name)
		} else {
			g.walk(dir+name+"/", rest)
		}
	}
}

// tilde expands a leading ~ or ~user: false when GLOB_TILDE_CHECK finds no
// such home.
func (g *globber) tilde(pattern string) (string, bool) {
	if g.flags&(globTilde|globTildeChk) == 0 || !strings.HasPrefix(pattern, "~") {
		return pattern, true
	}

	user, rest, slash := strings.Cut(pattern[1:], "/")
	home := ""

	if user == "" {
		home, _ = g.p.getenv("HOME")
	} else if e := g.p.passwdBy(func(e *passwdEntry) bool { return e.name == user }); e != nil {
		home = e.dir
	}

	if home == "" {
		return pattern, g.flags&globTildeChk == 0
	}

	if !slash {
		return home, true
	}

	return strings.TrimSuffix(home, "/") + "/" + rest, true
}

func (p *Process) glob(pattern, flags, errfunc, out uint32) uint32 {
	g := &globber{p: p, flags: flags, errfunc: errfunc}
	offs := uint32(0)

	if flags&globDooffs != 0 {
		offs = p.u32(out + 8)
	}

	if flags&globAppend == 0 {
		p.put32(out, 0)
		p.put32(out+4, 0)
		p.put32(out+8, offs)
	}

	pat, ok := g.tilde(p.str(pattern))

	if !ok || pat == "" {
		return globNomatch
	}

	if strings.HasPrefix(pat, "/") {
		g.walk("/", strings.Split(pat[1:], "/"))
	} else {
		g.walk("", strings.Split(pat, "/"))
	}

	if g.aborted {
		return globAborted
	}

	if len(g.found) == 0 {
		if flags&globNocheck == 0 {
			return globNomatch
		}

		g.found = []string{p.str(pattern)}
	}

	if flags&globNosort == 0 {
		slices.Sort(g.found)
	}

	if flags&globMark != 0 {
		for i, name := range g.found {
			var st syscall.Stat_t
			isDir := p.onPath(name, true, func(fd int) error { return syscall.Fstat(fd, &st) }) == nil && st.Mode&syscall.S_IFMT == syscall.S_IFDIR

			if isDir && !strings.HasSuffix(name, "/") {
				g.found[i] = name + "/"
			}
		}
	}

	count, old := p.u32(out), p.u32(out+4)

	total := uint64(offs) + uint64(count) + uint64(len(g.found)) + 1

	if total*4 > heapMax {
		return globNospace
	}

	pathv := p.malloc(uint32(total * 4))

	if pathv == 0 {
		return globNospace
	}

	clear(p.span(pathv, offs*4))

	if old != 0 {
		copy(p.span(pathv+offs*4, count*4), p.span(old+offs*4, count*4))
	}

	for i, name := range g.found {
		s := p.newString(name)

		if s == 0 {
			p.put32(pathv+(offs+count+uint32(i))*4, 0)
			p.globFree(pathv, offs, count+uint32(i))

			return globNospace
		}

		p.put32(pathv+(offs+count+uint32(i))*4, s)
	}

	p.put32(pathv+(offs+count+uint32(len(g.found)))*4, 0)
	p.free(old)
	p.put32(out, count+uint32(len(g.found)))
	p.put32(out+4, pathv)

	return 0
}

// globFree frees count strings of a pathv after offs, and the array.
func (p *Process) globFree(pathv, offs, count uint32) {
	for i := range count {
		p.free(p.u32(pathv + (offs+i)*4))
	}

	p.free(pathv)
}

func (p *Process) libcGlobfree(out uint32) {
	if pathv := p.u32(out + 4); pathv != 0 {
		p.globFree(pathv, p.u32(out+8), p.u32(out))
	}

	p.put32(out, 0)
	p.put32(out+4, 0)
}

func (p *Process) libcGlob(pattern, flags, errfunc, out uint32) uint32 {
	return p.glob(pattern, flags, errfunc, out)
}
