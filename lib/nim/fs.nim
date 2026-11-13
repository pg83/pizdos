## A file server for the kernel: a tree of nodes behind endpoints.
##
## An open file is one end of a seqpacket pair; the server holds the other
## and answers on it. Every request brings a fresh endpoint, last among
## its descriptors: the reply goes on it, and an OPEN's stays the open
## file's, so a server that does not own a name sends the request on to
## the server mounted there, endpoint and all, and is out of the way. The
## kernel speaks this protocol for the POSIX calls of a process
## (bin/vm/kernel_remote.go); a program speaks it itself with sendmsg on a
## file it opened, as init does to mount.
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
##   on a directory's endpoint, the path relative to it, ".." staying in it:
##     OPEN      arg flags   mode u32 path OK stat; fds[0] an object: the file is it
##     STAT      arg flags   path          OK stat; AT_SYMLINK_NOFOLLOW
##     READLINK              path          OK target
##     MKDIR     arg mode    path
##     MKNOD     arg mode    path          a FIFO or a regular file
##     UNLINK    arg flags   path          AT_REMOVEDIR
##     RENAME                old 0 new
##     LINK      arg follow  old 0 new     follow: AT_SYMLINK_FOLLOW's
##     SYMLINK               target 0 path
##     MOUNT                 path          fds = [root of the server, reply]
##     CHMOD     arg mode    path
##     CHOWN     arg follow  uid u32 gid u32 path
##     UTIMES    arg when    atime u64 mtime u64 path   when: 1 follow,
##                                         2 atime now, 4 mtime now,
##                                         8 atime kept, 16 mtime kept
##     TRUNCATE              size u64 path
##     STATFS                path          OK statfs
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
##   replies: OK, ERR with arg the errno
##   stat { ino u64, size u64, mtime u64 nanoseconds, mode u32, nlink u32,
##          uid u32, gid u32, atime u64, dev u64, ctime u64 }
##   statfs { blocks u64, bfree u64, files u64, ffree u64, dev u64,
##            bsize u32, namemax u32, flags u32 }  flags: 1 read-only
##
## A directory's entries have positions, each after those before it: a
## READDIR gives the entries after one, so a reading that removes what it
## read misses nothing. A node has the names that are links to it, a
## directory one. dev is the server's: the number of the pair it first
## answers on, which no other pair in the kernel has.
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
  atSymlinkNofollow* = 0x100'u32
  atRemovedir* = 0x200'u32
  oCreat* = 0o100'u32
  oExcl* = 0o200'u32
  oTrunc* = 0o1000'u32
  oNofollow* = 0o400000'u32
  whenFollow = 1'u32
  whenAtimeNow = 2'u32
  whenMtimeNow = 4'u32
  whenAtimeKept = 8'u32
  whenMtimeKept = 16'u32
  oDirectory* = 0o200000'i32
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

type
  Op* = enum
    opOpen = 1, opStat, opReadlink, opMkdir, opUnlink, opRename, opSymlink,
    opMount, opRead, opWrite, opReaddir, opFstat, opChmod, opChown, opUtimes,
    opTruncate, opFchmod, opFchown, opFutimes, opFtruncate, opFsync,
    opMknod, opLink, opStatfs, opFstatfs, opFlock, opLock, opOk, opErr

  Header* {.packed.} = object
    op*, arg*: uint32
    uid*, gid*: uint32

  Stat* {.packed.} = object
    ino*, size*, mtime*: uint64
    mode*, nlink*, uid*, gid*: uint32
    atime*, dev*, ctime*: uint64

  Statfs* {.packed.} = object
    blocks*, bfree*, files*, ffree*, dev*: uint64
    bsize*, namemax*, flags*: uint32

  NodeKind* = enum
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

  Node* = ref object
    ino*: uint64
    perm*: uint32  ## the permission bits
    uid*, gid*: uint32
    atime*, mtime*, ctime*: uint64  ## nanoseconds
    links*: uint32  ## the names that are links to it
    opens: int  ## the endpoints open on it
    shared: seq[cint]  ## the endpoints that hold flock's shared lock
    exclusive: cint  ## the endpoint that holds flock's exclusive lock, or -1
    ranges: seq[Range]
    case kind*: NodeKind
    of nkDir:
      parent*: Node  ## where ".." goes; none for the root
      children*: OrderedTable[string, Entry]  ## in the order of their positions
      entries: uint64  ## the last position given out
      mount*: cint  ## the root endpoint of a server mounted here, or -1
    of nkFile: data*: string
    of nkLink: target*: string
    of nkObject: fd*: cint
    of nkFifo, nkNull, nkZero, nkRandom: discard

  Entry* = object
    node*: Node
    pos: uint64  ## where the name is among the directory's entries

  Endpoint = object
    fd: cint
    node: Node  ## what the endpoint is open on
    reads, writes: bool  ## what its open lets it do

  Server* = object
    root*: Node
    writable*: bool
    capacity*: uint64  ## the bytes a writable server's files may hold
    eps: Table[cint, Endpoint]  ## by descriptor
    inodes: uint64  ## the inode numbers given out
    nodes: uint64  ## the nodes there are
    used: uint64  ## the bytes the files hold
    dev: uint64  ## the device of the server's files

  Walk = object
    node: Node
    dir: Node  ## the directory node was found in
    rest: string  ## the path left for the server mounted at node
    forward: bool
    err: cint

