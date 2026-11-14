//go:build linux && amd64

package main

import (
	"encoding/binary"
	"sync/atomic"
	"syscall"
	"time"
)

// A server's files. A process whose root is a connection to a server
// reaches every file through it: an open file is one end of a seqpacket
// pair (kernel_endpoint.go), the server holds the other and answers on
// it. The kernel speaks the protocol here for the POSIX calls of a
// process, as the libc of the process would; rootfs speaks it as the
// servers (bin/rootfs/rootfs.cpp). The kernel walks a path as Linux does
// (kernel_path.go): RESOLVE walks it from a directory to the directory of
// its last name, and the call is then one on that name, which no server
// follows: the kernel follows a symbolic link itself. A directory the
// process holds, its root, its working directory, one a descriptor is,
// is an endpoint of the server's, not a name.
//
// A packet is a header and a body; files ride with it.
//
//	header { op u32, arg u32, uid u32, gid u32 }
//
//	uid and gid are who asks: the process the kernel calls for, whose ids
//	are vm's; a server makes what it creates theirs and lets them do what
//	its modes say, gid their group (permits). A reply's are zero.
//
//	Every request brings a fresh endpoint, last among its files: the reply
//	comes on it, and an OPEN's stays the open file's. A READ or a WRITE
//	brings before it a memfd of the memory the process reads into or
//	writes from (kernel_memfd.go): the server writes the bytes there or
//	reads them from there itself, from the memfd's byte woff on, one
//	copy, as many as asked, and closes it as it answers. A server may hand
//	a request on to another, its files with it, which then answers it. A
//	server that is gone is EIO. On the endpoint of a
//	directory a call is about the directory or about one name in it, ""
//	being the directory itself; a name is no "." or "..", has no "/" and
//	is never followed: a symbolic link is itself, and to the calls that
//	look at a name the root of a server mounted on it is what it is. On
//	the endpoint of a node a node call is the node's.
//	  RESOLVE             root dev u64, root ino u64, links u32, path
//	                      OK step u32, links u32, last u32, flags u32, name
//	  OPEN      arg flags mode u32 name   OK stat; a file among the
//	                      reply's files is the file itself
//	  STAT                name             OK stat
//	  READLINK            name             OK target
//	  MKDIR     arg mode  name
//	  MKNOD     arg mode  name             a FIFO or a regular file
//	  UNLINK    arg flags name             AT_REMOVEDIR
//	  RENAME              dev u64 ino u64 old 0 new   new a name of the
//	                      directory of device dev and inode ino
//	  LINK                dev u64 ino u64 old 0 new
//	  SYMLINK             target 0 name
//	  CHMOD     arg mode  name
//	  CHOWN               uid u32 gid u32 name
//	  UTIMES    arg when  atime u64 mtime u64 name   when: 2 atime now,
//	                      4 mtime now, 8 atime kept, 16 mtime kept
//	  TRUNCATE            size u64 name
//	  STATFS              name             OK statfs
//	  READ      arg len   off u64 woff u32 OK count u32
//	  WRITE     arg append off u64 len u32 woff u32
//	                                       OK count u32 at u64
//	  READDIR             after u64        OK (ino u64, pos u64, type u8, name, 0)*
//	  FSTAT                                OK stat
//	  FCHMOD    arg mode
//	  FCHOWN              uid u32 gid u32
//	  FUTIMES   arg when  atime u64 mtime u64
//	  FTRUNCATE           size u64
//	  FSYNC     arg data
//	  FSTATFS                              OK statfs
//	  FLOCK     arg op                     LOCK_SH, LOCK_EX, LOCK_UN
//	  LOCK      arg get   type u32 start u64 len u64   OK; with get the
//	                      lock in the way, F_UNLCK for none:
//	                      type u32 start u64 len u64
//	  PATH                root dev u64, root ino u64   OK the path from the
//	                      root of the process: a directory's, a file's by
//	                      the name it was opened by in its directory
//	replies: OK, ERR with arg the errno
//	stat { ino u64, size u64, mtime u64 nanoseconds, mode u32, nlink u32,
//	       uid u32, gid u32, atime u64, dev u64, ctime u64 }
//	statfs { blocks u64, bfree u64, files u64, ffree u64, dev u64,
//	         bsize u32, namemax u32, flags u32 }   flags: 1 read-only
//
// The root of the process is its device and inode: ".." stays there, an
// absolute link starts there. RESOLVE's step is 0 at the directory of the
// last name, which is the reply's endpoint from then on when flags has 2,
// else the directory asked; last is the kind of the last name (a name,
// ".", "..", or none: the root alone), flags 1 a slash after it. A walk to
// a root the server has no node of is step 1, with the path left to walk
// from the root. A directory's entries have positions, each after those
// before it, and a READDIR gives those after one: removing what was read
// moves nothing a reading has to come to. dev is the server's own, the
// files of two servers never share one. A WRITE with append goes at the
// end the file has when the server takes it: O_APPEND is the server's,
// so no write of another comes between. A lock is the endpoint's, an open
// file's, as the host's open file description locks are, and goes when
// it closes; a FIFO's pipe is the kernel's (kernel_fifo.go).
const (
	rOpen uint32 = iota + 1
	rStat
	rReadlink
	rMkdir
	rUnlink
	rRename
	rSymlink
	rResolve
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
	rMknod
	rLink
	rStatfs
	rFstatfs
	rFlock
	rLock
	rPath
	rOk
	rErr
	remoteHeader = 16
	remoteStat   = 64
	remoteStatfs = 52
	// The whens of UTIMES and FUTIMES.
	whenAtimeNow  = 2
	whenMtimeNow  = 4
	whenAtimeKept = 8
	whenMtimeKept = 16
	utimeNow      = 1<<30 - 1
	utimeOmit     = 1<<30 - 2
	// RESOLVE's steps and flags.
	stepDone  = 0
	stepRoot  = 1
	stepSlash = 1
	stepKept  = 2
)

