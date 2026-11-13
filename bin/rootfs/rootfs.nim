## rootfs: the system, the first process. It serves three file systems
## from one loop: the image linked into it, read-only, at the root; /dev,
## the terminal the kernel gave vm as descriptor 0, null, zero, urandom and
## random; and /tmp, a tree in its memory of at most tmpCapacity bytes, a
## quarter of what a memory of wasm32 holds. It makes itself the root of
## everything else with what POSIX has: one seqpacket pair, chroot to one
## end in the shell's process. It starts the shell on the terminal with a
## clean environment, reaps what ends under it, and ends with the shell's
## status, as a system does.
##
## A file server is a tree of nodes behind endpoints. An open file is one
## end of a seqpacket pair; the server holds the other and answers on it.
## Every request brings a fresh endpoint, last among its descriptors: the
## reply goes on it, and an OPEN's stays the open file's. The kernel
## speaks this protocol for the POSIX calls of a process
## (bin/vm/kernel_remote.go) and walks a path as Linux does: RESOLVE walks
## it here to the directory of its last name, and the call is one on that
## name, which the kernel follows itself when it is a symbolic link. /dev
## and /tmp are servers of this process mounted on names of the image
## (mountOn): a walk goes into the root of a server mounted on a name, and
## by ".." out of that root to where it is mounted.
##
## A packet is a Header and a body; descriptors ride with it (SCM_RIGHTS).
##
##   Header { op u32, arg u32, uid u32, gid u32 }
##
## uid and gid are who asks: the kernel puts its process's ids, a program
## that speaks the protocol its own. A server makes what it creates
## theirs and lets them do what its modes let them: root everything but
## executing what nobody may, the owner what the owner's bits say, the
## group of gid the group's, everyone else the others'. A path is searched
## through directories who asks may search, a name is made or removed in a
## directory they may write, and in a sticky one removed only by the
## owner of the name's node or of the directory. A reply's ids are zero.
##
##   on a directory's endpoint, about the directory or one name in it: ""
##   is the directory itself; a name is no "." or "..", has no "/" and is
##   never followed: a symbolic link is itself, and to the calls that look
##   at a name the root of a server mounted on it is what it is:
##     RESOLVE               root dev u64, root ino u64, links u32, path
##                                         OK step u32, links u32, last u32,
##                                         flags u32, name (resolve)
##     OPEN      arg flags   mode u32 name OK stat; fds[0] an object: the file is it
##     STAT                  name          OK stat
##     READLINK              name          OK target
##     MKDIR     arg mode    name
##     MKNOD     arg mode    name          a FIFO or a regular file
##     UNLINK    arg flags   name          AT_REMOVEDIR
##     RENAME                dev u64 ino u64 old 0 new   new a name of the
##                                         directory of device dev, inode ino
##     LINK                  dev u64 ino u64 old 0 new
##     SYMLINK               target 0 name
##     CHMOD     arg mode    name
##     CHOWN                 uid u32 gid u32 name
##     UTIMES    arg when    atime u64 mtime u64 name   when: 2 atime now,
##                                         4 mtime now, 8 atime kept,
##                                         16 mtime kept
##     TRUNCATE              size u64 name
##     STATFS                name          OK statfs
##   on a node's endpoint:
##     READ      arg len     off u64       OK data
##     WRITE     arg append  off u64 data  OK count u32 at u64
##     READDIR               after u64     OK (ino u64, pos u64, type u8, name, 0)*
##     FSTAT                               OK stat
##     FCHMOD    arg mode
##     FCHOWN                uid u32 gid u32
##     FUTIMES   arg when    atime u64 mtime u64
##     FTRUNCATE             size u64
##     FSYNC     arg data
##     FSTATFS                             OK statfs
##     FLOCK     arg op                    LOCK_SH, LOCK_EX, LOCK_UN
##     LOCK      arg get     type u32 start u64 len u64
##                                         OK; with get, the lock in the
##                                         way, F_UNLCK for none: type u32
##                                         start u64 len u64
##     PATH                  root dev u64, root ino u64
##                                         OK the path from the root of the
##                                         process: a directory's, a file's
##                                         by the name it was opened by
##   replies: OK, ERR with arg the errno
##   stat { ino u64, size u64, mtime u64 nanoseconds, mode u32, nlink u32,
##          uid u32, gid u32, atime u64, dev u64, ctime u64 }
##   statfs { blocks u64, bfree u64, files u64, ffree u64, dev u64,
##            bsize u32, namemax u32, flags u32 }  flags: 1 read-only
##
## The root of the process that asks is a device and an inode: ".." stays
## there, and an absolute link goes on from there. A node of this
## process's servers is found by them; a root that is no node of theirs
## sends the walk back to the kernel to go on from there.
##
## A directory's entries have positions, each after those before it: a
## READDIR gives the entries after one, so a reading that removes what it
## read misses nothing. A node has the names that are links to it, a
## directory one. dev is the server's: the number of a pair it makes as it
## makes its root, which no other pair in the kernel has.
##
## A WRITE with append writes at the end the file has when the server takes
## it, which no other write comes between. A lock is an endpoint's, an open
## file's, as Linux's open file description locks are: flock's on the
## whole file, fcntl's on a range of its bytes, refused with EAGAIN when
## another endpoint holds one in the way, and gone when the endpoint
## closes. A FIFO is a node and nothing more: the kernel makes the pipe its
## openers share. A writable server holds what its capacity lets it, in
## bytes and in nodes, a node a block's worth: past that, ENOSPC.

import std/[tables, strutils, sequtils, posix, epoll]

const
  atRemovedir = 0x200'u32
  oCreat = 0o100'u32
  oExcl = 0o200'u32
  oTrunc = 0o1000'u32
  oNofollow = 0o400000'u32
  whenAtimeNow = 2'u32
  whenMtimeNow = 4'u32
  whenAtimeKept = 8'u32
  whenMtimeKept = 16'u32
  oDirectory = 0o200000'u32
  oPath = 0o10000000'u32
  msgCmsgCloexec = 0x40000000'i32
  sockCloexec = 0o2000000'i32
  maxFds = 4
  maxPacket = 65536
  blockSize = 4096'u64
  dtFifo = 1'u8
  dtChr = 2'u8
  dtDir = 4'u8
  dtReg = 8'u8
  dtLnk = 10'u8
  sIfmt = 0o170000'u32
  sIfifo = 0o010000'u32
  sIfreg = 0o100000'u32
  sSticky = 0o1000'u32
  lockSh = 1'u32
  lockEx = 2'u32
  lockNb = 4'u32
  lockUn = 8'u32
  fRdlck = 0'u32
  fWrlck = 1'u32
  fUnlck = 2'u32
  stRdonly = 1'u32
  noId = 0xffffffff'u32
  past = high(uint64)  ## the stop of a lock to the end of a file and past it
  nameMax = 255  ## the longest name
  maxLinks = 40'u32  ## the symbolic links one path goes through, Linux's
  # RESOLVE's reply: where the walk ended, the kind of the last name and
  # what goes with it.
  stepDone = 0'u32  ## at the directory of the last name
  stepRoot = 1'u32  ## the walk goes on from the root of the process
  lastName = 0'u32
  lastDot = 1'u32
  lastDotdot = 2'u32
  lastNone = 3'u32  ## the path is "/" alone
  stepSlash = 1'u32  ## a slash ends the path
  stepKept = 2'u32  ## the reply's endpoint is the directory's from now on

