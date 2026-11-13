//go:build linux && amd64

package main

import (
	"encoding/binary"
	"path/filepath"
	"strings"
	"sync/atomic"
	"syscall"
	"time"
)

// A server's files. A process whose root is a connection to a server
// reaches every file through it: an open file is one end of a seqpacket
// pair (kernel_endpoint.go), the server holds the other and answers on
// it. The kernel speaks the protocol here for the POSIX calls of a
// process, as the libc of the process would; the servers speak it in
// lib/nim/fs.nim. The kernel keeps nothing of a server's tree: a path goes
// to the server whole, the name of an open directory is the path it was
// opened by, and the working directory is a name.
//
// A packet is a header and a body; files ride with it.
//
//	header { op u32, arg u32, uid u32, gid u32 }
//
//	uid and gid are who asks: the process the kernel calls for, whose ids
//	are vm's; a server makes what it creates theirs. A reply's are zero.
//
//	Every request brings a fresh endpoint, last among its files: the reply
//	comes on it, and an OPEN's stays the open file's. The server answers
//	where the request came: on the endpoint of a directory a path op is
//	relative to it, ".." stays in it; on the endpoint of a node a node op
//	is the node's.
//	  OPEN      arg flags   mode u32 path OK stat; a file among the
//	                                      reply's files is the file itself
//	  STAT      arg flags   path          OK stat; AT_SYMLINK_NOFOLLOW
//	  READLINK              path          OK target
//	  MKDIR     arg mode    path
//	  UNLINK    arg flags   path          AT_REMOVEDIR
//	  RENAME                old 0 new
//	  SYMLINK               target 0 path
//	  MOUNT                 path          files: a server's root, then the reply
//	  CHMOD     arg mode    path
//	  CHOWN     arg follow  uid u32 gid u32 path
//	  UTIMES    arg when    atime u64 mtime u64 path   when: 1 follow,
//	                                      2 atime now, 4 mtime now,
//	                                      8 atime kept, 16 mtime kept
//	  TRUNCATE              size u64 path
//	  READ      arg len     off u64       OK data
//	  WRITE                 off u64 data  OK count u32
//	  READDIR               after u64     OK (ino u64, pos u64, type u8, name, 0)*
//	  FSTAT                               OK stat
//	  FCHMOD    arg mode
//	  FCHOWN                uid u32 gid u32
//	  FUTIMES   arg when    atime u64 mtime u64
//	  FTRUNCATE             size u64
//	  FSYNC     arg data
//	replies: OK, ERR with arg the errno
//	stat { ino u64, size u64, mtime u64 nanoseconds, mode u32, nlink u32,
//	       uid u32, gid u32, atime u64, dev u64 }
//
// A directory's entries have positions, each after those before it, and
// a READDIR gives those after one: removing what was read moves nothing a
// reading has to come to. dev is the server's own, the files of two
// servers never share one.
const (
	rOpen uint32 = iota + 1
	rStat
	rReadlink
	rMkdir
	rUnlink
	rRename
	rSymlink
	rMount
	rRead
	rWrite
	rReaddir
	rFstat
	rChmod
	rChown
	rUtimes
	rTruncate
	rFchmod
	rFchown
	rFutimes
	rFtruncate
	rFsync
	rOk
	rErr
	remoteHeader = 16
	remoteStat   = 56
	// The whens of UTIMES and FUTIMES.
	whenFollow    = 1
	whenAtimeNow  = 2
	whenMtimeNow  = 4
	whenAtimeKept = 8
	whenMtimeKept = 16
	utimeNow      = 1<<30 - 1
	utimeOmit     = 1<<30 - 2
	// remoteData is the most a read asks or a write sends in one packet.
	remoteData = packetMax - remoteHeader - 8
)

// remoteNode is an open file of a server: the end of its pair, and what
// the kernel keeps of it.
type remoteNode struct {
	end  *pairEnd
	path string
	refs atomic.Int32
	// accmode is the O_ACCMODE it was opened with, append its O_APPEND.
	accmode  int
	append   bool
	nonblock atomic.Bool
	// pos is the position of read, write and seek; after the position of
	// the last entry readdir gave.
	pos   int64
	after uint64
}