# lib/c has these; Nim's posix does not declare them all.
proc chroot*(path: cstring): cint {.importc, header: "<unistd.h>".}
proc c_signal(sig: cint, handler: proc(s: cint) {.noconv.}): pointer {.importc: "signal", header: "<signal.h>".}
proc getentropy(buf: pointer, n: csize_t): cint {.importc, header: "<unistd.h>".}

# ---- packets --------------------------------------------------------------

proc fatal*(why: string) =
  stderr.writeLine(why)
  quit(1)

proc send*(ep: cint, h: Header, body: openArray[byte] = [],
           fds: openArray[cint] = []) =
  var head = h
  var iov = [IOVec(iov_base: head.addr, iov_len: csize_t(sizeof(head))),
             IOVec(iov_base: if body.len > 0: body[0].unsafeAddr else: nil, iov_len: csize_t(body.len))]
  var control = newSeq[byte](int(CMSG_SPACE(csize_t(maxFds * sizeof(cint)))))
  var m = Tmsghdr(msg_iov: iov[0].addr, msg_iovlen: 2)
  if fds.len > 0:
    m.msg_control = control[0].addr
    m.msg_controllen = CMSG_LEN(csize_t(fds.len * sizeof(cint)))
    let c = CMSG_FIRSTHDR(m.addr)
    c.cmsg_level = SOL_SOCKET
    c.cmsg_type = SCM_RIGHTS
    c.cmsg_len = m.msg_controllen
    copyMem(CMSG_DATA(c), fds[0].unsafeAddr, fds.len * sizeof(cint))
  discard sendmsg(SocketHandle(ep), m.addr, 0)  # a gone reader is its own loss

proc send*(ep: cint, op: Op, arg: uint32, body: openArray[byte] = [],
           fds: openArray[cint] = []) =
  send(ep, Header(op: uint32(op), arg: arg), body, fds)

proc recv*(ep: cint, h: var Header, body: var seq[byte], fds: var seq[cint]): bool =
  ## One packet, or false at the end of the endpoint.
  var buf = newSeq[byte](maxPacket)
  var iov = IOVec(iov_base: buf[0].addr, iov_len: csize_t(buf.len))
  var control = newSeq[byte](int(CMSG_SPACE(csize_t(maxFds * sizeof(cint)))))
  var m = Tmsghdr(msg_iov: iov.addr, msg_iovlen: 1, msg_control: control[0].addr,
                  msg_controllen: csize_t(control.len))
  let n = recvmsg(SocketHandle(ep), m.addr, msgCmsgCloexec)
  if n <= 0:
    return false
  if n < sizeof(Header):
    fatal("a packet without a header")
  copyMem(h.addr, buf[0].addr, sizeof(Header))
  body = buf[sizeof(Header) ..< n]
  fds.setLen(0)
  var c = CMSG_FIRSTHDR(m.addr)
  while c != nil:
    if c.cmsg_level == SOL_SOCKET and c.cmsg_type == SCM_RIGHTS:
      let count = (int(c.cmsg_len) - int(CMSG_LEN(0))) div sizeof(cint)
      for i in 0 ..< count:
        fds.add cast[ptr UncheckedArray[cint]](CMSG_DATA(c))[i]
    c = CMSG_NXTHDR(m.addr, c)
  true