type
  Op = enum
    opOpen = 1, opStat, opReadlink, opMkdir, opUnlink, opRename, opSymlink,
    opResolve, opRead, opWrite, opReaddir, opFstat, opChmod, opChown, opUtimes,
    opTruncate, opFchmod, opFchown, opFutimes, opFtruncate, opFsync,
    opMknod, opLink, opStatfs, opFstatfs, opFlock, opLock, opPath, opOk, opErr

  Header {.packed.} = object
    op, arg: uint32
    uid, gid: uint32

  Stat {.packed.} = object
    ino, size, mtime: uint64
    mode, nlink, uid, gid: uint32
    atime, dev, ctime: uint64

  Statfs {.packed.} = object
    blocks, bfree, files, ffree, dev: uint64
    bsize, namemax, flags: uint32

  NodeKind = enum
    nkDir, nkFile, nkLink
    nkFifo    ## a named pipe: the kernel makes the pipe
    nkObject  ## an object of the kernel the server hands out: a tty, a disk
    nkNull    ## reads nothing, takes everything
    nkZero    ## reads zeros
    nkRandom  ## reads the kernel's random bytes

  Range = object
    ## A lock of fcntl: an endpoint's, on the bytes [start, stop), to read
    ## or to write.
    owner: cint
    write: bool
    start, stop: uint64

  Node = ref object
    ino: uint64
    perm: uint32  ## the permission bits
    uid, gid: uint32
    atime, mtime, ctime: uint64  ## nanoseconds
    links: uint32  ## the names that are links to it
    opens: int  ## the endpoints open on it
    shared: seq[cint]  ## the endpoints that hold flock's shared lock
    exclusive: cint  ## the endpoint that holds flock's exclusive lock, or -1
    ranges: seq[Range]
    case kind: NodeKind
    of nkDir:
      parent: Node  ## where ".." goes; none for the root
      children: OrderedTable[string, Entry]  ## in the order of their positions
      entries: uint64  ## the last position given out
      served: ptr Server  ## a server of this process mounted here
    of nkFile: data: string
    of nkLink: target: string
    of nkObject: fd: cint
    of nkFifo, nkNull, nkZero, nkRandom: discard

  Entry = object
    node: Node
    pos: uint64  ## where the name is among the directory's entries

  Endpoint = object
    fd: cint
    node: Node  ## what the endpoint is open on
    reads, writes: bool  ## what its open lets it do
    dir: Node  ## the directory a file was opened in, by name
    name: string

  Server = object
    root: Node
    writable: bool
    capacity: uint64  ## the bytes a writable server's files may hold
    eps: Table[cint, Endpoint]  ## by descriptor
    inodes: uint64  ## the inode numbers given out
    nodes: uint64  ## the nodes there are
    used: uint64  ## the bytes the files hold
    dev: uint64  ## the device of the server's files
    byIno: Table[uint64, Node]  ## the nodes there are, by inode
    above: Node  ## where the server is mounted, of aboveServer
    aboveServer: ptr Server

  Place = object
    ## A node of one of this process's servers.
    s: ptr Server
    node: Node

# lib/c has these; Nim's posix does not declare them all.
proc chroot(path: cstring): cint {.importc, header: "<unistd.h>".}
proc c_signal(sig: cint, handler: proc(s: cint) {.noconv.}): pointer {.importc: "signal", header: "<signal.h>".}
proc epoll_pwait(epfd: cint, events: ptr EpollEvent, maxevents: cint, timeout: cint,
                 sigmask: ptr Sigset): cint {.importc, header: "<sys/epoll.h>".}
proc getentropy(buf: pointer, n: csize_t): cint {.importc, header: "<unistd.h>".}

# ---- packets --------------------------------------------------------------

proc fatal(why: string) =
  stderr.writeLine(why)
  quit(1)

# The packet recv takes, the descriptors that come with it and the room
# for them, and the room for the descriptors send sends: one of each for
# the process, whose loop takes a packet and is done with it before the
# next, and sends each reply whole. Nothing is made or cleared for a
# packet: its body is a part of packet.
var packet: array[maxPacket, byte]
var packetFds: seq[cint]
var packetControl: array[64, byte]
var sendControl: array[64, byte]

proc send(ep: cint, h: Header, body: openArray[byte] = [],
           fds: openArray[cint] = []) =
  var head = h
  var iov = [IOVec(iov_base: head.addr, iov_len: csize_t(sizeof(head))),
             IOVec(iov_base: if body.len > 0: body[0].unsafeAddr else: nil, iov_len: csize_t(body.len))]
  var m = Tmsghdr(msg_iov: iov[0].addr, msg_iovlen: 2)
  if fds.len > 0:
    zeroMem(sendControl[0].addr, sendControl.len)
    m.msg_control = sendControl[0].addr
    m.msg_controllen = CMSG_LEN(csize_t(fds.len * sizeof(cint)))
    let c = CMSG_FIRSTHDR(m.addr)
    c.cmsg_level = SOL_SOCKET
    c.cmsg_type = SCM_RIGHTS
    c.cmsg_len = m.msg_controllen
    copyMem(CMSG_DATA(c), fds[0].unsafeAddr, fds.len * sizeof(cint))
  discard sendmsg(SocketHandle(ep), m.addr, 0)  # a gone reader is its own loss

proc send(ep: cint, op: Op, arg: uint32, body: openArray[byte] = [],
           fds: openArray[cint] = []) =
  send(ep, Header(op: uint32(op), arg: arg), body, fds)

proc recv(ep: cint, h: var Header, fds: var seq[cint]): int =
  ## One packet into packet, its descriptors into fds: the length of its
  ## body, after the header, or -1 at the end of the endpoint.
  var iov = IOVec(iov_base: packet[0].addr, iov_len: csize_t(packet.len))
  var m = Tmsghdr(msg_iov: iov.addr, msg_iovlen: 1, msg_control: packetControl[0].addr,
                  msg_controllen: CMSG_SPACE(csize_t(maxFds * sizeof(cint))))
  let n = recvmsg(SocketHandle(ep), m.addr, msgCmsgCloexec)
  if n <= 0:
    return -1
  if n < sizeof(Header):
    fatal("a packet without a header")
  copyMem(h.addr, packet[0].addr, sizeof(Header))
  fds.setLen(0)
  var c = CMSG_FIRSTHDR(m.addr)
  while c != nil:
    if c.cmsg_level == SOL_SOCKET and c.cmsg_type == SCM_RIGHTS:
      let count = (int(c.cmsg_len) - int(CMSG_LEN(0))) div sizeof(cint)
      for i in 0 ..< count:
        fds.add cast[ptr UncheckedArray[cint]](CMSG_DATA(c))[i]
    c = CMSG_NXTHDR(m.addr, c)
  n - sizeof(Header)

proc u32(body: openArray[byte], at: var int): uint32 =
  if at + 4 > body.len:
    fatal("a short packet")
  for i in 0 ..< 4:
    result = result or (uint32(body[at + i]) shl (8 * i))
  at += 4

proc u64(body: openArray[byte], at: var int): uint64 =
  if at + 8 > body.len:
    fatal("a short packet")
  for i in 0 ..< 8:
    result = result or (uint64(body[at + i]) shl (8 * i))
  at += 8

proc str(body: openArray[byte], at: var int): string =
  ## A string of the body up to its NUL, or its end; at goes past it.
  while at < body.len and body[at] != 0:
    result.add char(body[at])
    inc at
  inc at

proc bytes[T](v: var T): seq[byte] =
  result = newSeq[byte](sizeof(T))
  copyMem(result[0].addr, v.addr, sizeof(T))

proc ok(ep: cint, body: openArray[byte] = [], fds: openArray[cint] = []) =
  send(ep, opOk, 0, body, fds)

proc err(ep: cint, e: cint) =
  send(ep, opErr, uint32(e))

# ---- the tree -------------------------------------------------------------

proc now(): uint64 =
  var ts: Timespec
  discard clock_gettime(CLOCK_REALTIME, ts)
  uint64(ts.tv_sec) * 1_000_000_000'u64 + uint64(ts.tv_nsec)

proc changed(n: Node) =
  ## A change of what the node is: its ctime.
  n.ctime = now()