type remoteFile struct {
	remoteNode
}

type remoteDir struct {
	remoteNode
}

var (
	_ Transfer = (*remoteFile)(nil)
	_ Seeker   = (*remoteFile)(nil)
	_ Socket   = (*remoteFile)(nil)
	_ Attrs    = (*remoteFile)(nil)
	_ Dir      = (*remoteDir)(nil)
	_ Seeker   = (*remoteDir)(nil)
	_ Socket   = (*remoteDir)(nil)
	_ Attrs    = (*remoteDir)(nil)
	_ Dir      = (*noRoot)(nil)
)

// remoteRoot is the Dir of a connection to a server, for a root: the
// endpoint is its own, with the reference the Dir holds.
func remoteRoot(end *pairEnd) *remoteDir {
	d := &remoteDir{remoteNode{end: end, path: "/"}}
	d.refs.Store(1)

	return d
}

// asRoot is the Dir a descriptor's file is for fchdir and chroot: a
// directory, named; or a connection to a server, the root of what it
// serves, named "/" and nothing else. The reference is the caller's for
// a connection, the file's own for a directory.
func asRoot(f File) (Dir, bool) {
	if d, ok := f.(Dir); ok {
		return d, true
	}

	if c, ok := f.(interface{ endpoint() *pairEnd }); ok {
		c.endpoint().ref()

		return remoteRoot(c.endpoint()), false
	}

	return nil, false
}

func (n *remoteNode) ref() {
	n.refs.Add(1)
}

func (n *remoteNode) unref() {
	if n.refs.Add(-1) == 0 {
		n.end.unref()
	}
}

func (n *remoteNode) changes(events int16) []<-chan struct{} {
	return nil
}

func (n *remoteNode) poll(events int16) int16 {
	return events & (pollIn | pollOut | pollRdnorm | pollWrnorm)
}

func (n *remoteNode) flags() (int, error) {
	fl := n.accmode

	if n.append {
		fl |= syscall.O_APPEND
	}

	if n.nonblock.Load() {
		fl |= syscall.O_NONBLOCK
	}

	return fl, nil
}

func (n *remoteNode) setFlags(fl int) error {
	n.nonblock.Store(fl&syscall.O_NONBLOCK != 0)

	return nil
}

// The Socket role: an open file of a server is a connection to it, and a
// program that knows the protocol speaks it there itself, as init does to
// mount; the messages are the endpoint's.

func (n *remoteNode) sendmsg(p *Process, at, flags uint32) (int, error) {
	return n.end.sendmsg(p, at, flags)
}

func (n *remoteNode) recvmsg(p *Process, at, flags uint32) (int, error) {
	return n.end.recvmsg(p, at, flags)
}

func (n *remoteNode) sendto(p *Process, buf, size, flags, addr, alen uint32) (int, error) {
	return n.end.sendto(p, buf, size, flags, addr, alen)
}

func (n *remoteNode) recvfrom(p *Process, buf, size, flags uint32) (int, []byte, error) {
	return n.end.recvfrom(p, buf, size, flags)
}

func (n *remoteNode) bind(p *Process, at, size uint32) error    { return syscall.EINVAL }
func (n *remoteNode) connect(p *Process, at, size uint32) error { return syscall.EISCONN }
func (n *remoteNode) listen(backlog int) error                  { return syscall.EINVAL }
func (n *remoteNode) accept(p *Process, flags uint32) (File, []byte, error) {
	return nil, nil, syscall.EINVAL
}
func (n *remoteNode) sockname(p *Process) ([]byte, error) { return n.end.sockname(p) }
func (n *remoteNode) peername(p *Process) ([]byte, error) { return n.end.peername(p) }
func (n *remoteNode) shutdown(how int) error              { return n.end.shutdown(how) }
func (n *remoteNode) getsockopt(p *Process, level, name, at, lenAt uint32) error {
	return n.end.getsockopt(p, level, name, at, lenAt)
}
func (n *remoteNode) setsockopt(p *Process, level, name, at, size uint32) error {
	return n.end.setsockopt(p, level, name, at, size)
}