proc u32*(body: openArray[byte], at: var int): uint32 =
  if at + 4 > body.len:
    fatal("a short packet")
  for i in 0 ..< 4:
    result = result or (uint32(body[at + i]) shl (8 * i))
  at += 4

proc u64*(body: openArray[byte], at: var int): uint64 =
  if at + 8 > body.len:
    fatal("a short packet")
  for i in 0 ..< 8:
    result = result or (uint64(body[at + i]) shl (8 * i))
  at += 8

proc str*(body: openArray[byte], at: var int): string =
  ## A string of the body up to its NUL, or its end; at goes past it.
  while at < body.len and body[at] != 0:
    result.add char(body[at])
    inc at
  inc at

proc bytes*[T](v: var T): seq[byte] =
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

proc node*(s: var Server, parent: Node, name: string, kind: NodeKind): Node =
  ## A new node, with the permissions of its kind until they are set.
  inc s.inodes
  inc s.nodes
  let t = now()
  result = Node(ino: s.inodes, kind: kind, atime: t, mtime: t, ctime: t, exclusive: -1)
  result.perm = case kind
    of nkDir: 0o755'u32
    of nkFile, nkFifo: 0o644'u32
    of nkLink: 0o777'u32
    of nkObject, nkNull, nkZero, nkRandom: 0o666'u32
  if kind == nkDir:
    result.mount = -1
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
  ## server's again.
  if n.links == 0 and n.opens == 0:
    if n.kind == nkFile:
      s.used -= uint64(n.data.len)
      n.data = ""
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

proc fill*(s: var Server, n: Node, data: string) =
  ## A file's data, counted as the server's: for a server that fills its
  ## tree itself, as rootfs does from its image.
  s.used -= uint64(n.data.len)
  n.data = data
  s.used += uint64(data.len)

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

proc fail(e: cint): Walk = Walk(err: e)

proc walk(top, start: Node, path: string, h: Header, follow = true, depth = 0): Walk =
  ## Resolves a path from start, under top, which ".." does not leave: the
  ## directory an endpoint is open on. A link's target starts at the
  ## directory the link is in, or at top when absolute. A directory with a
  ## server mounted on it ends the walk: the rest is that server's. Each
  ## directory gone through must be one who asks may search.
  if depth > 40:
    return fail(ELOOP)
  var node = start
  var dir = start
  let parts = path.split('/')
  for i, part in parts:
    if node.kind == nkDir and node.mount >= 0:
      return Walk(node: node, rest: parts[i .. ^1].join("/"), forward: true)
    if part == "":
      continue
    if node.kind == nkLink:
      let w = walk(top, (if node.target.startsWith("/"): top else: dir), node.target, h, true, depth + 1)
      if w.err != 0 or w.forward:
        return w
      node = w.node
    if node.kind != nkDir:
      return fail(ENOTDIR)
    if not node.may(h, 1):
      return fail(EACCES)
    if part == ".":
      continue
    if part == "..":
      if node != top and node.parent != nil:
        node = node.parent
      dir = node
    elif part in node.children:
      dir = node
      node = node.children[part].node
    else:
      return fail(ENOENT)
  if node.kind == nkLink and follow:
    return walk(top, (if node.target.startsWith("/"): top else: dir), node.target, h, true, depth + 1)
  if node.kind == nkDir and node.mount >= 0:
    return Walk(node: node, rest: "", forward: true)
  Walk(node: node, dir: dir)

proc parentOf(start: Node, path: string, h: Header): (Walk, string) =
  ## The directory a path's last name is in, and the name.
  var p = path
  while p.len > 1 and p.endsWith("/"):
    p.setLen(p.len - 1)
  let at = p.rfind('/')
  let (dir, name) = if at < 0: (".", p) else: (p[0 .. at], p[at + 1 .. ^1])
  if name in ["", ".", ".."]:
    return (fail(EINVAL), "")
  var w = walk(start, start, dir, h)
  if w.err == 0 and w.forward:
    w.rest = (if w.rest == "": name else: w.rest & "/" & name)
  (w, name)

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