proc place(dir: Node, name: string, n: Node) =
  ## Puts a name for a node in a directory, after every entry there: at the
  ## next position.
  inc dir.entries
  dir.children[name] = Entry(node: n, pos: dir.entries)
  inc n.links
  if n.kind == nkDir:
    n.parent = dir
  dir.mtime = now()
  dir.ctime = dir.mtime
  n.changed

proc device(): uint64 =
  ## A device for a server's files: the number of a pair, which no other
  ## pair in the kernel has, as it never gives a number twice.
  var ends: array[0..1, cint]
  if socketpair(AF_UNIX, SOCK_SEQPACKET or sockCloexec, 0, ends) != 0:
    fatal("socketpair")
  var st: posix.Stat
  if fstat(ends[0], st) != 0:
    fatal("fstat")
  discard close(ends[0])
  discard close(ends[1])
  uint64(st.st_ino)

proc node(s: var Server, parent: Node, name: string, kind: NodeKind): Node =
  ## A new node, with the permissions of its kind until they are set; the
  ## first gives the server its device.
  if s.dev == 0:
    s.dev = device()
  inc s.inodes
  inc s.nodes
  let t = now()
  result = Node(ino: s.inodes, kind: kind, atime: t, mtime: t, ctime: t, exclusive: -1)
  result.perm = case kind
    of nkDir: 0o755'u32
    of nkFile, nkFifo: 0o644'u32
    of nkLink: 0o777'u32
    of nkObject, nkNull, nkZero, nkRandom: 0o666'u32
  s.byIno[result.ino] = result
  if parent != nil:
    parent.place(name, result)

proc unplace(dir: Node, name: string): Node =
  ## Takes a name out of a directory: the node it named, one link fewer.
  result = dir.children[name].node
  dir.children.del(name)
  dec result.links
  dir.mtime = now()
  dir.ctime = dir.mtime
  result.changed

proc release(s: var Server, n: Node) =
  ## A node with no name and no endpoint is gone: what it held is the
  ## server's again. The root has no name, and stays.
  if n.links == 0 and n.opens == 0 and n != s.root:
    if n.kind == nkFile:
      s.used -= uint64(n.data.len)
      n.data = ""
    s.byIno.del(n.ino)
    dec s.nodes

proc full(s: Server): bool =
  ## Whether a writable server has no node left to make.
  s.capacity > 0 and s.nodes >= s.capacity div blockSize

proc resize(s: var Server, n: Node, size: int): bool =
  ## A file's size set, its bytes counted against the capacity: false when
  ## they do not fit, ENOSPC.
  let old = n.data.len
  if size > old and s.capacity > 0 and s.used + uint64(size - old) > s.capacity:
    return false
  n.data.setLen(size)
  if size > old:
    s.used += uint64(size - old)
  else:
    s.used -= uint64(old - size)
  true

proc fill(s: var Server, n: Node, data: string) =
  ## A file's data, counted as the server's: for a server that fills its
  ## tree itself, as rootfs does from its image.
  s.used -= uint64(n.data.len)
  n.data = data
  s.used += uint64(data.len)

proc mountPoint(n: Node): bool =
  ## Whether a server is mounted on the node.
  n.kind == nkDir and n.served != nil

proc may(n: Node, h: Header, want: uint32): bool =
  ## Whether who asks may read (4), write (2), and search or execute (1)
  ## the node.
  if h.uid == 0:
    return (want and 1) == 0 or n.kind == nkDir or (n.perm and 0o111) != 0
  let bits =
    if h.uid == n.uid: n.perm shr 6
    elif h.gid == n.gid: n.perm shr 3
    else: n.perm
  (bits and want) == want

proc owns(n: Node, h: Header): bool =
  h.uid == 0 or h.uid == n.uid

proc keeps(dir, n: Node, h: Header): bool =
  ## Whether a sticky directory keeps a name from who asks: neither the
  ## node it names nor the directory is theirs.
  (dir.perm and sSticky) != 0 and not n.owns(h) and h.uid != dir.uid

# ---- places ------------------------------------------------------------------

# A walk goes from place to place: into the root of a server mounted on a
# name, and by ".." from such a root to the parent of where it is mounted,
# never above the root of the process that asks.

var everyServer: seq[ptr Server]  ## this process's servers, which serve

proc at(p: Place, dev, ino: uint64): bool =
  ## Whether a place is the node of a device and inode.
  p.s.dev == dev and p.node.ino == ino

proc found(dev, ino: uint64): Place =
  ## The place of a device and inode, or none of this process.
  for s in everyServer:
    if s.dev == dev:
      return Place(s: s, node: s.byIno.getOrDefault(ino))

proc down(p: Place): Place =
  ## Where a name goes: the root of a server mounted on it, or its node.
  result = p
  while result.node.kind == nkDir and result.node.served != nil:
    let s = result.node.served
    result = Place(s: s, node: s.root)

proc up(p: Place, dev, ino: uint64): Place =
  ## Where ".." goes from a directory: nowhere from the root of the
  ## process, the node of dev and ino; from the root of a mounted server,
  ## to the parent of where it is mounted; else to the parent.
  result = p
  while not result.at(dev, ino):
    if result.node != result.s.root:
      return Place(s: result.s, node: result.node.parent)
    if result.s.above == nil:
      return
    result = Place(s: result.s.aboveServer, node: result.s.above)

proc badName(name: string): cint =
  ## Why a name is none a call takes, 0 for one it does: "." and ".." and
  ## names with "/" are a walk's; a name past nameMax is too long.
  if name == "." or name == ".." or '/' in name:
    return EINVAL
  if name.len > nameMax:
    return ENAMETOOLONG

proc named(s: ptr Server, c: Node, name: string, h: Header, e: var cint): Place =
  ## What a name of a directory is to a call that looks at it: the
  ## directory itself for "", else the name's node, or the root of a
  ## server mounted on it; e says why there is none.
  if name.len == 0:
    return Place(s: s, node: c)
  e = badName(name)
  if e == 0 and c.kind != nkDir:
    e = ENOTDIR
  elif e == 0 and not c.may(h, 1):
    e = EACCES
  elif e == 0 and name notin c.children:
    e = ENOENT
  if e == 0:
    result = Place(s: s, node: c.children[name].node).down

proc gone(s: ptr Server, c: Node): bool =
  ## Whether a directory was removed: it takes no name.
  c.kind == nkDir and c.links == 0 and c != s.root

proc creates(s: ptr Server, c: Node, name: string, h: Header): cint =
  ## Why a name cannot be made in a directory, 0 when it can. A name that
  ## is there is EEXIST before anything else, on a server that writes
  ## nothing too: mkdir -p asks for each directory.
  result = badName(name)
  if result != 0:
    return
  if c.kind != nkDir:
    return ENOTDIR
  if not c.may(h, 1):
    return EACCES
  if name in c.children:
    return EEXIST
  if s.gone(c):
    return ENOENT
  if not s.writable:
    return EROFS
  if not c.may(h, 3):
    return EACCES
  if s[].full:
    return ENOSPC

proc mode(n: Node): uint32 =
  let kind = case n.kind
    of nkDir: 0o040000'u32
    of nkFile: sIfreg
    of nkLink: 0o120000'u32
    of nkFifo: sIfifo
    of nkObject, nkNull, nkZero, nkRandom: 0o020000'u32
  kind or n.perm

proc dtype(n: Node): uint8 =
  case n.kind
  of nkDir: dtDir
  of nkFile: dtReg
  of nkLink: dtLnk
  of nkFifo: dtFifo
  of nkObject, nkNull, nkZero, nkRandom: dtChr

proc nlink(n: Node): uint32 =
  ## A directory's: its name, its ".", and the ".." of each directory in
  ## it; another's: its names.
  if n.kind != nkDir:
    return n.links
  result = 2
  for e in n.children.values:
    if e.node.kind == nkDir:
      inc result

