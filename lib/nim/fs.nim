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
##   Header { op u32, arg u32 }
##
##   on a directory's endpoint, the path relative to it, ".." staying in it:
##     OPEN      arg flags   path          OK stat; fds[0] an object: the file is it
##     STAT      arg flags   path          OK stat; AT_SYMLINK_NOFOLLOW
##     READLINK              path          OK target
##     MKDIR     arg mode    path
##     UNLINK    arg flags   path          AT_REMOVEDIR
##     RENAME                old 0 new
##     SYMLINK               target 0 path
##     MOUNT                 path          fds = [root of the server, reply]
##   on a node's endpoint:
##     READ      arg len     off u64       OK data
##     WRITE                 off u64 data  OK count u32
##     READDIR   arg index                 OK (ino u64, type u8, name, 0)*
##     FSTAT                               OK stat
##   replies: OK, ERR with arg the errno
##   stat { ino u64, size u64, mtime u64 nanoseconds, mode u32, nlink u32 }

import std/[tables, strutils, posix]

const
  atSymlinkNofollow* = 0x100'u32
  atRemovedir* = 0x200'u32
  oCreat* = 0o100'u32
  oNofollow* = 0o400000'u32
  oDirectory* = 0o200000'i32
  msgCmsgCloexec = 0x40000000'i32
  sockCloexec = 0o2000000'i32
  maxFds = 4
  maxPacket = 65536
  dtChr = 2'u8
  dtDir = 4'u8
  dtReg = 8'u8
  dtLnk = 10'u8

type
  Op* = enum
    opOpen = 1, opStat, opReadlink, opMkdir, opUnlink, opRename, opSymlink,
    opMount, opRead, opWrite, opReaddir, opFstat, opOk, opErr

  Header* {.packed.} = object
    op*, arg*: uint32

  Stat* {.packed.} = object
    ino*, size*, mtime*: uint64
    mode*, nlink*: uint32

  NodeKind* = enum
    nkDir, nkFile, nkLink
    nkObject  ## an object of the kernel the server hands out: a tty, a disk
    nkNull    ## reads nothing, takes everything
    nkZero    ## reads zeros

  Node* = ref object
    name*: string
    parent*: Node
    ino*: uint64
    perm*: uint32  ## the permission bits of a file, 0 for the kind's own
    case kind*: NodeKind
    of nkDir:
      children*: OrderedTable[string, Node]
      mount*: cint  ## the root endpoint of a server mounted here, or -1
    of nkFile: data*: string
    of nkLink: target*: string
    of nkObject: fd*: cint
    of nkNull, nkZero: discard

  Endpoint = object
    fd: cint
    node: Node  ## what the endpoint is open on

  Server* = object
    root*: Node
    writable*: bool
    eps: seq[Endpoint]
    inodes: uint64

  Walk = object
    node: Node
    rest: string  ## the path left for the server mounted at node
    forward: bool
    err: cint

# lib/c has these; Nim's posix does not declare them all.
proc chroot*(path: cstring): cint {.importc, header: "<unistd.h>".}
proc c_signal(sig: cint, handler: proc(s: cint) {.noconv.}): pointer {.importc: "signal", header: "<signal.h>".}

# ---- packets --------------------------------------------------------------

proc fatal*(why: string) =
  stderr.writeLine(why)
  quit(1)