# ---- calls on paths ---------------------------------------------------------

# pathAt is where the path of a call starts in its body: after what
# comes before it.
proc pathAt(op: Op, body: openArray[byte]): int =
  case op
  of opOpen: 4
  of opChown, opTruncate: 8
  of opUtimes: 16
  of opSymlink:
    var at = 0
    discard str(body, at)
    at
  else: 0

proc forwardAfter(w: Walk, h: Header, prefix: openArray[byte], fds: seq[cint]) =
  ## The same call, who asks, what came before its path, the rest of the
  ## path, the same endpoints: on to the server mounted where the walk
  ## ended.
  var packet = newSeq[byte]()
  packet.add prefix
  packet.add w.rest.toOpenArrayByte(0, w.rest.high)
  send(w.node.mount, h, packet, fds)
  for fd in fds:
    discard close(fd)

proc forward(w: Walk, h: Header, body: openArray[byte], fds: seq[cint]) =
  forwardAfter(w, h, body.toOpenArray(0, min(pathAt(Op(h.op), body), body.len) - 1), fds)

template resolved(reply: cint, walked: Walk, h: Header, body, fds): Node =
  ## The node a walk reached, or the reply that ends the request. The walk
  ## is one expression, taken once: its path reader moves.
  let w = walked
  if w.err != 0:
    err(reply, w.err)
    discard close(reply)
    return
  if w.forward:
    forward(w, h, body, fds)
    return
  w.node

template answer(reply: cint, e: cint) =
  ## The reply of a call that changes something: OK, or the error.
  if e == 0:
    ok(reply)
  else:
    err(reply, e)
  discard close(reply)

proc listen*(s: var Server, ep: cint, node: Node = nil, reads = true, writes = false)

proc open(s: var Server, c: Node, h: Header, body: openArray[byte], fds: seq[cint]) =
  ## A node opened: read or written as its access mode says and who asks
  ## may, made when O_CREAT asks in a directory they may write, emptied
  ## when O_TRUNC does.
  let reply = fds[^1]
  var at = 0
  let mode = u32(body, at)
  let path = str(body, at)
  let access = h.arg and 3
  let reads = access != 1
  let writes = access != 0
  let truncates = (h.arg and oTrunc) != 0
  var w = walk(c, c, path, h, (h.arg and oNofollow) == 0)
  var made = false
  if w.err == 0 and not w.forward and (h.arg and (oCreat or oExcl)) == (oCreat or oExcl):
    w = fail(EEXIST)
  if w.err == ENOENT and (h.arg and oCreat) != 0:
    let (p, name) = parentOf(c, path, h)
    if p.err != 0 or p.forward:
      w = p
    elif not s.writable:
      w = fail(EROFS)
    elif not p.node.may(h, 3):
      w = fail(EACCES)
    elif s.full:
      w = fail(ENOSPC)
    else:
      w = Walk(node: s.node(p.node, name, nkFile))
      w.node.perm = mode and 0o7777
      w.node.uid = h.uid
      w.node.gid = h.gid
      made = true
  let node = resolved(reply, w, h, body, fds)
  let want = (if reads: 4'u32 else: 0'u32) or (if writes or truncates: 2'u32 else: 0'u32)
  var e: cint = 0
  if not made and not node.may(h, want):
    e = EACCES
  elif node.kind == nkDir and writes:
    e = EISDIR
  elif (writes or truncates) and node.kind == nkFile and not s.writable:
    e = EROFS
  if e != 0:
    answer(reply, e)
    return
  case node.kind
  of nkObject:
    ok(reply, fds = [node.fd])  # the object itself becomes the caller's
    discard close(reply)
  else:
    if truncates and node.kind == nkFile and node.data.len > 0:
      discard s.resize(node, 0)
      node.mtime = now()
      node.changed
    var st = s.statOf(node)
    s.listen(reply, node, reads, writes)
    ok(reply, st.bytes)

proc stat(s: var Server, c: Node, h: Header, body: openArray[byte], fds: seq[cint]) =
  let reply = fds[^1]
  var at = 0
  let node = resolved(reply, walk(c, c, str(body, at), h, (h.arg and atSymlinkNofollow) == 0), h, body, fds)
  var st = s.statOf(node)
  ok(reply, st.bytes)
  discard close(reply)