proc statOf(s: Server, n: Node): Stat =
  result = Stat(ino: n.ino, mode: n.mode, nlink: n.nlink, uid: n.uid, gid: n.gid, atime: n.atime, mtime: n.mtime,
                dev: s.dev, ctime: n.ctime)
  case n.kind
  of nkFile: result.size = uint64(n.data.len)
  of nkLink: result.size = uint64(n.target.len)
  else: discard

proc statfsOf(s: Server): Statfs =
  ## A writable server's capacity, in blocks and in nodes, and what is
  ## left of it; a read-only one's what it holds, nothing left.
  result = Statfs(bsize: uint32(blockSize), namemax: 255, dev: s.dev)
  let used = (s.used + blockSize - 1) div blockSize
  if s.writable and s.capacity > 0:
    result.blocks = s.capacity div blockSize
    result.bfree = result.blocks - min(used, result.blocks)
    result.files = s.capacity div blockSize
    result.ffree = result.files - min(s.nodes, result.files)
  else:
    result.blocks = used
    result.files = s.nodes
  if not s.writable:
    result.flags = stRdonly

# ---- calls on paths and names ------------------------------------------------

template answer(reply: cint, e: cint) =
  ## The reply of a call that changes something: OK, or the error.
  if e == 0:
    ok(reply)
  else:
    err(reply, e)
  discard close(reply)

proc listen(s: var Server, ep: cint, node: Node = nil, reads = true, writes = false, dir: Node = nil, name = "")

proc put32(b: var seq[byte], v: uint32) =
  for i in 0 ..< 4:
    b.add byte((v shr (8 * i)) and 0xff)

proc rest(body: openArray[byte], at: int): string =
  ## The bytes of a body from at to its end, as a string.
  result = newString(max(body.len - at, 0))
  for i in at ..< body.len:
    result[i - at] = char(body[i])

proc resolve(s: ptr Server, ep: Endpoint, h: Header, body: openArray[byte], fds: seq[cint]) =
  ## A path walked from the endpoint's directory to the directory of its
  ## last name, as Linux walks one: every name but the last must be a
  ## directory who asks may search, or a symbolic link, whose target takes
  ## its place, from the root of the process when it is absolute, no more
  ## than maxLinks of them in all; ".", "..", and the root of a mounted
  ## server for its mount point, as places have it. The reply is the
  ## step, the links gone through, the kind of the last name, whether a
  ## slash ends the path and whether the reply's endpoint is now the
  ## directory's, its own; then the last name. A walk to a root of no
  ## server of this process is the step stepRoot, with the path left.
  let reply = fds[^1]
  var at = 0
  let dev = u64(body, at)
  let ino = u64(body, at)
  var links = u32(body, at)
  let path = rest(body, at)
  var names = path.split('/').filterIt(it.len > 0)
  let slash = path.endsWith("/")
  var here = Place(s: s, node: ep.node)
  var e: cint = 0
  while e == 0 and names.len > 1:
    let name = names[0]
    names.delete(0)
    if here.node.kind != nkDir:
      e = ENOTDIR
    elif not here.node.may(h, 1):
      e = EACCES
    elif name == ".":
      discard
    elif name == "..":
      here = here.up(dev, ino)
    elif name.len > nameMax:
      e = ENAMETOOLONG
    elif name notin here.node.children:
      e = ENOENT
    else:
      let next = Place(s: here.s, node: here.node.children[name].node).down
      if next.node.kind != nkLink:
        here = next
      elif links >= maxLinks:
        e = ELOOP
      elif next.node.target.len == 0:
        e = ENOENT
      else:
        inc links
        names = next.node.target.split('/').filterIt(it.len > 0) & names
        if next.node.target[0] == '/':
          here = found(dev, ino)
          if here.node == nil:
            var head: seq[byte]
            head.put32(stepRoot)
            head.put32(links)
            head.put32(lastNone)
            head.put32(0)
            let left = names.join("/") & (if slash: "/" else: "")
            ok(reply, head & @(left.toOpenArrayByte(0, left.high)))
            discard close(reply)
            return
  if e == 0 and here.node.kind != nkDir:
    e = ENOTDIR
  if e == 0 and names.len > 0 and not here.node.may(h, 1):
    e = EACCES
  var last = lastNone
  var name = ""
  if e == 0 and names.len > 0:
    name = names[0]
    if name == ".":
      last = lastDot
      name = ""
    elif name == "..":
      last = lastDotdot
      name = ""
      here = here.up(dev, ino)
    elif name.len > nameMax:
      e = ENAMETOOLONG
    else:
      last = lastName
  if e != 0:
    err(reply, e)
    discard close(reply)
    return
  let kept = here.s != s or here.node != ep.node
  var head: seq[byte]
  head.put32(stepDone)
  head.put32(links)
  head.put32(last)
  head.put32((if slash and last == lastName: stepSlash else: 0'u32) or (if kept: stepKept else: 0'u32))
  if kept:
    here.s[].listen(reply, here.node, reads = false)
    ok(reply, head & @(name.toOpenArrayByte(0, name.high)))
  else:
    ok(reply, head & @(name.toOpenArrayByte(0, name.high)))
    discard close(reply)

proc pathOf(s: ptr Server, ep: Endpoint, h: Header, body: openArray[byte], fds: seq[cint]) =
  ## The path of an endpoint's directory from the root of the process,
  ## the node of a device and inode, up through the places ".." goes to;
  ## of a file, its directory's and the name it was opened by. A removed
  ## directory has none, nor one the root is not above: ENOENT.
  let reply = fds[^1]
  var at = 0
  let dev = u64(body, at)
  let ino = u64(body, at)
  var here = Place(s: s, node: ep.node)
  var names: seq[string]
  var e: cint = 0
  if here.node.kind != nkDir:
    if ep.dir == nil:
      e = ENOENT
    else:
      names.add ep.name
      here = Place(s: s, node: ep.dir)
  while e == 0 and not here.at(dev, ino):
    if here.node == here.s.root:
      if here.s.above == nil:
        e = ENOENT
      else:
        here = Place(s: here.s.aboveServer, node: here.s.above)
      continue
    if here.s.gone(here.node):
      e = ENOENT
      continue
    let parent = here.node.parent
    var name = ""
    for k, entry in parent.children:
      if entry.node == here.node:
        name = k
        break
    if name.len == 0:
      e = ENOENT
    names.add name
    here = Place(s: here.s, node: parent)
  if e != 0:
    err(reply, e)
  else:
    var path = ""
    for i in countdown(names.high, 0):
      path.add "/"
      path.add names[i]
    if path.len == 0:
      path = "/"
    ok(reply, path.toOpenArrayByte(0, path.high))
  discard close(reply)