// relative is a path as a server takes it: from the directory, without
// the leading slash of the absolute path the process gave.
func relative(path string) string {
	return strings.TrimLeft(path, "/")
}

// request sends a request on an endpoint; the files go with it. The
// kernel waits as itself, not as the process: nothing of the process's
// interrupts a call to a server.
func request(to *pairEnd, op, arg uint32, body []byte, files []File) error {
	data := make([]byte, remoteHeader+len(body))
	binary.LittleEndian.PutUint32(data, op)
	binary.LittleEndian.PutUint32(data[4:], arg)
	binary.LittleEndian.PutUint32(data[8:], uint32(syscall.Geteuid()))
	binary.LittleEndian.PutUint32(data[12:], uint32(syscall.Getegid()))
	copy(data[remoteHeader:], body)

	return to.send(nil, data, files, ioMode{quiet: true})
}

// reply waits for the reply on an endpoint: the reply must come to the
// request that was sent. A server that goes away is EIO; its error is
// the reply's.
func reply(on *pairEnd) (packet, error) {
	for {
		pk, eof, err := on.recv(nil, ioMode{}, false)

		if err != nil {
			return pk, err
		}

		if eof || len(pk.data) < remoteHeader {
			return pk, syscall.EIO
		}

		if binary.LittleEndian.Uint32(pk.data) == rErr {
			dropFiles(pk.files)

			return pk, syscall.Errno(binary.LittleEndian.Uint32(pk.data[4:]))
		}

		pk.data = pk.data[remoteHeader:]

		return pk, nil
	}
}

// call is a call on the node: a fresh pair, its one end with the
// request, the reply on the other. The reply's packet and that end come
// back; the caller keeps the end for an open file or drops it.
func (n *remoteNode) call(op, arg uint32, body []byte, files []File) (packet, *pairEnd, error) {
	mine, theirs := newPair(false)

	if err := request(n.end, op, arg, body, append(files, theirs)); err != nil {
		theirs.unref()
		mine.unref()

		return packet{}, nil, err
	}

	pk, err := reply(mine)

	if err != nil {
		mine.unref()

		return packet{}, nil, err
	}

	return pk, mine, nil
}

// on is a call on the node that opens nothing: the reply's packet.
func (n *remoteNode) on(op, arg uint32, body []byte) (packet, error) {
	pk, mine, err := n.call(op, arg, body, nil)

	if err != nil {
		return packet{}, err
	}

	mine.unref()

	return pk, nil
}

func pathBody(paths ...string) []byte {
	var b []byte

	for i, path := range paths {
		if i > 0 {
			b = append(b, 0)
		}

		b = append(b, relative(path)...)
	}

	return b
}

// decodeStat is a Stat_t of the protocol's stat.
func decodeStat(b []byte, st *syscall.Stat_t) error {
	if len(b) < remoteStat {
		return syscall.EIO
	}

	le := binary.LittleEndian
	*st = syscall.Stat_t{Ino: le.Uint64(b), Size: int64(le.Uint64(b[8:])), Mode: le.Uint32(b[24:]), Nlink: uint64(le.Uint32(b[28:])),
		Uid: le.Uint32(b[32:]), Gid: le.Uint32(b[36:]), Blksize: 4096}
	st.Blocks = (st.Size + 511) / 512
	st.Mtim = syscall.NsecToTimespec(int64(le.Uint64(b[16:])))
	st.Atim = syscall.NsecToTimespec(int64(le.Uint64(b[40:])))
	st.Ctim = st.Mtim
	st.Dev = le.Uint64(b[48:])

	return nil
}