proc statfs(s: var Server, c: Node, h: Header, body: openArray[byte], fds: seq[cint]) =
  ## The file system of what a path names: the server's, or the one
  ## mounted where the path goes.
  let reply = fds[^1]
  var at = 0
  discard resolved(reply, walk(c, c, str(body, at), h), h, body, fds)
  var st = s.statfsOf
  ok(reply, st.bytes)
  discard close(reply)

proc readlink(s: var Server, c: Node, h: Header, body: openArray[byte], fds: seq[cint]) =
  let reply = fds[^1]
  var at = 0
  let node = resolved(reply, walk(c, c, str(body, at), h, follow = false), h, body, fds)
  if node.kind != nkLink:
    err(reply, EINVAL)
  else:
    ok(reply, node.target.toOpenArrayByte(0, node.target.high))
  discard close(reply)

proc create(s: var Server, c: Node, h: Header, body: openArray[byte], fds: seq[cint], op: Op) =
  ## mkdir, mknod and symlink: a new name in a directory of ours, who
  ## asks may write, theirs, with the mode asked for. A name that is there
  ## is EEXIST before anything else, on a server that writes nothing too,
  ## and so is the directory a path ends in: mkdir -p asks for "/" first.
  ## mknod makes FIFOs and regular files; a device is EPERM.
  let reply = fds[^1]
  var at = 0
  let target = if op == opSymlink: str(body, at) else: ""
  let path = str(body, at)
  let there = walk(c, c, path, h, follow = false)
  if there.err == 0 and there.forward:
    forward(there, h, body, fds)
    return
  if there.err != ENOENT:
    answer(reply, if there.err == 0: EEXIST else: there.err)
    return
  let (w, name) = parentOf(c, path, h)
  let dir = resolved(reply, w, h, body, fds)
  var kind = nkDir
  var e: cint = 0
  if op == opSymlink:
    kind = nkLink
  elif op == opMknod:
    case h.arg and sIfmt
    of sIfifo: kind = nkFifo
    of 0, sIfreg: kind = nkFile
    else: e = EPERM
  if e != 0:
    discard
  elif not s.writable:
    e = EROFS
  elif not dir.may(h, 3):
    e = EACCES
  elif s.full:
    e = ENOSPC
  else:
    let n = s.node(dir, name, kind)
    n.uid = h.uid
    n.gid = h.gid
    if kind == nkLink:
      n.target = target
    else:
      n.perm = h.arg and 0o7777
  answer(reply, e)

proc unlink(s: var Server, c: Node, h: Header, body: openArray[byte], fds: seq[cint]) =
  let reply = fds[^1]
  var at = 0
  let (w, name) = parentOf(c, str(body, at), h)
  let dir = resolved(reply, w, h, body, fds)
  let removeDir = (h.arg and atRemovedir) != 0
  var e: cint = 0
  if not s.writable:
    e = EROFS
  elif name notin dir.children:
    e = ENOENT
  else:
    let n = dir.children[name].node
    if removeDir and n.kind != nkDir:
      e = ENOTDIR
    elif not removeDir and n.kind == nkDir:
      e = EISDIR
    elif not dir.may(h, 3):
      e = EACCES
    elif dir.keeps(n, h):
      e = EPERM
    elif removeDir and n.mount >= 0:
      e = EBUSY
    elif removeDir and n.children.len > 0:
      e = ENOTEMPTY
    else:
      s.release(dir.unplace(name))
  answer(reply, e)

proc within(n, dir: Node): bool =
  ## Whether dir is n or under it.
  var up = dir
  while up != nil:
    if up == n:
      return true
    up = up.parent
  false