proc open(s: ptr Server, ep: Endpoint, h: Header, body: openArray[byte], fds: seq[cint]) =
  ## A name of the directory opened, or the directory itself for "": read
  ## or written as its access mode says and who asks may, made when
  ## O_CREAT asks and it is not there, emptied when O_TRUNC asks; with
  ## O_PATH only held, for a working directory or a root. A symbolic link
  ## is ELOOP, for the kernel to follow, or held itself with O_PATH and
  ## O_NOFOLLOW.
  let reply = fds[^1]
  let c = ep.node
  var at = 0
  let mode = u32(body, at)
  let name = str(body, at)
  let opath = (h.arg and oPath) != 0
  let access = h.arg and 3
  let reads = not opath and access != 1
  let writes = not opath and access != 0
  let truncates = not opath and (h.arg and oTrunc) != 0
  let creating = (h.arg and oCreat) != 0 and name.len > 0
  let there = name.len > 0 and c.kind == nkDir and name in c.children
  var e: cint = 0
  var target: Place
  var made = false
  if creating and there and (h.arg and oExcl) != 0:
    e = EEXIST
  elif creating and not there:
    e = s.creates(c, name, h)
    if e == 0:
      let n = s[].node(c, name, nkFile)
      n.perm = mode and 0o7777
      n.uid = h.uid
      n.gid = h.gid
      target = Place(s: s, node: n)
      made = true
  else:
    target = s.named(c, name, h, e)
  if e == 0 and target.node.kind == nkLink and not (opath and (h.arg and oNofollow) != 0):
    e = ELOOP
  elif e == 0 and (h.arg and oDirectory) != 0 and target.node.kind != nkDir:
    e = ENOTDIR
  elif e == 0 and not made and not opath:
    let want = (if reads: 4'u32 else: 0'u32) or (if writes or truncates: 2'u32 else: 0'u32)
    if not target.node.may(h, want):
      e = EACCES
    elif target.node.kind == nkDir and (writes or (h.arg and oCreat) != 0):
      e = EISDIR
    elif (writes or truncates) and target.node.kind == nkFile and not target.s.writable:
      e = EROFS
  if e != 0:
    answer(reply, e)
    return
  if target.node.kind == nkObject and not opath:
    ok(reply, fds = [target.node.fd])  # the object itself becomes the caller's
    discard close(reply)
    return
  if truncates and target.node.kind == nkFile and target.node.data.len > 0:
    discard target.s[].resize(target.node, 0)
    target.node.mtime = now()
    target.node.changed
  var st = target.s[].statOf(target.node)
  target.s[].listen(reply, target.node, reads, writes, (if name.len > 0: c else: nil), name)
  ok(reply, st.bytes)

proc stat(s: ptr Server, ep: Endpoint, h: Header, body: openArray[byte], fds: seq[cint]) =
  let reply = fds[^1]
  var at = 0
  var e: cint = 0
  let t = s.named(ep.node, str(body, at), h, e)
  if e != 0:
    answer(reply, e)
    return
  var st = t.s[].statOf(t.node)
  ok(reply, st.bytes)
  discard close(reply)

proc statfs(s: ptr Server, ep: Endpoint, h: Header, body: openArray[byte], fds: seq[cint]) =
  ## The file system of a name: the server's, or the one mounted there.
  let reply = fds[^1]
  var at = 0
  var e: cint = 0
  let t = s.named(ep.node, str(body, at), h, e)
  if e != 0:
    answer(reply, e)
    return
  var st = t.s[].statfsOf
  ok(reply, st.bytes)
  discard close(reply)

proc readlink(s: ptr Server, ep: Endpoint, h: Header, body: openArray[byte], fds: seq[cint]) =
  let reply = fds[^1]
  var at = 0
  var e: cint = 0
  let t = s.named(ep.node, str(body, at), h, e)
  if e == 0 and t.node.kind != nkLink:
    e = EINVAL
  if e != 0:
    answer(reply, e)
    return
  ok(reply, t.node.target.toOpenArrayByte(0, t.node.target.high))
  discard close(reply)

proc create(s: ptr Server, ep: Endpoint, h: Header, body: openArray[byte], fds: seq[cint], op: Op) =
  ## mkdir, mknod and symlink: a new name in the directory, who asks may
  ## write, theirs, with the mode asked for. mknod makes FIFOs and regular
  ## files; a device is EPERM.
  let reply = fds[^1]
  let c = ep.node
  var at = 0
  let target = if op == opSymlink: str(body, at) else: ""
  let name = str(body, at)
  var kind = nkDir
  var e = s.creates(c, name, h)
  if e == 0 and op == opSymlink:
    kind = nkLink
  elif e == 0 and op == opMknod:
    case h.arg and sIfmt
    of sIfifo: kind = nkFifo
    of 0, sIfreg: kind = nkFile
    else: e = EPERM
  if e == 0:
    let n = s[].node(c, name, kind)
    n.uid = h.uid
    n.gid = h.gid
    if kind == nkLink:
      n.target = target
    else:
      n.perm = h.arg and 0o7777
  answer(reply, e)

proc unlink(s: ptr Server, ep: Endpoint, h: Header, body: openArray[byte], fds: seq[cint]) =
  let reply = fds[^1]
  let c = ep.node
  var at = 0
  let name = str(body, at)
  let removeDir = (h.arg and atRemovedir) != 0
  var e = badName(name)
  if e == 0 and c.kind != nkDir:
    e = ENOTDIR
  elif e == 0 and not c.may(h, 1):
    e = EACCES
  elif e == 0 and not s.writable:
    e = EROFS
  elif e == 0 and name notin c.children:
    e = ENOENT
  elif e == 0:
    let n = c.children[name].node
    if removeDir and n.kind != nkDir:
      e = ENOTDIR
    elif not removeDir and n.kind == nkDir:
      e = EISDIR
    elif not c.may(h, 3):
      e = EACCES
    elif c.keeps(n, h):
      e = EPERM
    elif removeDir and n.mountPoint:
      e = EBUSY
    elif removeDir and n.children.len > 0:
      e = ENOTEMPTY
    else:
      s[].release(c.unplace(name))
  answer(reply, e)

proc within(n, dir: Node): bool =
  ## Whether dir is n or under it.
  var up = dir
  while up != nil:
    if up == n:
      return true
    up = up.parent
  false

proc directory(s: ptr Server, dev, ino: uint64, e: var cint): Node =
  ## The directory of a device and inode another name of a call is in:
  ## EXDEV on another server, ENOENT when it is no directory there now.
  if e != 0:
    return
  if dev != s.dev:
    e = EXDEV
    return
  result = s.byIno.getOrDefault(ino)
  if result == nil or result.kind != nkDir or s.gone(result):
    e = ENOENT
    result = nil

proc rename(s: ptr Server, ep: Endpoint, h: Header, body: openArray[byte], fds: seq[cint]) =
  ## A name of the directory moved to a name of another of the server's,
  ## of a device and inode: on another server EXDEV. What the new name
  ## names goes, when it is of the old one's kind, and empty if a
  ## directory; a directory does not go under itself, nor a mount point
  ## anywhere, and one that goes to another directory must be one who
  ## asks may write, for its "..".
  let reply = fds[^1]
  let src = ep.node
  var at = 0
  let dev = u64(body, at)
  let ino = u64(body, at)
  let oldName = str(body, at)
  let newName = str(body, at)
  var e = badName(oldName)
  if e == 0:
    e = badName(newName)
  let dst = s.directory(dev, ino, e)
  if e == 0 and src.kind != nkDir:
    e = ENOTDIR
  elif e == 0 and (not src.may(h, 1) or not dst.may(h, 1)):
    e = EACCES
  elif e == 0 and not s.writable:
    e = EROFS
  elif e == 0 and oldName notin src.children:
    e = ENOENT
  elif e == 0:
    let n = src.children[oldName].node
    let old = if newName in dst.children: dst.children[newName].node else: nil
    if old == n:
      discard
    elif n.kind == nkDir and within(n, dst):
      e = EINVAL
    elif not src.may(h, 3) or not dst.may(h, 3):
      e = EACCES
    elif src.keeps(n, h) or old != nil and dst.keeps(old, h):
      e = EPERM
    elif n.kind == nkDir and src != dst and not n.may(h, 2):
      e = EACCES
    elif n.mountPoint or old != nil and old.mountPoint:
      e = EBUSY
    elif old != nil and n.kind == nkDir and old.kind != nkDir:
      e = ENOTDIR
    elif old != nil and n.kind != nkDir and old.kind == nkDir:
      e = EISDIR
    elif old != nil and old.kind == nkDir and old.children.len > 0:
      e = ENOTEMPTY
    else:
      discard src.unplace(oldName)
      if old != nil:
        s[].release(dst.unplace(newName))
      dst.place(newName, n)
  answer(reply, e)