// whens is the when of UTIMES for the times of a call: nil is now for
// both, UTIME_NOW and UTIME_OMIT in a nanoseconds field are what they say.
func whens(ts []syscall.Timespec, follow bool) (uint32, []byte) {
	when := uint32(0)
	body := make([]byte, 16)

	if follow {
		when |= whenFollow
	}

	if ts == nil {
		return when | whenAtimeNow | whenMtimeNow, body
	}

	for i, t := range ts[:2] {
		now, kept := uint32(whenAtimeNow), uint32(whenAtimeKept)

		if i == 1 {
			now, kept = whenMtimeNow, whenMtimeKept
		}

		switch t.Nsec {
		case utimeNow:
			when |= now
		case utimeOmit:
			when |= kept
		default:
			binary.LittleEndian.PutUint64(body[8*i:], uint64(t.Sec)*1e9+uint64(t.Nsec))
		}
	}

	return when, body
}

// The Attrs role, of a file or a directory of a server.

func (n *remoteNode) simple(op, arg uint32, body []byte) error {
	pk, err := n.on(op, arg, body)

	if err != nil {
		return err
	}

	dropFiles(pk.files)

	return nil
}

func (n *remoteNode) truncate(size int64) error {
	var body [8]byte
	binary.LittleEndian.PutUint64(body[:], uint64(size))

	return n.simple(rFtruncate, 0, body[:])
}

func (n *remoteNode) sync(data bool) error {
	arg := uint32(0)

	if data {
		arg = 1
	}

	return n.simple(rFsync, arg, nil)
}

func (n *remoteNode) chmod(mode uint32) error {
	return n.simple(rFchmod, mode, nil)
}

func (n *remoteNode) chown(uid, gid int) error {
	var body [8]byte
	binary.LittleEndian.PutUint32(body[:], uint32(uid))
	binary.LittleEndian.PutUint32(body[4:], uint32(gid))

	return n.simple(rFchown, 0, body[:])
}

func (n *remoteNode) utimes(ts []syscall.Timespec) error {
	when, body := whens(ts, false)

	return n.simple(rFutimes, when, body)
}

func (n *remoteNode) stat(st *syscall.Stat_t) error {
	pk, err := n.on(rFstat, 0, nil)

	if err != nil {
		return err
	}

	dropFiles(pk.files)

	return decodeStat(pk.data, st)
}

// The Dir role.

func (d *remoteDir) name(p *Process) (string, error) {
	return d.path, nil
}

func (d *remoteDir) open(p *Process, path string, flags int, mode uint32) (File, error) {
	if chaosFail("openat") {
		return nil, syscall.EIO
	}

	body := make([]byte, 4)
	binary.LittleEndian.PutUint32(body, mode)
	pk, mine, err := d.call(rOpen, uint32(flags), append(body, pathBody(path)...), nil)

	if err != nil {
		return nil, err
	}

	if len(pk.files) > 0 {
		// The file itself: an object of the kernel the server hands out.
		mine.unref()
		dropFiles(pk.files[1:])

		return pk.files[0], nil
	}

	var st syscall.Stat_t

	if err := decodeStat(pk.data, &st); err != nil {
		mine.unref()

		return nil, err
	}

	node := remoteNode{end: mine, path: filepath.Clean("/" + path), accmode: flags & 3, append: flags&syscall.O_APPEND != 0}
	node.refs.Store(1)

	if st.Mode&syscall.S_IFMT == syscall.S_IFDIR {
		return &remoteDir{node}, nil
	}

	if flags&oDirectory != 0 {
		mine.unref()

		return nil, syscall.ENOTDIR
	}

	return &remoteFile{node}, nil
}

const oDirectory = 0o200000

// done drops the reply's endpoint and files of a call that opened
// nothing.
func done(pk packet, mine *pairEnd) {
	dropFiles(pk.files)
	mine.unref()
}

func (d *remoteDir) statPath(p *Process, path string, follow bool, st *syscall.Stat_t) error {
	flags := uint32(0)

	if !follow {
		flags = atSymlinkNofollow
	}

	pk, mine, err := d.call(rStat, flags, pathBody(path), nil)

	if err != nil {
		return err
	}

	defer done(pk, mine)

	return decodeStat(pk.data, st)
}

func (d *remoteDir) readlinkPath(p *Process, path string) (string, error) {
	pk, mine, err := d.call(rReadlink, 0, pathBody(path), nil)

	if err != nil {
		return "", err
	}

	defer done(pk, mine)

	return string(pk.data), nil
}