proc send*(ep: cint, op: Op, arg: uint32, body: openArray[byte] = [],
           fds: openArray[cint] = []) =
  var h = Header(op: uint32(op), arg: arg)
  var iov = [IOVec(iov_base: h.addr, iov_len: csize_t(sizeof(h))),
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

proc sendStr*(ep: cint, op: Op, arg: uint32, s: string, fds: openArray[cint] = []) =
  send(ep, op, arg, s.toOpenArrayByte(0, s.high), fds)

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

proc node*(s: var Server, parent: Node, name: string, kind: NodeKind): Node =
  inc s.inodes
  result = Node(name: name, parent: parent, ino: s.inodes, kind: kind)
  if kind == nkDir:
    result.mount = -1
  if parent != nil:
    parent.children[name] = result

proc fail(e: cint): Walk = Walk(err: e)

proc walk(top, start: Node, path: string, follow = true, depth = 0): Walk =
  ## Resolves a path from start, under top, which ".." does not leave: the
  ## directory an endpoint is open on. A link's target starts at the
  ## link's directory, or at top when absolute. A directory with a server
  ## mounted on it ends the walk: the rest is that server's.
  if depth > 40:
    return fail(ELOOP)
  var node = start
  let parts = path.split('/')
  for i, part in parts:
    if node.kind == nkDir and node.mount >= 0:
      return Walk(node: node, rest: parts[i .. ^1].join("/"), forward: true)
    if part == "" or part == ".":
      continue
    if node.kind == nkLink:
      let w = walk(top, (if node.target.startsWith("/"): top else: node.parent), node.target, true, depth + 1)
      if w.err != 0 or w.forward:
        return w
      node = w.node
    if node.kind != nkDir:
      return fail(ENOTDIR)
    if part == "..":
      if node != top and node.parent != nil:
        node = node.parent
    elif part in node.children:
      node = node.children[part]
    else:
      return fail(ENOENT)
  if node.kind == nkLink and follow:
    return walk(top, (if node.target.startsWith("/"): top else: node.parent), node.target, true, depth + 1)
  if node.kind == nkDir and node.mount >= 0:
    return Walk(node: node, rest: "", forward: true)
  Walk(node: node)

proc parentOf(start: Node, path: string): (Walk, string) =
  ## The directory a path's last name is in, and the name.
  var p = path
  while p.len > 1 and p.endsWith("/"):
    p.setLen(p.len - 1)
  let at = p.rfind('/')
  let (dir, name) = if at < 0: (".", p) else: (p[0 .. at], p[at + 1 .. ^1])
  if name in ["", ".", ".."]:
    return (fail(EINVAL), "")
  var w = walk(start, start, dir)
  if w.err == 0 and w.forward:
    w.rest = (if w.rest == "": name else: w.rest & "/" & name)
  (w, name)

proc mode(n: Node): uint32 =
  case n.kind
  of nkDir: 0o040755
  of nkFile: 0o100000'u32 or (if n.perm != 0: n.perm else: 0o644'u32)
  of nkLink: 0o120777
  of nkObject, nkNull, nkZero: 0o020666

proc dtype(n: Node): uint8 =
  case n.kind
  of nkDir: dtDir
  of nkFile: dtReg
  of nkLink: dtLnk
  of nkObject, nkNull, nkZero: dtChr

proc statOf(n: Node): Stat =
  result = Stat(ino: n.ino, mode: n.mode, nlink: 1)
  case n.kind
  of nkFile: result.size = uint64(n.data.len)
  of nkLink: result.size = uint64(n.target.len)
  else: discard

# ---- calls on paths ---------------------------------------------------------

proc forward(w: Walk, h: Header, body: openArray[byte], fds: seq[cint]) =
  ## The same call, the rest of the path, the same endpoints: on to the
  ## server mounted where the walk ended.
  var packet = newSeq[byte]()
  if h.op == uint32(opRename) or h.op == uint32(opSymlink):
    var at = 0
    let first = str(body, at)
    packet.add first.toOpenArrayByte(0, first.high)
    packet.add 0
  packet.add w.rest.toOpenArrayByte(0, w.rest.high)
  send(w.node.mount, Op(h.op), h.arg, packet, fds)
  for fd in fds:
    discard close(fd)

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

proc open(s: var Server, c: Node, h: Header, body: openArray[byte], fds: seq[cint]) =
  let reply = fds[^1]
  var at = 0
  let path = str(body, at)
  var w = walk(c, c, path, (h.arg and oNofollow) == 0)
  if w.err == ENOENT and (h.arg and oCreat) != 0:
    let (p, name) = parentOf(c, path)
    if p.err == 0 and not p.forward and s.writable:
      w = Walk(node: s.node(p.node, name, nkFile))
    elif p.err == 0 and not p.forward:
      w = fail(EROFS)
  let node = resolved(reply, w, h, body, fds)
  case node.kind
  of nkObject:
    ok(reply, fds = [node.fd])  # the object itself becomes the caller's
    discard close(reply)
  else:
    var st = node.statOf
    s.eps.add Endpoint(fd: reply, node: node)
    ok(reply, st.bytes)

proc stat(s: var Server, c: Node, h: Header, body: openArray[byte], fds: seq[cint]) =
  let reply = fds[^1]
  var at = 0
  let node = resolved(reply, walk(c, c, str(body, at), (h.arg and atSymlinkNofollow) == 0), h, body, fds)
  var st = node.statOf
  ok(reply, st.bytes)
  discard close(reply)