proc link(s: ptr Server, ep: Endpoint, h: Header, body: openArray[byte], fds: seq[cint]) =
  ## A new name, in a directory of the server's of a device and inode, for
  ## the node a name of this one is, which is no directory.
  let reply = fds[^1]
  let src = ep.node
  var at = 0
  let dev = u64(body, at)
  let ino = u64(body, at)
  let oldName = str(body, at)
  let newName = str(body, at)
  var e = badName(oldName)
  if e == 0 and src.kind != nkDir:
    e = ENOTDIR
  elif e == 0 and not src.may(h, 1):
    e = EACCES
  elif e == 0 and oldName notin src.children:
    e = ENOENT
  let dst = s.directory(dev, ino, e)
  if e == 0:
    e = s.creates(dst, newName, h)
  if e == 0 and src.children[oldName].node.kind == nkDir:
    e = EPERM
  if e == 0:
    dst.place(newName, src.children[oldName].node)
  answer(reply, e)

# The attributes of a node: set when the server is writable, by its owner
# or root, a group given away by the owner only to its own, the times set
# to now by who may write it too, a size by who may write it.

proc setTimes(node: Node, mask: uint32, atime, mtime: uint64) =
  if (mask and whenAtimeKept) == 0:
    node.atime = (if (mask and whenAtimeNow) != 0: now() else: atime)
  if (mask and whenMtimeKept) == 0:
    node.mtime = (if (mask and whenMtimeNow) != 0: now() else: mtime)

proc change(s: var Server, node: Node, op: Op, h: Header, body: openArray[byte]): cint =
  ## chmod, chown, utimes or truncate of a node, by name or by endpoint,
  ## their arguments in body: 0, or the error.
  var at = 0
  case op
  of opChmod, opFchmod:
    if not node.owns(h):
      return EPERM
    node.perm = h.arg and 0o7777
  of opChown, opFchown:
    let uid = u32(body, at)
    let gid = u32(body, at)
    let toUid = if uid == noId: node.uid else: uid
    let toGid = if gid == noId: node.gid else: gid
    if h.uid != 0 and (h.uid != node.uid or toUid != node.uid or toGid != node.gid and toGid != h.gid):
      return EPERM
    if (toUid != node.uid or toGid != node.gid) and node.kind != nkDir:
      node.perm = node.perm and not 0o6000'u32
    node.uid = toUid
    node.gid = toGid
  of opUtimes, opFutimes:
    let atime = u64(body, at)
    let mtime = u64(body, at)
    let touch = (h.arg and whenAtimeNow) != 0 and (h.arg and whenMtimeNow) != 0
    if touch and not node.owns(h) and not node.may(h, 2):
      return EACCES
    if not touch and not node.owns(h):
      return EPERM
    node.setTimes(h.arg, atime, mtime)
  of opTruncate, opFtruncate:
    let size = u64(body, at)
    if node.kind == nkDir:
      return (if op == opTruncate: EISDIR else: EINVAL)
    if node.kind != nkFile or size > uint64(high(int32)) * 1024:
      return EINVAL
    if op == opTruncate and not node.may(h, 2):
      return EACCES
    if not s.resize(node, int(size)):
      return ENOSPC
    node.mtime = now()
  else: discard
  node.changed
  0

proc attr(s: ptr Server, ep: Endpoint, h: Header, body: openArray[byte], fds: seq[cint]) =
  ## chmod, chown, utimes and truncate of a name, or of the directory.
  let reply = fds[^1]
  let op = Op(h.op)
  let ahead = case op
    of opChown, opTruncate: 8
    of opUtimes: 16
    else: 0
  var at = ahead
  var e: cint = 0
  let t = s.named(ep.node, str(body, at), h, e)
  if e == 0 and not t.s.writable:
    e = EROFS
  if e == 0:
    e = t.s[].change(t.node, op, h, body.toOpenArray(0, ahead - 1))
  answer(reply, e)

proc fattr(s: var Server, reply: cint, ep: Endpoint, h: Header, body: openArray[byte]) =
  ## fchmod, fchown, futimes, ftruncate and fsync of an open node; only an
  ## endpoint open to write truncates.
  let op = Op(h.op)
  if op == opFsync:
    ok(reply)
  elif not s.writable:
    err(reply, EROFS)
  elif op == opFtruncate and not ep.writes:
    err(reply, EINVAL)
  else:
    let e = s.change(ep.node, op, h, body)
    if e == 0: ok(reply) else: err(reply, e)

# ---- calls on a node ---------------------------------------------------------

# offset is a request's offset, or EINVAL answered for one no file has.
proc offset(reply: cint, body: openArray[byte], off: var int): bool =
  var at = 0
  let raw = u64(body, at)
  if raw > uint64(high(int)):
    err(reply, EINVAL)
    return false
  off = int(raw)
  true

proc read(s: var Server, reply: cint, ep: Endpoint, h: Header, body: openArray[byte]) =
  var off: int
  if not offset(reply, body, off):
    return
  let node = ep.node
  let n = int(h.arg)
  if node.kind != nkDir and not ep.reads:
    err(reply, EBADF)
    return
  case node.kind
  of nkFile:
    let count = min(n, max(node.data.len - off, 0))
    if count == 0:
      ok(reply)
    else:
      ok(reply, node.data.toOpenArrayByte(off, off + count - 1))
  of nkNull:
    ok(reply)
  of nkZero:
    ok(reply, newSeq[byte](min(n, maxPacket - sizeof(Header))))
  of nkRandom:
    var bytes = newSeq[byte](min(n, maxPacket - sizeof(Header)))
    var at = 0
    while at < bytes.len:
      let chunk = min(256, bytes.len - at)
      if getentropy(bytes[at].addr, csize_t(chunk)) != 0:
        err(reply, EIO)
        return
      at += chunk
    ok(reply, bytes)
  else:
    err(reply, EISDIR)

proc write(s: var Server, reply: cint, ep: Endpoint, h: Header, body: openArray[byte]) =
  ## The data at the offset, or at the end with append, as much as the
  ## capacity takes: the count and where it went.
  var off: int
  if not offset(reply, body, off):
    return
  let node = ep.node
  let data = body[8 .. ^1]
  if node.kind != nkDir and not ep.writes:
    err(reply, EBADF)
    return
  case node.kind
  of nkNull, nkZero, nkRandom:
    var reply32 = uint32(data.len)
    var where = uint64(off)
    ok(reply, reply32.bytes & where.bytes)
  of nkFile:
    if not s.writable:
      err(reply, EROFS)
      return
    if (h.arg and 1) != 0:
      off = node.data.len
    var count = data.len
    let grows = max(off + count - node.data.len, 0)
    if s.capacity > 0 and uint64(grows) > s.capacity - s.used:
      count = max(count - (grows - int(s.capacity - s.used)), 0)
    if count == 0 and data.len > 0:
      err(reply, ENOSPC)
      return
    if node.data.len < off + count:
      discard s.resize(node, off + count)
    for i in 0 ..< count:
      node.data[off + i] = char(data[i])
    node.mtime = now()
    node.changed
    var written = uint32(count)
    var where = uint64(off)
    ok(reply, written.bytes & where.bytes)
  else:
    err(reply, EISDIR)

proc readdir(s: var Server, reply: cint, node: Node, h: Header, body: openArray[byte]) =
  ## The entries after a position, in order, as many as one reply holds.
  if node.kind != nkDir:
    err(reply, ENOTDIR)
    return
  var at = 0
  let after = u64(body, at)
  var entries: seq[byte]
  for name, entry in node.children:
    if entry.pos <= after:
      continue
    if entries.len + 17 + name.len + 1 > maxPacket - sizeof(Header):
      break
    var ino = entry.node.ino
    var pos = entry.pos
    entries.add ino.bytes
    entries.add pos.bytes
    entries.add entry.node.dtype
    entries.add name.toOpenArrayByte(0, name.high)
    entries.add 0
  ok(reply, entries)