// simplePath is a call on a path that answers OK or an error.
func (d *remoteDir) simplePath(op, arg uint32, body []byte) error {
	pk, mine, err := d.call(op, arg, body, nil)

	if err != nil {
		return err
	}

	done(pk, mine)

	return nil
}

func (d *remoteDir) mkdir(p *Process, path string, mode uint32) error {
	return d.simplePath(rMkdir, mode, pathBody(path))
}

func (d *remoteDir) unlink(p *Process, path string, dir bool) error {
	flags := uint32(0)

	if dir {
		flags = atRemovedir
	}

	return d.simplePath(rUnlink, flags, pathBody(path))
}

func (d *remoteDir) rename(p *Process, old, new string) error {
	return d.simplePath(rRename, 0, pathBody(old, new))
}

// Hard links and device nodes are not in the protocol.
func (d *remoteDir) link(p *Process, old, new string, follow bool) error {
	return syscall.EPERM
}

func (d *remoteDir) symlink(p *Process, target, path string) error {
	return d.simplePath(rSymlink, 0, append(append([]byte(target), 0), relative(path)...))
}

func (d *remoteDir) mknod(p *Process, path string, mode uint32, dev uint64) error {
	return syscall.EPERM
}

// accessPath reads the file's mode: a bit asked for in any class is
// enough, as the process is the owner of all it reaches.
func (d *remoteDir) accessPath(p *Process, path string, mode, flags uint32) error {
	var st syscall.Stat_t

	if err := d.statPath(p, path, true, &st); err != nil {
		return err
	}

	for _, bit := range []uint32{4, 2, 1} {
		if mode&bit != 0 && st.Mode&(bit<<6|bit<<3|bit) == 0 {
			return syscall.EACCES
		}
	}

	return nil
}

func (d *remoteDir) chmodPath(p *Process, path string, mode uint32) error {
	return d.simplePath(rChmod, mode, pathBody(path))
}

func (d *remoteDir) chownPath(p *Process, path string, uid, gid int, follow bool) error {
	body := make([]byte, 8)
	binary.LittleEndian.PutUint32(body, uint32(uid))
	binary.LittleEndian.PutUint32(body[4:], uint32(gid))
	arg := uint32(0)

	if follow {
		arg = whenFollow
	}

	return d.simplePath(rChown, arg, append(body, pathBody(path)...))
}

func (d *remoteDir) utimesPath(p *Process, path string, ts []syscall.Timespec, follow bool) error {
	when, body := whens(ts, follow)

	return d.simplePath(rUtimes, when, append(body, pathBody(path)...))
}

func (d *remoteDir) truncatePath(p *Process, path string, size int64) error {
	body := make([]byte, 8)
	binary.LittleEndian.PutUint64(body, uint64(size))

	return d.simplePath(rTruncate, 0, append(body, pathBody(path)...))
}

// statfsPath tells a file system of nothing: no sizes, no limits but
// the name's.
func (d *remoteDir) statfsPath(p *Process, path string, st *syscall.Statfs_t) error {
	var dummy syscall.Stat_t

	if err := d.statPath(p, path, true, &dummy); err != nil {
		return err
	}

	*st = syscall.Statfs_t{Type: 0x70697a64, Bsize: 4096, Frsize: 4096, Namelen: 255}

	return nil
}

// realpath is the path cleaned, when it names something: the server's
// links stay as they are.
func (d *remoteDir) realpath(p *Process, path string) (string, error) {
	var st syscall.Stat_t

	if err := d.statPath(p, path, true, &st); err != nil {
		return "", err
	}

	return filepath.Clean("/" + path), nil
}