proc readlink(s: var Server, c: Node, h: Header, body: openArray[byte], fds: seq[cint]) =
  let reply = fds[^1]
  var at = 0
  let node = resolved(reply, walk(c, c, str(body, at), follow = false), h, body, fds)
  if node.kind != nkLink:
    err(reply, EINVAL)
  else:
    ok(reply, node.target.toOpenArrayByte(0, node.target.high))
  discard close(reply)

proc create(s: var Server, c: Node, h: Header, body: openArray[byte], fds: seq[cint], kind: NodeKind, target = "") =
  ## mkdir and symlink: a new name in a directory of ours.
  let reply = fds[^1]
  var at = 0
  let (w, name) = parentOf(c, str(body, at))
  let dir = resolved(reply, w, h, body, fds)
  if not s.writable:
    err(reply, EROFS)
  elif name in dir.children:
    err(reply, EEXIST)
  else:
    let n = s.node(dir, name, kind)
    if kind == nkLink:
      n.target = target
    ok(reply)
  discard close(reply)

proc unlink(s: var Server, c: Node, h: Header, body: openArray[byte], fds: seq[cint]) =
  let reply = fds[^1]
  var at = 0
  let (w, name) = parentOf(c, str(body, at))
  let dir = resolved(reply, w, h, body, fds)
  let removeDir = (h.arg and atRemovedir) != 0
  if not s.writable:
    err(reply, EROFS)
  elif name notin dir.children:
    err(reply, ENOENT)
  elif removeDir and dir.children[name].kind != nkDir:
    err(reply, ENOTDIR)
  elif not removeDir and dir.children[name].kind == nkDir:
    err(reply, EISDIR)
  elif removeDir and dir.children[name].children.len > 0:
    err(reply, ENOTEMPTY)
  else:
    dir.children.del(name)
    ok(reply)
  discard close(reply)

proc rename(s: var Server, c: Node, h: Header, body: openArray[byte], fds: seq[cint]) =
  ## Both names are ours, or the move crosses a server: EXDEV.
  let reply = fds[^1]
  var at = 0
  let (src, oldName) = parentOf(c, str(body, at))
  let (dst, newName) = parentOf(c, str(body, at))
  if src.err != 0 or dst.err != 0:
    err(reply, if src.err != 0: src.err else: dst.err)
  elif src.forward and src.node == dst.node:
    forward(src, h, body, fds)  # both names in one mounted server: its rename
    return
  elif src.forward or dst.forward:
    err(reply, EXDEV)
  elif not s.writable:
    err(reply, EROFS)
  elif oldName notin src.node.children:
    err(reply, ENOENT)
  else:
    let n = src.node.children[oldName]
    src.node.children.del(oldName)
    n.name = newName
    n.parent = dst.node
    dst.node.children[newName] = n
    ok(reply)
  discard close(reply)

proc mount(s: var Server, c: Node, h: Header, body: openArray[byte], fds: seq[cint]) =
  ## A server's root endpoint goes on a directory of ours: names under it
  ## are that server's from now on. Anyone who reaches the directory may
  ## mount on it: who may is a question for later.
  if fds.len != 2:
    fatal("mount without a root")
  let reply = fds[^1]
  var at = 0
  let node = resolved(reply, walk(c, c, str(body, at)), h, body, fds)
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

proc read(s: var Server, reply: cint, node: Node, h: Header, body: openArray[byte]) =
  var off: int
  if not offset(reply, body, off):
    return
  let n = int(h.arg)
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
    ok(reply, newSeq[byte](min(n, maxPacket - 16)))
  else:
    err(reply, EISDIR)

proc write(s: var Server, reply: cint, node: Node, h: Header, body: openArray[byte]) =
  var off: int
  if not offset(reply, body, off):
    return
  let data = body[8 .. ^1]
  var count = uint32(data.len)
  case node.kind
  of nkNull, nkZero:
    ok(reply, count.bytes)
  of nkFile:
    if not s.writable:
      err(reply, EROFS)
      return
    if node.data.len < off + data.len:
      node.data.setLen(off + data.len)
    for i, b in data:
      node.data[off + i] = char(b)
    ok(reply, count.bytes)
  else:
    err(reply, EISDIR)