proc flock(s: var Server, reply: cint, ep: Endpoint, h: Header) =
  ## flock's lock of the endpoint's node: shared, which no other
  ## endpoint's exclusive one is in the way of, exclusive, which no other
  ## endpoint's lock is, or none.
  let n = ep.node
  let others = n.shared.filterIt(it != ep.fd)
  let other = n.exclusive >= 0 and n.exclusive != ep.fd
  case h.arg and not lockNb
  of lockUn:
    n.shared = others
    if n.exclusive == ep.fd:
      n.exclusive = -1
  of lockSh:
    if other:
      err(reply, EAGAIN)
      return
    n.exclusive = -1
    n.shared = others & @[ep.fd]
  of lockEx:
    if other or others.len > 0:
      err(reply, EAGAIN)
      return
    n.shared = others
    n.exclusive = ep.fd
  else:
    err(reply, EINVAL)
    return
  ok(reply)

proc lock(s: var Server, reply: cint, ep: Endpoint, h: Header, body: openArray[byte]) =
  ## fcntl's locks of the endpoint's node, on a range, to the end and past
  ## with a length of 0. With get, the first lock of another endpoint in
  ## the way of the one described, else F_UNLCK. Else that lock is set in
  ## place of what the endpoint held of the range, unless another
  ## endpoint's lock is in the way: EAGAIN; F_UNLCK lets the range go.
  var at = 0
  let kind = u32(body, at)
  let start = u64(body, at)
  let length = u64(body, at)
  let stop = if length == 0 or start > past - length: past else: start + length
  if kind > fUnlck:
    err(reply, EINVAL)
    return
  let n = ep.node
  var way = -1
  for i, l in n.ranges:
    if l.owner != ep.fd and l.start < stop and start < l.stop and (l.write or kind == fWrlck):
      way = i
      break
  if h.arg == 1:
    var found = Range(write: false, start: start, stop: stop)
    var t = fUnlck
    if way >= 0:
      found = n.ranges[way]
      t = if found.write: fWrlck else: fRdlck
    var b = t.bytes
    var from64 = found.start
    var len64 = if found.stop == past: 0'u64 else: found.stop - found.start
    if way < 0:
      from64 = start
      len64 = length
    b.add from64.bytes
    b.add len64.bytes
    ok(reply, b)
    return
  if kind != fUnlck and way >= 0:
    err(reply, EAGAIN)
    return
  var kept: seq[Range]
  for l in n.ranges:
    if l.owner != ep.fd or l.stop <= start or l.start >= stop:
      kept.add l
      continue
    if l.start < start:
      kept.add Range(owner: l.owner, write: l.write, start: l.start, stop: start)
    if l.stop > stop:
      kept.add Range(owner: l.owner, write: l.write, start: stop, stop: l.stop)
  if kind != fUnlck:
    kept.add Range(owner: ep.fd, write: kind == fWrlck, start: start, stop: stop)
  n.ranges = kept
  ok(reply)

# ---- the loop ---------------------------------------------------------------
#
# One epoll of the process watches every endpoint of its servers: an
# endpoint joins it when it is listened on, leaves it before it closes.

const EPOLL_CLOEXEC = 0o2000000'i32

var watcher = -1'i32

# The signal mask of the wait, once reapOnChild blocked SIGCHLD everywhere
# else: a child's end that comes while the loop works waits for the wait,
# which it interrupts at once, instead of coming before it, unseen.
var waitMask: Sigset
var waitMasked = false

proc watch(fd: cint) =
  if watcher >= 0:
    var ev: EpollEvent
    ev.events = uint32(EPOLLIN)
    ev.data.fd = fd
    if epoll_ctl(watcher, EPOLL_CTL_ADD, fd, ev.addr) != 0:
      fatal("fs: epoll_ctl")

proc unwatch(fd: cint) =
  if watcher >= 0:
    discard epoll_ctl(watcher, EPOLL_CTL_DEL, fd, nil)

proc listen(s: var Server, ep: cint, node: Node = nil, reads = true, writes = false, dir: Node = nil, name = "") =
  ## An endpoint the server answers on, open on node, or on the root, to
  ## read, to write or both; a file opened by a name of dir says it.
  discard fcntl(ep, F_SETFD, FD_CLOEXEC)
  let n = if node == nil: s.root else: node
  inc n.opens
  s.eps[ep] = Endpoint(fd: ep, node: n, reads: reads, writes: writes, dir: dir, name: name)
  watch(ep)

proc forget(s: var Server, ep: Endpoint) =
  ## An endpoint that closes: its locks go with it, and its node when no
  ## name is left of it.
  let n = ep.node
  n.shared = n.shared.filterIt(it != ep.fd)
  if n.exclusive == ep.fd:
    n.exclusive = -1
  n.ranges = n.ranges.filterIt(it.owner != ep.fd)
  dec n.opens
  s.release(n)

proc dispatch(s: var Server, ep: Endpoint, h: Header, body: openArray[byte], fds: seq[cint]) =
  ## A request on an endpoint.
  when defined(fsDebug):
    stderr.writeLine("fs: op " & $h.op & " arg " & $h.arg & " body " & $body.len & " fds " & $fds.len)
  if fds.len == 0:
    return  # a request without its reply endpoint: nobody to answer
  let reply = fds[^1]
  if h.op < uint32(opOpen) or h.op > uint32(opErr):
    err(reply, ENOSYS)
    discard close(reply)
    return
  case Op(h.op)
  of opResolve: s.addr.resolve(ep, h, body, fds)
  of opPath: s.addr.pathOf(ep, h, body, fds)
  of opOpen: s.addr.open(ep, h, body, fds)
  of opStat: s.addr.stat(ep, h, body, fds)
  of opStatfs: s.addr.statfs(ep, h, body, fds)
  of opReadlink: s.addr.readlink(ep, h, body, fds)
  of opMkdir, opMknod, opSymlink: s.addr.create(ep, h, body, fds, Op(h.op))
  of opUnlink: s.addr.unlink(ep, h, body, fds)
  of opRename: s.addr.rename(ep, h, body, fds)
  of opLink: s.addr.link(ep, h, body, fds)
  of opChmod, opChown, opUtimes, opTruncate: s.addr.attr(ep, h, body, fds)
  of opFchmod, opFchown, opFutimes, opFtruncate, opFsync:
    s.fattr(reply, ep, h, body)
    discard close(reply)
  of opRead:
    s.read(reply, ep, h, body)
    discard close(reply)
  of opWrite:
    s.write(reply, ep, h, body)
    discard close(reply)
  of opReaddir:
    s.readdir(reply, ep.node, h, body)
    discard close(reply)
  of opFstat:
    var st = s.statOf(ep.node)
    ok(reply, st.bytes)
    discard close(reply)
  of opFstatfs:
    var st = s.statfsOf
    ok(reply, st.bytes)
    discard close(reply)
  of opFlock:
    s.flock(reply, ep, h)
    discard close(reply)
  of opLock:
    s.lock(reply, ep, h, body)
    discard close(reply)
  of opOk, opErr:
    for fd in fds:
      discard close(fd)

proc handle(s: var Server, fd: cint) =
  let ep = s.eps[fd]
  var h: Header
  let n = recv(ep.fd, h, packetFds)
  if n < 0:
    unwatch(ep.fd)
    s.forget(ep)
    discard close(ep.fd)  # the other end closed: the file is closed
    s.eps.del(fd)
    return
  s.dispatch(ep, h, packet.toOpenArray(sizeof(Header), sizeof(Header) + n - 1), packetFds)

