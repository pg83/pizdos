#!/usr/bin/env python3
"""lib/c's functions under `wasm2o run`: tst/libc/check.c prints one line
of results per area, which must be these. Its root has /etc/passwd,
/etc/group, /etc/hosts and /etc/services, so no name goes to the host's
resolver, a /tmp to make files and sockets in, and the program as
/bin/check, the path dladdr names.

    test_libc.py <wasm2o> <check.o>
"""
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

PASSWD = "root:x:0:0:root:/root:/bin/sh\nuser:x:1000:100:User:/home/user:/bin/sh\n"
GROUP = "root:x:0:\nwheel:x:10:root,user\nusers:x:100:\nstaff:x:50:user\n"
HOSTS = "127.0.0.1 localhost\n10.0.0.7 box box.example\n::1 localhost\n"
SERVICES = "ssh 22/tcp\nhttp 80/tcp www\ndomain 53/tcp\ndomain 53/udp\n"

EXPECTED = """\
strtok 3 a b c
strlcpy 11 overflo 10
memrchr 4 memmem 4 rawmemchr 2
basename lib file.txt
strerror_r 0 No such file or directory 34 No 
div -3 -1 142857142857 1 ffs 5 41
memccpy 4 swab 2143 strcoll 1
pointers aabcef 6 -1 1 3 3 4
wcslen 12 wcschr 7 wcsrchr 9 wcsstr 6
wcsspn 1 wcscspn 5 wcspbrk 6 wcscmp 1 0
wcstok 3 wcwidth 1 2 0 -1 wcswidth 4
wcstol -123 x wcstod 2500
wcsftime 2020-02-29 13 Sun é
wmem 2 1 zzz
ctype 10111111 Az
wctype 110111 Éж 0
towctrans A b
setlocale C C.UTF-8 C.UTF-8;C;C;C;C;C refused C
localeconv [.] [] 127
langinfo UTF-8 Monday Dec %a %b %e %T %Y ^[yY]
gmtime 2023-11-14 22:13 UTC yday 317 asctime Tue Nov 14 22:13:20 2023
timegm 2768400 2/2 1
difftime 6 clock_getres 0 cpu 1 nanosleep 0
tzset EST EDT 18000 1
setenv 0 1 0 1 0 3 0 unset -1 1
getsubopt 0:- 1:10 2:-
random 262836907 2022765545 1318479596 1 rand_r 1993684161 1388323688
memalign 0 1 1 1 usable 1 reallocarray 1 1 mmap 0 0 0 0
openat 1 pwrite 6 pread cde copy_file_range 4 5 bcde fstatat 0 6 faccessat 0 renameat 0 linkat 0 symlinkat 0 readlinkat 1 g truncate 0 2 unlinkat 0 0
mkfifo 0 1 statvfs 0 1 flock 0 lockf 0 0
dir 4 2 1 scandir 4 . .. g p glob 0 /tmp/d/g /tmp/d/p 1
mkstemps 1 suffix tmpfile temporary tmpnam 1 tempnam 1
getpwnam user 1000 /home/user getpwuid_r 0 root 34 getpwent 2 getgrnam wheel 10 root user getgrouplist 3 100 10 50 fgetpwent root fgetgrent wheel root
mntent / ext4 rw,noatime 1 noatime
sigset 1 0 -1 pending 1 sigwait 0 10 sigtimedwait -1 1 alarm 1 setitimer 1 sigaltstack 0 1
waitid 0 1 5 wait4 1 5
inet 1 0200010a 0100007f 10.1.0.2 1 2001:db8::ff00:42:8329 0 1
getaddrinfo 0 2/6 10.0.0.7:80 box -2 Name does not resolve
getnameinfo 0 box ssh servent domain 53 udp protoent udp 17 hostent box 10.0.0.7 1
socketpair ping bind 0 listen 0 1 connect 0 accept 1 getsockname /tmp/socket sendmsg 1 recvmsg 1 x pass timeout -1 1
if_nameindex 1 crypt $6$salt$egUxKNxDs8kPfh8iPMNcosMhb2eWah6d3R44JDm5Rj/j/XWR5E33QPd0YmHXoDHOIDR6kL5D3JcQcz0O8FHE00 xy/gRonXQz8UE
dlopen 1 1 1 dlsym 1 dladdr 1 /bin/check
fenv 0 0 0 0 0 gethostname 1 confstr 14 sysconf 1 pathconf 255
"""


def main():
    wasm2o, check = sys.argv[1:3]

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "etc").mkdir()
        (root / "tmp").mkdir()
        (root / "etc" / "passwd").write_text(PASSWD)
        (root / "etc" / "group").write_text(GROUP)
        (root / "etc" / "hosts").write_text(HOSTS)
        (root / "etc" / "services").write_text(SERVICES)
        (root / "bin").mkdir()
        shutil.copy(check, root / "bin" / "check")
        r = subprocess.run([wasm2o, "run", "--root", str(root), "-a", "check", str(root / "bin" / "check")],
                           capture_output=True, text=True, env={"PATH": "/bin"}, timeout=120)

    if r.returncode != 0 or r.stdout != EXPECTED:
        got, want = r.stdout.splitlines(), EXPECTED.splitlines()

        for i, (g, w) in enumerate(zip(got, want)):
            if g != w:
                print(f"line {i + 1}:\n  got  {g}\n  want {w}")

        print(f"FAIL check: exit {r.returncode}, {len(got)} lines of {len(want)}\n{r.stderr[-2000:]}")
        sys.exit(1)

    print(f"libc: {len(EXPECTED.splitlines())} lines of results as expected")


if __name__ == "__main__":
    main()