proc rename(s: var Server, c: Node, h: Header, body: openArray[byte], fds: seq[cint]) =
  ## Both names are ours, or the move crosses a server: EXDEV. What the
  ## new name names goes, when it is of the old one's kind, and empty if a
  ## directory; a directory does not go under itself, nor a mount point
  ## anywhere, and one that goes to another directory must be one who
  ## asks may write, for its "..".
  let reply = fds[^1]
  var at = 0
  let (src, oldName) = parentOf(c, str(body, at), h)
  let (dst, newName) = parentOf(c, str(body, at), h)
  var e: cint = 0
  if src.err != 0 or dst.err != 0:
    e = if src.err != 0: src.err else: dst.err
  elif src.forward and src.node == dst.node:
    # Both names in one mounted server: its rename, of its paths.
    var old = newSeq[byte]()
    old.add src.rest.toOpenArrayByte(0, src.rest.high)
    old.add 0
    forwardAfter(dst, h, old, fds)
    return
  elif src.forward or dst.forward:
    e = EXDEV
  elif not s.writable:
    e = EROFS
  elif oldName notin src.node.children:
    e = ENOENT
  else:
    let n = src.node.children[oldName].node
    let old = if newName in dst.node.children: dst.node.children[newName].node else: nil
    if old == n:
      discard
    elif n.kind == nkDir and within(n, dst.node):
      e = EINVAL
    elif not src.node.may(h, 3) or not dst.node.may(h, 3):
      e = EACCES
    elif src.node.keeps(n, h) or old != nil and dst.node.keeps(old, h):
      e = EPERM
    elif n.kind == nkDir and src.node != dst.node and not n.may(h, 2):
      e = EACCES
    elif n.kind == nkDir and n.mount >= 0 or old != nil and old.kind == nkDir and old.mount >= 0:
      e = EBUSY
    elif old != nil and n.kind == nkDir and old.kind != nkDir:
      e = ENOTDIR
    elif old != nil and n.kind != nkDir and old.kind == nkDir:
      e = EISDIR
    elif old != nil and old.kind == nkDir and old.children.len > 0:
      e = ENOTEMPTY
    else:
      discard src.node.unplace(oldName)
      if old != nil:
        s.release(dst.node.unplace(newName))
      dst.node.place(newName, n)
  answer(reply, e)

proc link(s: var Server, c: Node, h: Header, body: openArray[byte], fds: seq[cint]) =
  ## A new name for a node that is no directory, in a directory of ours
  ## who asks may write: across servers EXDEV.
  let reply = fds[^1]
  var at = 0
  let src = walk(c, c, str(body, at), h, follow = (h.arg and 1) != 0)
  let (dst, newName) = parentOf(c, str(body, at), h)
  var e: cint = 0
  if src.err != 0 or dst.err != 0:
    e = if src.err != 0: src.err else: dst.err
  elif src.forward and src.node == dst.node:
    var old = newSeq[byte]()
    old.add src.rest.toOpenArrayByte(0, src.rest.high)
    old.add 0
    forwardAfter(dst, h, old, fds)
    return
  elif src.forward or dst.forward:
    e = EXDEV
  elif not s.writable:
    e = EROFS
  elif newName in dst.node.children:
    e = EEXIST
  elif src.node.kind == nkDir:
    e = EPERM
  elif not dst.node.may(h, 3):
    e = EACCES
  else:
    dst.node.place(newName, src.node)
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
  ## chmod, chown, utimes or truncate of a node, by path or by endpoint,
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

proc attr(s: var Server, c: Node, h: Header, body: openArray[byte], fds: seq[cint]) =
  ## chmod, chown, utimes and truncate of a path.
  let reply = fds[^1]
  let op = Op(h.op)
  let ahead = pathAt(op, body)
  var at = ahead
  let follow = op == opChmod or op == opTruncate or (h.arg and whenFollow) != 0
  let node = resolved(reply, walk(c, c, str(body, at), h, follow), h, body, fds)
  answer(reply, if not s.writable: EROFS else: s.change(node, op, h, body.toOpenArray(0, ahead - 1)))

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

proc mount(s: var Server, c: Node, h: Header, body: openArray[byte], fds: seq[cint]) =
  ## A server's root endpoint goes on a directory of ours: names under it
  ## are that server's from now on. Anyone who reaches the directory may
  ## mount on it: who may is a question for later.
  if fds.len != 2:
    fatal("mount without a root")
  let reply = fds[^1]
  var at = 0
  let node = resolved(reply, walk(c, c, str(body, at), h), h, body, fds)
  if node.kind != nkDir:
    err(reply, ENOTDIR)
    discard close(fds[0])
  else:
    node.mount = fds[0]
    ok(reply)
  discard close(reply)

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