proc readdir(s: var Server, reply: cint, node: Node, h: Header) =
  if node.kind != nkDir:
    err(reply, ENOTDIR)
    return
  var entries: seq[byte]
  var i = 0
  for name, child in node.children:
    if i >= int(h.arg) and entries.len + 9 + name.len + 1 <= maxPacket - 16:
      var ino = child.ino
      entries.add ino.bytes
      entries.add child.dtype
      entries.add name.toOpenArrayByte(0, name.high)
      entries.add 0
    inc i
  ok(reply, entries)

# ---- the loop ---------------------------------------------------------------

proc listen*(s: var Server, ep: cint, node: Node = nil) =
  ## An endpoint the server answers on, open on node, or on the root.
  discard fcntl(ep, F_SETFD, FD_CLOEXEC)
  s.eps.add Endpoint(fd: ep, node: if node == nil: s.root else: node)

proc handle(s: var Server, i: int) =
  let ep = s.eps[i]
  var h: Header
  var body: seq[byte]
  var fds: seq[cint]
  if not recv(ep.fd, h, body, fds):
    discard close(ep.fd)  # the other end closed: the file is closed
    s.eps.del(i)
    return
  when defined(fsDebug):
    stderr.writeLine("fs: op " & $h.op & " arg " & $h.arg & " body " & $body.len & " fds " & $fds.len)
  if fds.len == 0:
    for fd in fds:
      discard close(fd)
    return  # a request without its reply endpoint: nobody to answer
  case Op(h.op)
  of opOpen: s.open(ep.node, h, body, fds)
  of opStat: s.stat(ep.node, h, body, fds)
  of opReadlink: s.readlink(ep.node, h, body, fds)
  of opMkdir: s.create(ep.node, h, body, fds, nkDir)
  of opSymlink:
    var at = 0
    let target = str(body, at)
    s.create(ep.node, h, body[at .. ^1], fds, nkLink, target)
  of opUnlink: s.unlink(ep.node, h, body, fds)
  of opRename: s.rename(ep.node, h, body, fds)
  of opMount: s.mount(ep.node, h, body, fds)
  of opRead:
    s.read(fds[^1], ep.node, h, body)
    discard close(fds[^1])
  of opWrite:
    s.write(fds[^1], ep.node, h, body)
    discard close(fds[^1])
  of opReaddir:
    s.readdir(fds[^1], ep.node, h)
    discard close(fds[^1])
  of opFstat:
    var st = ep.node.statOf
    ok(fds[^1], st.bytes)
    discard close(fds[^1])
  of opOk, opErr:
    for fd in fds:
      discard close(fd)

proc serve*(s: var Server, idle: proc(): bool = nil) =
  ## Until the last endpoint closes: then nobody can reach the server.
  ## idle runs after every wait, interrupted ones too, and ends the loop
  ## by returning false.
  while s.eps.len > 0:
    var fds = newSeq[TPollfd](s.eps.len)
    for i, ep in s.eps:
      fds[i] = TPollfd(fd: ep.fd, events: POLLIN)
    let n = poll(fds[0].addr, Tnfds(fds.len), -1)
    when defined(fsDebug):
      stderr.writeLine("fs: poll " & $n & " errno " & $errno & " eps " & $s.eps.len)
    if idle != nil and not idle():
      return
    if n <= 0:
      continue
    for i in countdown(fds.high, 0):
      if (fds[i].revents and (POLLIN or POLLHUP)) != 0:
        s.handle(i)

# ---- for programs that make servers and mounts ------------------------------

proc pair*(): (cint, cint) =
  ## A seqpacket pair: one end for a server, the other for its clients.
  var ends: array[0..1, cint]
  if socketpair(AF_UNIX, SOCK_SEQPACKET or sockCloexec, 0, ends) != 0:
    fatal("socketpair")
  (ends[0], ends[1])

proc call*(dir: cint, op: Op, arg: uint32, path: string, fds: openArray[cint] = []): (Header, seq[byte], seq[cint]) =
  ## A call on a path through a directory's endpoint, with the reply
  ## endpoint it needs: what the kernel does for every POSIX call on a path.
  let (mine, theirs) = pair()
  var all = @fds
  all.add theirs
  sendStr(dir, op, arg, path, all)
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