// readdir fills the entries after the last it gave, as many as fit, in
// the guest's struct dirent layout: ino, off, reclen, type, name; an
// entry's off is its position.
func (d *remoteDir) readdir(b []byte) (int, error) {
	var after [8]byte
	binary.LittleEndian.PutUint64(after[:], d.after)
	pk, err := d.on(rReaddir, 0, after[:])

	if err != nil {
		return 0, err
	}

	dropFiles(pk.files)
	data := pk.data
	n := 0
	le := binary.LittleEndian

	for len(data) >= 17 {
		ino, pos := le.Uint64(data), le.Uint64(data[8:])
		kind := data[16]
		end := 17

		for end < len(data) && data[end] != 0 {
			end++
		}

		name := data[17:end]
		reclen := (19 + len(name) + 1 + 7) &^ 7

		if n+reclen > len(b) {
			break
		}

		clear(b[n : n+reclen])
		le.PutUint64(b[n:], ino)
		le.PutUint64(b[n+8:], pos)
		le.PutUint16(b[n+16:], uint16(reclen))
		b[n+18] = kind
		copy(b[n+19:], name)
		n += reclen
		d.after = pos
		data = data[min(end+1, len(data)):]
	}

	return n, nil
}

// seek of a directory is rewinddir.
func (d *remoteDir) seek(off int64, whence int) (int64, error) {
	if off != 0 || whence != 0 {
		return 0, syscall.EINVAL
	}

	d.after = 0

	return 0, nil
}

// The Transfer role, of a file.

func (f *remoteFile) readAt(p *Process, b []byte, off int64) (int, error) {
	if off < 0 {
		return 0, syscall.EINVAL
	}

	if len(b) == 0 {
		return 0, nil
	}

	var body [8]byte
	binary.LittleEndian.PutUint64(body[:], uint64(off))
	pk, err := f.on(rRead, uint32(min(len(b), remoteData)), body[:])

	if err != nil {
		return 0, err
	}

	dropFiles(pk.files)

	return copy(b, pk.data), nil
}

func (f *remoteFile) writeAt(p *Process, b []byte, off int64) (int, error) {
	if off < 0 {
		return 0, syscall.EINVAL
	}

	if len(b) == 0 {
		return 0, nil
	}

	body := make([]byte, 8, 8+min(len(b), remoteData))
	binary.LittleEndian.PutUint64(body, uint64(off))
	body = append(body, b[:min(len(b), remoteData)]...)
	pk, err := f.on(rWrite, 0, body)

	if err != nil {
		return 0, err
	}

	dropFiles(pk.files)

	if len(pk.data) < 4 {
		return 0, syscall.EIO
	}

	return int(binary.LittleEndian.Uint32(pk.data)), nil
}

func (f *remoteFile) read(p *Process, b []byte, mode ioMode) (int, error) {
	if chaosFail("read") {
		return 0, syscall.EIO
	}

	n, err := f.readAt(p, b, atomic.LoadInt64(&f.pos))
	atomic.AddInt64(&f.pos, int64(n))

	return n, err
}

// write writes at the position, or at the end with O_APPEND, which the
// server's size says.
func (f *remoteFile) write(p *Process, b []byte, mode ioMode) (int, error) {
	if chaosFail("write") {
		return 0, syscall.EIO
	}

	if f.append {
		var st syscall.Stat_t

		if err := f.stat(&st); err != nil {
			return 0, err
		}

		atomic.StoreInt64(&f.pos, st.Size)
	}

	n, err := f.writeAt(p, b, atomic.LoadInt64(&f.pos))
	atomic.AddInt64(&f.pos, int64(n))

	return n, err
}

func (f *remoteFile) pread(p *Process, b []byte, off int64) (int, error) {
	return f.readAt(p, b, off)
}

func (f *remoteFile) pwrite(p *Process, b []byte, off int64) (int, error) {
	return f.writeAt(p, b, off)
}

func (f *remoteFile) seek(off int64, whence int) (int64, error) {
	base := int64(0)

	switch whence {
	case 0:
	case 1:
		base = atomic.LoadInt64(&f.pos)
	case 2:
		var st syscall.Stat_t

		if err := f.stat(&st); err != nil {
			return 0, err
		}

		base = st.Size
	default:
		return 0, syscall.EINVAL
	}

	if base+off < 0 {
		return 0, syscall.EINVAL
	}

	atomic.StoreInt64(&f.pos, base+off)

	return base + off, nil
}

