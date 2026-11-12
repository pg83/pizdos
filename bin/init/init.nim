## init: pid 2, from the image. Its root is the rootfs server. It serves
## /dev itself: the terminal the kernel gave vm, null, zero and urandom;
## and /tmp, a tree in its memory; mounts both; starts the shell on the
## terminal with a clean environment; reaps what ends under it, and ends
## with the shell's status.

import std/posix
import fs

var
  dev, tmp: Server
  shellPid: Pid
  shellStatus = -1'i32

proc devfs(): Server =
  ## The terminal is descriptor 0, inherited from the first process: the
  ## object itself goes to whoever opens /dev/tty. urandom is the kernel's
  ## entropy, through getentropy.
  result = Server(writable: false)
  result.root = result.node(nil, "", nkDir)
  result.node(result.root, "tty", nkObject).fd = 0
  discard result.node(result.root, "null", nkNull)
  discard result.node(result.root, "zero", nkZero)
  discard result.node(result.root, "urandom", nkRandom)
  discard result.node(result.root, "random", nkRandom)

proc spawn(path: string, argv, env: openArray[string]): Pid =
  result = fork()
  if result == 0:
    discard execve(path.cstring, allocCStringArray(argv), allocCStringArray(env))
    stderr.writeLine("init: " & path & ": errno " & $errno)
    quit(127)
  if result < 0:
    fatal("init: fork")

proc reap(): bool =
  ## After a wait: what ended is reaped; the shell's end is ours.
  while true:
    var status: cint
    let pid = waitpid(-1, status, WNOHANG)
    if pid <= 0:
      return shellStatus < 0
    if pid == shellPid:
      shellStatus = (if WIFEXITED(status): WEXITSTATUS(status) else: 128 + WTERMSIG(status))

proc main() =
  dev = devfs()
  let (ours, theirs) = pair()
  dev.listen(ours)
  mountAt("/dev", theirs)
  tmp = Server(writable: true)
  tmp.root = tmp.node(nil, "", nkDir)
  tmp.root.perm = 0o1777
  let (tmpOurs, tmpTheirs) = pair()
  tmp.listen(tmpOurs)
  mountAt("/tmp", tmpTheirs)
  reapOnChild(proc(s: cint) {.noconv.} = discard)
  shellPid = spawn("/bin/sh", ["sh"], ["PATH=/bin:/usr/bin:/sbin:/usr/sbin", "HOME=/", "TERM=vt100"])
  serve([dev.addr, tmp.addr], reap)
  quit(max(shellStatus, 0))

main()