// remoteNode is an open file of a server: the end of its pair, and what
// the kernel keeps of it.
type remoteNode struct {
	end  *pairEnd
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

// remoteDir is a directory of a server; dev and ino are its, once known.
type remoteDir struct {
	remoteNode
	known    atomic.Bool
	dev, ino atomic.Uint64
}

var (
	_ Transfer = (*remoteFile)(nil)
	_ Seeker   = (*remoteFile)(nil)
	_ Socket   = (*remoteFile)(nil)
	_ Attrs    = (*remoteFile)(nil)
	_ Locker   = (*remoteFile)(nil)
	_ Dir      = (*remoteDir)(nil)
	_ Seeker   = (*remoteDir)(nil)
	_ Socket   = (*remoteDir)(nil)
	_ Attrs    = (*remoteDir)(nil)
	_ Locker   = (*remoteDir)(nil)
	_ Dir      = (*noRoot)(nil)
)

// remoteDirOn is the Dir of an endpoint of a server's directory: the
// endpoint is its own, with the reference the Dir holds.
func remoteDirOn(end *pairEnd) *remoteDir {
	d := &remoteDir{remoteNode: remoteNode{end: end}}
	d.refs.Store(1)

	return d
}

// asRoot is the Dir a descriptor's file is for fchdir, with a reference
// of the caller's: a directory, or a connection to a server, the root of
// what it serves; nil for anything else.
func asRoot(f File) Dir {
	if d, ok := f.(Dir); ok {
		d.ref()

		return d
	}

	if c, ok := f.(interface{ endpoint() *pairEnd }); ok {
		c.endpoint().ref()

		return remoteDirOn(c.endpoint())
	}

	return nil
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
// program that knows the protocol may speak it there itself; the messages
// are the endpoint's.

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
// back; the caller keeps the end for an open file or drops it. files go
// with the request and stay the caller's. Only an OPEN's reply brings a
// file, the file itself, which the packet keeps; the files of any other
// reply, or the rest of an OPEN's, go here.
func (n *remoteNode) call(op, arg uint32, body []byte, files []File) (packet, *pairEnd, error) {
	mine, theirs := newPair(false)
	err := request(n.end, op, arg, body, append(files, theirs))
	theirs.unref()

	if err == syscall.EPIPE {
		err = syscall.EIO // the server is gone
	}

	if err != nil {
		mine.unref()

		return packet{}, nil, err
	}

	pk, err := reply(mine)

	if err != nil {
		mine.unref()

		return packet{}, nil, err
	}

	if op == rOpen && len(pk.files) > 0 {
		dropFiles(pk.files[1:])
		pk.files = pk.files[:1]
	} else {
		dropFiles(pk.files)
		pk.files = nil
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
	st.Dev = le.Uint64(b[48:])
	st.Ctim = syscall.NsecToTimespec(int64(le.Uint64(b[56:])))

	return nil
}

// decodeStatfs is a Statfs_t of the protocol's statfs: the server's
// device is the fsid.
func decodeStatfs(b []byte, st *syscall.Statfs_t) error {
	if len(b) < remoteStatfs {
		return syscall.EIO
	}

	le := binary.LittleEndian
	bsize := int64(le.Uint32(b[40:]))
	dev := le.Uint64(b[32:])
	*st = syscall.Statfs_t{Type: 0x70697a64, Bsize: bsize, Frsize: bsize, Blocks: le.Uint64(b), Bfree: le.Uint64(b[8:]),
		Bavail: le.Uint64(b[8:]), Files: le.Uint64(b[16:]), Ffree: le.Uint64(b[24:]), Namelen: int64(le.Uint32(b[44:])),
		Flags: int64(le.Uint32(b[48:]))}
	st.Fsid.X__val[0], st.Fsid.X__val[1] = int32(uint32(dev)), int32(uint32(dev>>32))

	return nil
}

// permits tells whether the process may read (4), write (2) or execute
// (1) what a stat describes, as a server decides it: root everything but
// executing what nobody may, the owner by the owner's bits, the process's
// group, its effective gid, by the group's, everyone else by the others'.
func permits(st *syscall.Stat_t, want uint32) bool {
	uid, gid := uint32(syscall.Geteuid()), uint32(syscall.Getegid())

	if uid == 0 {
		return want&1 == 0 || st.Mode&syscall.S_IFMT == syscall.S_IFDIR || st.Mode&0o111 != 0
	}

	bits := st.Mode

	switch {
	case uid == st.Uid:
		bits >>= 6
	case gid == st.Gid:
		bits >>= 3
	}

	return bits&want == want
}

// whens is the when of UTIMES for the times of a call: nil is now for
// both, UTIME_NOW and UTIME_OMIT in a nanoseconds field are what they say.
func whens(ts []syscall.Timespec) (uint32, []byte) {
	when := uint32(0)
	body := make([]byte, 16)

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
	_, err := n.on(op, arg, body)

	return err
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
	when, body := whens(ts)

	return n.simple(rFutimes, when, body)
}

func (n *remoteNode) stat(st *syscall.Stat_t) error {
	pk, err := n.on(rFstat, 0, nil)

	if err != nil {
		return err
	}

	return decodeStat(pk.data, st)
}

func (n *remoteNode) statfs(st *syscall.Statfs_t) error {
	pk, err := n.on(rFstatfs, 0, nil)

	if err != nil {
		return err
	}

	return decodeStatfs(pk.data, st)
}

// The Locker role: the locks are the endpoint's, the open file's.

func (n *remoteNode) flock(op int) error {
	return n.simple(rFlock, uint32(op), nil)
}

// recordLock is fcntl's lock on the bytes a struct flock says, from the
// start, the position or the end, before the start for a negative length;
// F_OFD_GETLK tells the lock in the way, from the start.
func (n *remoteNode) recordLock(cmd int, lk *syscall.Flock_t) error {
	start, length := lk.Start, lk.Len

	switch lk.Whence {
	case 0:
	case 1:
		start += atomic.LoadInt64(&n.pos)
	case 2:
		var st syscall.Stat_t

		if err := n.stat(&st); err != nil {
			return err
		}

		start += st.Size
	default:
		return syscall.EINVAL
	}

	if length < 0 {
		start, length = start+length, -length
	}

	if start < 0 {
		return syscall.EINVAL
	}

	body := make([]byte, 20)
	binary.LittleEndian.PutUint32(body, uint32(lk.Type))
	binary.LittleEndian.PutUint64(body[4:], uint64(start))
	binary.LittleEndian.PutUint64(body[12:], uint64(length))

	if cmd != fOfdGetlk {
		return n.simple(rLock, 0, body)
	}

	pk, err := n.on(rLock, 1, body)

	if err != nil {
		return err
	}

	if len(pk.data) < 20 {
		return syscall.EIO
	}

	le := binary.LittleEndian
	lk.Type, lk.Whence = int16(le.Uint32(pk.data)), 0
	lk.Start, lk.Len, lk.Pid = int64(le.Uint64(pk.data[4:])), int64(le.Uint64(pk.data[12:])), -1

	return nil
}

// The Dir role.

// name is a file's path from the root of the process: its directory's,
// and the name it was opened by.
func (n *remoteNode) name(p *Process) (string, error) {
	return n.pathFrom(p.root)
}

// pathFrom is the path the server gives the node from root.
func (n *remoteNode) pathFrom(root Dir) (string, error) {
	dev, ino := identity(root)
	var body [16]byte
	binary.LittleEndian.PutUint64(body[:], dev)
	binary.LittleEndian.PutUint64(body[8:], ino)
	pk, err := n.on(rPath, 0, body[:])

	if err != nil {
		return "", err
	}

	return string(pk.data), nil
}

// identity is a directory's device and inode, which a server finds the
// root of a process by.
func identity(d Dir) (uint64, uint64) {
	switch d := d.(type) {
	case *remoteDir:
		return d.ident()
	case *hostDir:
		var st syscall.Stat_t

		if syscall.Fstat(d.fd, &st) == nil {
			return st.Dev, st.Ino
		}
	}

	return 0, 0
}

// ident is the directory's device and inode, asked once.
func (d *remoteDir) ident() (uint64, uint64) {
	if !d.known.Load() {
		var st syscall.Stat_t

		if d.stat(&st) != nil {
			return 0, 0
		}

		d.dev.Store(st.Dev)
		d.ino.Store(st.Ino)
		d.known.Store(true)
	}

	return d.dev.Load(), d.ino.Load()
}

// resolve asks the server to walk a path from the directory: a step at
// the directory of the last name, this one or one the reply's endpoint
// is, or a walk to go on from the root of the process.
func (d *remoteDir) resolve(p *Process, path string, w *walk) (step, error) {
	dev, ino := identity(w.root)
	body := make([]byte, 20, 20+len(path))
	le := binary.LittleEndian
	le.PutUint64(body, dev)
	le.PutUint64(body[8:], ino)
	le.PutUint32(body[16:], uint32(w.links))
	pk, mine, err := d.call(rResolve, 0, append(body, path...), nil)

	if err != nil {
		return step{}, err
	}

	if len(pk.data) < 16 {
		mine.unref()

		return step{}, syscall.EIO
	}

	w.links = int(le.Uint32(pk.data[4:]))
	flags := le.Uint32(pk.data[12:])
	s := step{last: string(pk.data[16:]), kind: int(le.Uint32(pk.data[8:])), slash: flags&stepSlash != 0}

	switch {
	case le.Uint32(pk.data) == stepRoot:
		mine.unref()
		w.root.ref()

		return step{dir: w.root, more: true, rest: s.last}, nil
	case flags&stepKept != 0:
		s.dir = remoteDirOn(mine)
	default:
		mine.unref()
		d.ref()
		s.dir = d
	}

	return s, nil
}

func (d *remoteDir) path(p *Process) (string, error) {
	return d.pathFrom(p.root)
}

// done drops the reply's endpoint and files of a call that opened
// nothing.
func done(pk packet, mine *pairEnd) {
	mine.unref()
}

// simpleName is a call on a name that answers OK or an error.
func (d *remoteDir) simpleName(op, arg uint32, body []byte) error {
	pk, mine, err := d.call(op, arg, body, nil)

	if err != nil {
		return err
	}

	done(pk, mine)

	return nil
}

func (d *remoteDir) statName(p *Process, name string, st *syscall.Stat_t) error {
	pk, mine, err := d.call(rStat, 0, []byte(name), nil)

	if err != nil {
		return err
	}

	defer done(pk, mine)

	return decodeStat(pk.data, st)
}

func (d *remoteDir) readlinkName(p *Process, name string) (string, error) {
	pk, mine, err := d.call(rReadlink, 0, []byte(name), nil)

	if err != nil {
		return "", err
	}

	defer done(pk, mine)

	return string(pk.data), nil
}

func (d *remoteDir) openName(p *Process, name string, flags int, mode uint32) (File, error) {
	if chaosFail("openat") {
		return nil, syscall.EIO
	}

	body := make([]byte, 4, 4+len(name))
	binary.LittleEndian.PutUint32(body, mode)
	pk, mine, err := d.call(rOpen, uint32(flags), append(body, name...), nil)

	if err != nil {
		return nil, err
	}

	if len(pk.files) > 0 {
		// The file itself: an object of the kernel the server hands out.
		mine.unref()

		return pk.files[0], nil
	}

	var st syscall.Stat_t

	if err := decodeStat(pk.data, &st); err != nil {
		mine.unref()

		return nil, err
	}

	node := remoteNode{end: mine, accmode: flags & 3, append: flags&syscall.O_APPEND != 0}
	node.refs.Store(1)

	switch {
	case st.Mode&syscall.S_IFMT == syscall.S_IFDIR:
		dir := &remoteDir{remoteNode: node}
		dir.dev.Store(st.Dev)
		dir.ino.Store(st.Ino)
		dir.known.Store(true)

		return dir, nil
	case flags&syscall.O_DIRECTORY != 0:
		mine.unref()

		return nil, syscall.ENOTDIR
	case st.Mode&syscall.S_IFMT == syscall.S_IFIFO && flags&oPath == 0:
		// The server said who may open it; its pipe is the kernel's.
		mine.unref()

		return p.openFifo(&st, flags)
	}

	return &remoteFile{node}, nil
}

func (d *remoteDir) mkdir(p *Process, name string, mode uint32) error {
	return d.simpleName(rMkdir, mode, []byte(name))
}

// mknod makes a FIFO or a regular file; a server makes no device.
func (d *remoteDir) mknod(p *Process, name string, mode uint32, dev uint64) error {
	switch mode & syscall.S_IFMT {
	case 0, syscall.S_IFREG, syscall.S_IFIFO:
		return d.simpleName(rMknod, mode, []byte(name))
	}

	return syscall.EPERM
}

func (d *remoteDir) symlink(p *Process, target, name string) error {
	return d.simpleName(rSymlink, 0, append(append([]byte(target), 0), name...))
}

func (d *remoteDir) unlink(p *Process, name string, dir bool) error {
	flags := uint32(0)

	if dir {
		flags = atRemovedir
	}

	return d.simpleName(rUnlink, flags, []byte(name))
}

// across is the body of a call on a name of the directory and a name of
// another of the same server's, which the server knows by its device and
// inode: none for one of another server, which is EXDEV.
func (d *remoteDir) across(name string, to Dir, toName string) ([]byte, error) {
	other, ok := to.(*remoteDir)

	if !ok {
		return nil, syscall.EXDEV
	}

	dev, ino := other.ident()
	body := make([]byte, 16, 16+len(name)+1+len(toName))
	binary.LittleEndian.PutUint64(body, dev)
	binary.LittleEndian.PutUint64(body[8:], ino)

	return append(append(append(body, name...), 0), toName...), nil
}

func (d *remoteDir) rename(p *Process, name string, to Dir, toName string) error {
	body, err := d.across(name, to, toName)

	if err != nil {
		return err
	}

	return d.simpleName(rRename, 0, body)
}

// link makes a name of another directory for what a name of this one is:
// to a directory of another file system EXDEV, but EROFS when that one is
// read-only, which Linux finds first.
func (d *remoteDir) link(p *Process, name string, to Dir, toName string) error {
	body, err := d.across(name, to, toName)

	if err == nil {
		dev, _ := d.ident()

		if other, _ := to.(*remoteDir); other != nil {
			if theirs, _ := other.ident(); theirs != dev {
				err = syscall.EXDEV
			}
		}
	}

	if err == syscall.EXDEV {
		var st syscall.Statfs_t

		if to.statfsName(p, "", &st) == nil && st.Flags&1 != 0 {
			return syscall.EROFS
		}

		return err
	}

	if err != nil {
		return err
	}

	return d.simpleName(rLink, 0, body)
}

// accessName is the server's walk to the name, which a directory the
// process may not search ends, and the name's mode for the process: the
// ids are vm's, real and effective alike.
func (d *remoteDir) accessName(p *Process, name string, mode, flags uint32) error {
	var st syscall.Stat_t

	if err := d.statName(p, name, &st); err != nil {
		return err
	}

	if !permits(&st, mode&7) {
		return syscall.EACCES
	}

	return nil
}

func (d *remoteDir) chmodName(p *Process, name string, mode uint32) error {
	return d.simpleName(rChmod, mode, []byte(name))
}

func (d *remoteDir) chownName(p *Process, name string, uid, gid int) error {
	body := make([]byte, 8, 8+len(name))
	binary.LittleEndian.PutUint32(body, uint32(uid))
	binary.LittleEndian.PutUint32(body[4:], uint32(gid))

	return d.simpleName(rChown, 0, append(body, name...))
}

func (d *remoteDir) utimesName(p *Process, name string, ts []syscall.Timespec) error {
	when, body := whens(ts)

	return d.simpleName(rUtimes, when, append(body, name...))
}

func (d *remoteDir) truncateName(p *Process, name string, size int64) error {
	body := make([]byte, 8, 8+len(name))
	binary.LittleEndian.PutUint64(body, uint64(size))

	return d.simpleName(rTruncate, 0, append(body, name...))
}

// statfsName is the file system of the server a name is on.
func (d *remoteDir) statfsName(p *Process, name string, st *syscall.Statfs_t) error {
	pk, err := d.on(rStatfs, 0, []byte(name))

	if err != nil {
		return err
	}

	return decodeStatfs(pk.data, st)
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

// readAt reads into b, the process's memory, at off: the READ brings a
// memfd of b.
func (f *remoteFile) readAt(p *Process, b []byte, off int64) (int, error) {
	if off < 0 || len(b) == 0 {
		return f.readInto(nil, 0, 0, off)
	}

	w := window(p, b, syscall.O_WRONLY)
	n, err := f.readInto(w, 0, len(b), off)
	w.unref()

	return n, err
}

// readInto reads n bytes at off into the memfd w, from its byte woff on:
// the READ brings w, and the server writes the bytes there itself. A
// count past n is the server's lie: EIO.
func (f *remoteFile) readInto(w *memfdFile, woff uint32, n int, off int64) (int, error) {
	if off < 0 {
		return 0, syscall.EINVAL
	}

	if n == 0 {
		return 0, nil
	}

	var body [12]byte
	binary.LittleEndian.PutUint64(body[:], uint64(off))
	binary.LittleEndian.PutUint32(body[8:], woff)
	pk, mine, err := f.call(rRead, uint32(n), body[:], []File{w})

	if err != nil {
		return 0, err
	}

	mine.unref()

	if len(pk.data) < 4 || int(binary.LittleEndian.Uint32(pk.data)) > n {
		return 0, syscall.EIO
	}

	return int(binary.LittleEndian.Uint32(pk.data)), nil
}

func (f *remoteFile) writeAt(p *Process, b []byte, off int64) (int, error) {
	n, _, err := f.put(p, b, off, false)

	return n, err
}

// put writes b, the process's memory, at off, or atEnd at the end the
// file has when the server takes the write: the WRITE brings a memfd of b.
func (f *remoteFile) put(p *Process, b []byte, off int64, atEnd bool) (int, int64, error) {
	if off < 0 || len(b) == 0 {
		return f.writeFrom(nil, 0, 0, off, atEnd)
	}

	w := window(p, b, syscall.O_RDONLY)
	n, at, err := f.writeFrom(w, 0, len(b), off, atEnd)
	w.unref()

	return n, at, err
}

// writeFrom writes n bytes of the memfd w, from its byte woff on, at off,
// or atEnd at the end the file has when the server takes the write: the
// WRITE brings w, and the server reads the bytes from there itself. The
// count and where they went; a count past n is the server's lie: EIO.
func (f *remoteFile) writeFrom(w *memfdFile, woff uint32, n int, off int64, atEnd bool) (int, int64, error) {
	if off < 0 {
		return 0, 0, syscall.EINVAL
	}

	if n == 0 {
		return 0, off, nil
	}

	arg := uint32(0)

	if atEnd {
		arg = 1
	}

	var body [16]byte
	binary.LittleEndian.PutUint64(body[:], uint64(off))
	binary.LittleEndian.PutUint32(body[8:], uint32(n))
	binary.LittleEndian.PutUint32(body[12:], woff)
	pk, mine, err := f.call(rWrite, arg, body[:], []File{w})

	if err != nil {
		return 0, 0, err
	}

	mine.unref()

	if len(pk.data) < 12 || int(binary.LittleEndian.Uint32(pk.data)) > n {
		return 0, 0, syscall.EIO
	}

	return int(binary.LittleEndian.Uint32(pk.data)), int64(binary.LittleEndian.Uint64(pk.data[4:])), nil
}

func (f *remoteFile) read(p *Process, b []byte, mode ioMode) (int, error) {
	if chaosFail("read") {
		return 0, syscall.EIO
	}

	n, err := f.readAt(p, b, atomic.LoadInt64(&f.pos))
	atomic.AddInt64(&f.pos, int64(n))

	return n, err
}

// write writes at the position, or with O_APPEND at the end, which the
// server finds as it writes; the position is after what was written.
func (f *remoteFile) write(p *Process, b []byte, mode ioMode) (int, error) {
	if chaosFail("write") {
		return 0, syscall.EIO
	}

	n, at, err := f.put(p, b, atomic.LoadInt64(&f.pos), f.append)

	if err == nil {
		atomic.StoreInt64(&f.pos, at+int64(n))
	}

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
func (r *noRoot) readdir(b []byte) (int, error)          { return 0, nil }
func (r *noRoot) path(p *Process) (string, error)        { return "/", nil }

// resolve walks a path in the one directory there is: a name in it is
// none, but ".", ".." and the last name, which the calls find missing.
func (r *noRoot) resolve(p *Process, path string, w *walk) (step, error) {
	names, slash := splitPath(path)

	for len(names) > 1 {
		if names[0] != "." && names[0] != ".." {
			return step{}, syscall.ENOENT
		}

		names = names[1:]
	}

	r.ref()
	s := step{dir: r, kind: lastNone}

	if len(names) == 1 {
		s.last, s.kind, s.slash = lastOf(names[0], slash)
	}

	return s, nil
}

func (r *noRoot) statName(p *Process, name string, st *syscall.Stat_t) error {
	if name != "" {
		return syscall.ENOENT
	}

	return r.stat(st)
}

func (r *noRoot) readlinkName(p *Process, name string) (string, error) {
	if name != "" {
		return "", syscall.ENOENT
	}

	return "", syscall.EINVAL
}

func (r *noRoot) openName(p *Process, name string, flags int, mode uint32) (File, error) {
	switch {
	case name != "" && flags&syscall.O_CREAT != 0:
		return nil, syscall.EROFS
	case name != "":
		return nil, syscall.ENOENT
	case flags&3 != syscall.O_RDONLY:
		return nil, syscall.EISDIR
	}

	r.ref()

	return r, nil
}

func (r *noRoot) mkdir(p *Process, name string, mode uint32) error             { return syscall.EROFS }
func (r *noRoot) mknod(p *Process, name string, mode uint32, dev uint64) error { return syscall.EROFS }
func (r *noRoot) symlink(p *Process, target, name string) error                { return syscall.EROFS }
func (r *noRoot) unlink(p *Process, name string, dir bool) error               { return syscall.ENOENT }
func (r *noRoot) rename(p *Process, name string, to Dir, toName string) error  { return syscall.ENOENT }
func (r *noRoot) link(p *Process, name string, to Dir, toName string) error    { return syscall.ENOENT }

func (r *noRoot) accessName(p *Process, name string, mode, flags uint32) error {
	if name != "" {
		return syscall.ENOENT
	}

	if mode&2 != 0 {
		return syscall.EROFS
	}

	return nil
}

func (r *noRoot) chmodName(p *Process, name string, mode uint32) error { return syscall.EROFS }
func (r *noRoot) chownName(p *Process, name string, uid, gid int) error {
	return syscall.EROFS
}
func (r *noRoot) utimesName(p *Process, name string, ts []syscall.Timespec) error {
	return syscall.EROFS
}
func (r *noRoot) truncateName(p *Process, name string, size int64) error { return syscall.EISDIR }

func (r *noRoot) statfsName(p *Process, name string, st *syscall.Statfs_t) error {
	if name != "" {
		return syscall.ENOENT
	}

	*st = syscall.Statfs_t{Bsize: 4096, Frsize: 4096, Namelen: 255}

	return nil
}