proc listen*(s: var Server, ep: cint, node: Node = nil, reads = true, writes = false) =
  ## An endpoint the server answers on, open on node, or on the root, to
  ## read, to write or both. The first one's pair gives the server its
  ## device: its number, which is the pair's alone in the kernel.
  discard fcntl(ep, F_SETFD, FD_CLOEXEC)
  if s.dev == 0:
    var st: posix.Stat
    if fstat(ep, st) == 0:
      s.dev = uint64(st.st_ino)
  let n = if node == nil: s.root else: node
  inc n.opens
  s.eps[ep] = Endpoint(fd: ep, node: n, reads: reads, writes: writes)
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

proc handle(s: var Server, fd: cint) =
  let ep = s.eps[fd]
  var h: Header
  var body: seq[byte]
  var fds: seq[cint]
  if not recv(ep.fd, h, body, fds):
    unwatch(ep.fd)
    s.forget(ep)
    discard close(ep.fd)  # the other end closed: the file is closed
    s.eps.del(fd)
    return
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
  of opOpen: s.open(ep.node, h, body, fds)
  of opStat: s.stat(ep.node, h, body, fds)
  of opStatfs: s.statfs(ep.node, h, body, fds)
  of opReadlink: s.readlink(ep.node, h, body, fds)
  of opMkdir, opMknod, opSymlink: s.create(ep.node, h, body, fds, Op(h.op))
  of opUnlink: s.unlink(ep.node, h, body, fds)
  of opRename: s.rename(ep.node, h, body, fds)
  of opLink: s.link(ep.node, h, body, fds)
  of opMount: s.mount(ep.node, h, body, fds)
  of opChmod, opChown, opUtimes, opTruncate: s.attr(ep.node, h, body, fds)
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

proc serve*(servers: openArray[ptr Server], idle: proc(): bool = nil) =
  ## Serves the servers, one process's, until the last endpoint of them
  ## all closes: then nobody can reach them. idle runs after every wait,
  ## interrupted ones too, and ends the loop by returning false.
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
    let n = epoll_wait(watcher, events[0].addr, cint(events.len), -1)
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

proc serve*(s: var Server, idle: proc(): bool = nil) =
  serve([s.addr], idle)

# ---- for programs that make servers and mounts ------------------------------

proc pair*(): (cint, cint) =
  ## A seqpacket pair: one end for a server, the other for its clients.
  var ends: array[0..1, cint]
  if socketpair(AF_UNIX, SOCK_SEQPACKET or sockCloexec, 0, ends) != 0:
    fatal("socketpair")
  (ends[0], ends[1])

proc call*(dir: cint, op: Op, arg: uint32, path: string, fds: openArray[cint] = []): (Header, seq[byte], seq[cint]) =
  ## A call on a path through a directory's endpoint, with the reply
  ## endpoint it needs, asked by the program itself: what the kernel does
  ## for every POSIX call on a path.
  let (mine, theirs) = pair()
  var all = @fds
  all.add theirs
  send(dir, Header(op: uint32(op), arg: arg, uid: uint32(getuid()), gid: uint32(getgid())),
       path.toOpenArrayByte(0, path.high), all)
  discard close(theirs)
  var h: Header
  var body: seq[byte]
  var got: seq[cint]
  if not recv(mine, h, body, got):
    fatal("no reply")
  discard close(mine)
  (h, body, got)

proc mountAt*(path: string, root: cint) =
  ## Puts a server's root endpoint at a path of our own root: the path is
  ## relative to it.
  let dir = posix.open("/", O_RDONLY or oDirectory)
  if dir < 0:
    fatal("open of the root")
  let (h, _, _) = call(dir, opMount, 0, path.strip(chars = {'/'}, trailing = false), [root])
  if Op(h.op) != opOk:
    fatal("mount at " & path & ": errno " & $h.arg)
  discard close(dir)
  discard close(root)

proc reapOnChild*(handler: proc(s: cint) {.noconv.}) =
  ## Has a child's end interrupt the server's wait: idle then reaps.
  discard c_signal(SIGCHLD, handler)