// noRoot is the root of a process that has none, the first process's
// when `vm run` is given no root: an empty directory nothing is under.
type noRoot struct {
	refs atomic.Int32
	made time.Time
}

func newNoRoot() *noRoot {
	r := &noRoot{made: time.Now()}
	r.refs.Store(1)

	return r
}

func (r *noRoot) ref()   { r.refs.Add(1) }
func (r *noRoot) unref() { r.refs.Add(-1) }

func (r *noRoot) stat(st *syscall.Stat_t) error {
	*st = syscall.Stat_t{Ino: 1, Mode: syscall.S_IFDIR | 0o555, Nlink: 2, Blksize: 4096}
	st.Mtim = syscall.NsecToTimespec(r.made.UnixNano())
	st.Atim, st.Ctim = st.Mtim, st.Mtim

	return nil
}

func (r *noRoot) poll(events int16) int16 {
	return events & (pollIn | pollOut | pollRdnorm | pollWrnorm)
}

func (r *noRoot) changes(events int16) []<-chan struct{} { return nil }
func (r *noRoot) flags() (int, error)                    { return syscall.O_RDONLY, nil }
func (r *noRoot) setFlags(fl int) error                  { return nil }
func (r *noRoot) name(p *Process) (string, error)        { return "/", nil }
func (r *noRoot) readdir(b []byte) (int, error)          { return 0, nil }

// itself tells whether a path names the root itself, the one thing there
// is.
func itself(path string) bool {
	return filepath.Clean("/"+path) == "/"
}

func (r *noRoot) open(p *Process, path string, flags int, mode uint32) (File, error) {
	if !itself(path) {
		if flags&syscall.O_CREAT != 0 {
			return nil, syscall.EROFS
		}

		return nil, syscall.ENOENT
	}

	if flags&3 != syscall.O_RDONLY {
		return nil, syscall.EISDIR
	}

	r.ref()

	return r, nil
}

func (r *noRoot) statPath(p *Process, path string, follow bool, st *syscall.Stat_t) error {
	if !itself(path) {
		return syscall.ENOENT
	}

	return r.stat(st)
}

func (r *noRoot) readlinkPath(p *Process, path string) (string, error) {
	if !itself(path) {
		return "", syscall.ENOENT
	}

	return "", syscall.EINVAL
}

func (r *noRoot) mkdir(p *Process, path string, mode uint32) error             { return syscall.EROFS }
func (r *noRoot) unlink(p *Process, path string, dir bool) error               { return syscall.ENOENT }
func (r *noRoot) rename(p *Process, old, new string) error                     { return syscall.ENOENT }
func (r *noRoot) link(p *Process, old, new string, follow bool) error          { return syscall.ENOENT }
func (r *noRoot) symlink(p *Process, target, path string) error                { return syscall.EROFS }
func (r *noRoot) mknod(p *Process, path string, mode uint32, dev uint64) error { return syscall.EROFS }

func (r *noRoot) accessPath(p *Process, path string, mode, flags uint32) error {
	if !itself(path) {
		return syscall.ENOENT
	}

	if mode&2 != 0 {
		return syscall.EROFS
	}

	return nil
}

func (r *noRoot) chmodPath(p *Process, path string, mode uint32) error { return syscall.EROFS }
func (r *noRoot) chownPath(p *Process, path string, uid, gid int, follow bool) error {
	return syscall.EROFS
}
func (r *noRoot) utimesPath(p *Process, path string, ts []syscall.Timespec, follow bool) error {
	return syscall.EROFS
}
func (r *noRoot) truncatePath(p *Process, path string, size int64) error { return syscall.EISDIR }

func (r *noRoot) statfsPath(p *Process, path string, st *syscall.Statfs_t) error {
	if !itself(path) {
		return syscall.ENOENT
	}

	*st = syscall.Statfs_t{Bsize: 4096, Frsize: 4096, Namelen: 255}

	return nil
}

func (r *noRoot) realpath(p *Process, path string) (string, error) {
	if !itself(path) {
		return "", syscall.ENOENT
	}

	return "/", nil
}