proc serve(servers: openArray[ptr Server], idle: proc(): bool = nil) =
  ## Serves the servers, one process's, until the last endpoint of them
  ## all closes: then nobody can reach them. idle runs after every wait,
  ## interrupted ones too, and ends the loop by returning false.
  everyServer = @servers
  watcher = epoll_create1(EPOLL_CLOEXEC)
  if watcher < 0:
    fatal("fs: epoll_create1")
  for s in servers:
    for fd in s[].eps.keys:
      watch(fd)
  var events: array[64, EpollEvent]
  while true:
    var open = 0
    for s in servers:
      open += s[].eps.len
    if open == 0:
      discard close(watcher)
      watcher = -1
      return
    let n = epoll_pwait(watcher, events[0].addr, cint(events.len), -1, if waitMasked: waitMask.addr else: nil)
    when defined(fsDebug):
      stderr.writeLine("fs: epoll_wait " & $n & " errno " & $errno & " eps " & $open)
    if idle != nil and not idle():
      return
    if n <= 0:
      continue
    for i in 0 ..< n:
      let fd = cint(events[i].data.fd)
      for s in servers:
        if fd in s[].eps:
          s[].handle(fd)
          break

proc serve(s: var Server, idle: proc(): bool = nil) =
  serve([s.addr], idle)

# ---- for programs that make servers and mounts ------------------------------

proc pair(): (cint, cint) =
  ## A seqpacket pair: one end for a server, the other for its clients.
  var ends: array[0..1, cint]
  if socketpair(AF_UNIX, SOCK_SEQPACKET or sockCloexec, 0, ends) != 0:
    fatal("socketpair")
  (ends[0], ends[1])

proc mountOn(s: var Server, path: string, target: ptr Server) =
  ## Mounts a server of this process on a directory of ours, a path of
  ## names from our root: a walk goes into its root there, and by ".."
  ## from its root back here.
  var n = s.root
  for name in path.split('/'):
    if name.len > 0:
      if n.kind != nkDir or name notin n.children:
        fatal("mount on " & path)
      n = n.children[name].node
  if n.kind != nkDir:
    fatal("mount on " & path)
  n.served = target
  target.above = n
  target.aboveServer = s.addr

proc reapOnChild(handler: proc(s: cint) {.noconv.}) =
  ## Has a child's end interrupt the server's wait, and nothing else: idle
  ## then reaps. SIGCHLD is blocked but in the wait, and the mask before is
  ## the wait's and, through spawn, a child's.
  var chld: Sigset
  discard sigemptyset(chld)
  discard sigaddset(chld, SIGCHLD)
  discard c_signal(SIGCHLD, handler)
  if sigprocmask(SIG_BLOCK, chld, waitMask) != 0:
    fatal("rootfs: sigprocmask")
  waitMasked = true

# The image is a ustar archive (bin/rootfs/image.py) linked in as data
# (dev/nim.py --image).
proc imageBase(): ptr UncheckedArray[byte] {.importc: "rootfs_image_base", header: "image.h".}
var imageSize {.importc: "rootfs_image_size", header: "image.h".}: cuint
let image = imageBase()

# bytesAt is a slice of the image as a string, its NULs stripped for a
# header field.
proc bytesAt(at, n: int, field = false): string =
  result = newString(n)
  if n > 0:
    copyMem(result[0].addr, image[at].addr, n)
  if field:
    result = result.strip(chars = {'\0'})


var
  server, dev, tmp: Server
  shellPid: Pid
  shellStatus = -1'i32

const tmpCapacity = 1'u64 shl 30

proc octal(s: string): int =
  for c in s:
    if c in '0' .. '7':
      result = result * 8 + ord(c) - ord('0')

proc dirOf(s: var Server, path: string): Node =
  ## The directory of a path, made on the way.
  result = s.root
  for part in path.split('/'):
    if part == "":
      continue
    if part notin result.children:
      discard s.node(result, part, nkDir)
    result = result.children[part].node

proc load(s: var Server) =
  ## The entries of the archive, a header block of 512 bytes each, the
  ## data of a file in the blocks after it.
  var at = 0
  let total = int(imageSize)
  while at + 512 <= total and image[at] != 0:
    let name = bytesAt(at, 100, field = true)
    let perm = uint32(octal(bytesAt(at + 100, 8))) and 0o777
    let size = octal(bytesAt(at + 124, 12))
    let kind = char(image[at + 156])
    let target = bytesAt(at + 157, 100, field = true)
    let path = name.strip(chars = {'/'}, leading = false)
    let slash = path.rfind('/')
    let dir = s.dirOf(if slash >= 0: path[0 ..< slash] else: "")
    let base = path[slash + 1 .. ^1]
    case kind
    of '5':
      if base != "" and base notin dir.children:
        discard s.node(dir, base, nkDir)
      if base != "":
        dir.children[base].node.perm = perm
    of '2':
      s.node(dir, base, nkLink).target = target
    else:
      let file = s.node(dir, base, nkFile)
      s.fill(file, bytesAt(at + 512, size))
      file.perm = perm
    at += 512 + (size + 511) div 512 * 512

proc devfs(): Server =
  ## The terminal is descriptor 0, from the kernel: the object itself goes
  ## to whoever opens /dev/tty. urandom is the kernel's entropy, through
  ## getentropy.
  result = Server(writable: false)
  result.root = result.node(nil, "", nkDir)
  result.node(result.root, "tty", nkObject).fd = 0
  discard result.node(result.root, "null", nkNull)
  discard result.node(result.root, "zero", nkZero)
  discard result.node(result.root, "urandom", nkRandom)
  discard result.node(result.root, "random", nkRandom)

proc spawn(path: string, argv, env: openArray[string], root: cint): Pid =
  ## A program run in a child whose root and working directory are root,
  ## an end of a pair of ours: the child asks us, who serve when it does.
  result = fork()
  if result == 0:
    var blocked: Sigset
    if waitMasked:
      discard sigprocmask(SIG_SETMASK, waitMask, blocked)
    if fchdir(root) != 0 or chroot(".") != 0:
      stderr.writeLine("rootfs: chroot: errno " & $errno)
      quit(127)
    discard close(root)
    discard execve(path.cstring, allocCStringArray(argv), allocCStringArray(env))
    stderr.writeLine("rootfs: " & path & ": errno " & $errno)
    quit(127)
  if result < 0:
    fatal("rootfs: fork")

proc reap(): bool =
  ## After a wait: what ended is reaped; the shell's end is the system's.
  while true:
    var status: cint
    let pid = waitpid(-1, status, WNOHANG)
    if pid <= 0:
      return shellStatus < 0
    if pid == shellPid:
      shellStatus = (if WIFEXITED(status): WEXITSTATUS(status) else: 128 + WTERMSIG(status))

proc main() =
  if CMSG_SPACE(csize_t(maxFds * sizeof(cint))) > csize_t(packetControl.len):
    fatal("rootfs: no room for the descriptors of a packet")
  server = Server(writable: false)
  server.root = server.node(nil, "", nkDir)
  server.load()
  dev = devfs()
  tmp = Server(writable: true, capacity: tmpCapacity)
  tmp.root = tmp.node(nil, "", nkDir)
  tmp.root.perm = 0o1777
  server.mountOn("dev", dev.addr)
  server.mountOn("tmp", tmp.addr)
  let (ours, theirs) = pair()
  # The shell's root is the far end of our own pair, never ours: a server
  # does not walk its own tree by path, and could not ask itself before
  # it serves.
  reapOnChild(proc(s: cint) {.noconv.} = discard)
  shellPid = spawn("/bin/sh", ["sh"], ["PATH=/bin", "HOME=/", "TERM=vt100"], theirs)
  discard close(theirs)
  server.listen(ours)
  when defined(fsDebug):
    stderr.writeLine("rootfs: serving, shell " & $shellPid)
  serve([server.addr, dev.addr, tmp.addr], reap)
  quit(max(shellStatus, 0))

main()
