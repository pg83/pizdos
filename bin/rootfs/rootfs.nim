## rootfs: the first process, a read-only file server of the image linked
## into it. The kernel starts it with the objects the host gave vm as
## descriptors, the terminal at 0..2, and no root at all. It makes itself
## the root of everything else: one seqpacket pair, chroot to one end,
## fork, and the child execs /bin/init out of the image, a module the
## kernel reads with OPEN and READ to the parent, already in its loop, and
## compiles. It reaps
## what ends under it, and ends with init: with its status, as a system
## does.

import std/[strutils, tables, posix]
import fs

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
  server: Server
  initPid: Pid
  initStatus = -1'i32

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

proc reap(): bool =
  ## After a wait: what ended is reaped; init's end is the system's.
  while true:
    var status: cint
    let pid = waitpid(-1, status, WNOHANG)
    if pid <= 0:
      return initStatus < 0
    if pid == initPid:
      initStatus = (if WIFEXITED(status): WEXITSTATUS(status) else: 128 + WTERMSIG(status))

proc main() =
  server = Server(writable: false)
  server.root = server.node(nil, "", nkDir)
  server.load()
  let (ours, theirs) = pair()
  # Our own root is the far end of our own pair: for the children, never
  # for us. A server does not walk its own tree by path.
  if fchdir(theirs) != 0 or chroot(".") != 0:
    fatal("rootfs: chroot")
  discard close(theirs)
  reapOnChild(proc(s: cint) {.noconv.} = discard)
  initPid = fork()
  if initPid == 0:
    discard execv("/bin/init", allocCStringArray(["init"]))
    fatal("rootfs: /bin/init: errno " & $errno)
  if initPid < 0:
    fatal("rootfs: fork")
  server.listen(ours)
  when defined(fsDebug):
    stderr.writeLine("rootfs: serving, init " & $initPid)
  server.serve(reap)
  quit(max(initStatus, 0))

main()
