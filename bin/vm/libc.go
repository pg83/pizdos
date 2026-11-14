//go:build linux && amd64

package main

// The kernel's functions by their names in lib/c: each takes the process
// from ctx and calls the method that does the work.

func (k *Kernel) functionTable() map[string]any {
	return map[string]any{
		"__kernel_start":         libc__kernel_start,
		"__kernel_program":       libc__kernel_program,
		"read":                   libcRead,
		"write":                  libcWrite,
		"readv":                  libcReadv,
		"writev":                 libcWritev,
		"preadv":                 libcPreadv,
		"pwritev":                libcPwritev,
		"pread":                  libcPread,
		"pwrite":                 libcPwrite,
		"copy_file_range":        libcCopy_file_range,
		"fdatasync":              libcFdatasync,
		"sync":                   libcSync,
		"syncfs":                 libcSyncfs,
		"flock":                  libcFlock,
		"lockf":                  libcLockf,
		"posix_fadvise":          libcPosix_fadvise,
		"posix_fallocate":        libcPosix_fallocate,
		"select":                 libcSelect,
		"pselect":                libcPselect,
		"lseek":                  libcLseek,
		"close":                  libcClose,
		"dup":                    libcDup,
		"dup2":                   libcDup2,
		"dup3":                   libcDup3,
		"pipe":                   libcPipe,
		"pipe2":                  libcPipe2,
		"fcntl":                  libcFcntl,
		"ioctl":                  libcIoctl,
		"isatty":                 libcIsatty,
		"tcgetattr":              libcTcgetattr,
		"tcsetattr":              libcTcsetattr,
		"cfmakeraw":              libcCfmakeraw,
		"tcdrain":                libcTcdrain,
		"tcsendbreak":            libcTcsendbreak,
		"tcflow":                 libcTcflow,
		"tcflush":                libcTcflush,
		"tcgetsid":               libcTcgetsid,
		"cfgetospeed":            libcCfgetospeed,
		"cfgetispeed":            libcCfgetispeed,
		"cfsetospeed":            libcCfsetospeed,
		"cfsetspeed":             libcCfsetspeed,
		"cfsetispeed":            libcCfsetispeed,
		"poll":                   libcPoll,
		"ppoll":                  libcPpoll,
		"epoll_create":           libcEpoll_create,
		"epoll_create1":          libcEpoll_create1,
		"epoll_ctl":              libcEpoll_ctl,
		"epoll_wait":             libcEpoll_wait,
		"epoll_pwait":            libcEpoll_pwait,
		"fsync":                  libcFsync,
		"ftruncate":              libcFtruncate,
		"fstat":                  libcFstat,
		"fchmod":                 libcFchmod,
		"fchown":                 libcFchown,
		"futimes":                libcFutimes,
		"futimens":               libcFutimens,
		"open":                   libcOpen,
		"openat":                 libcOpenat,
		"stat":                   libcStat,
		"lstat":                  libcLstat,
		"fstatat":                libcFstatat,
		"access":                 libcAccess,
		"faccessat":              libcFaccessat,
		"truncate":               libcTruncate,
		"statvfs":                libcStatvfs,
		"fstatvfs":               libcFstatvfs,
		"fchdir":                 libcFchdir,
		"chroot":                 libcChroot,
		"chdir":                  libcChdir,
		"getcwd":                 libcGetcwd,
		"mkdir":                  libcMkdir,
		"mkdirat":                libcMkdirat,
		"rmdir":                  libcRmdir,
		"unlink":                 libcUnlink,
		"unlinkat":               libcUnlinkat,
		"remove":                 libcRemove,
		"rename":                 libcRename,
		"renameat":               libcRenameat,
		"link":                   libcLink,
		"linkat":                 libcLinkat,
		"symlink":                libcSymlink,
		"symlinkat":              libcSymlinkat,
		"readlink":               libcReadlink,
		"readlinkat":             libcReadlinkat,
		"canonicalize_file_name": libcCanonicalize_file_name,
		"get_current_dir_name":   libcGet_current_dir_name,
		"creat":                  libcCreat,
		"eaccess":                libcEaccess,
		"euidaccess":             libcEuidaccess,
		"statfs":                 libcStatfs,
		"fstatfs":                libcFstatfs,
		"acct":                   libcAcct,
		"realpath":               libcRealpath,
		"chmod":                  libcChmod,
		"fchmodat":               libcFchmodat,
		"chown":                  libcChown,
		"lchown":                 libcLchown,
		"fchownat":               libcFchownat,
		"mknod":                  libcMknod,
		"mknodat":                libcMknodat,
		"mkfifo":                 libcMkfifo,
		"mkfifoat":               libcMkfifoat,
		"utimes":                 libcUtimes,
		"lutimes":                libcLutimes,
		"utime":                  libcUtime,
		"utimensat":              libcUtimensat,
		"opendir":                libcOpendir,
		"fdopendir":              libcFdopendir,
		"readdir":                libcReaddir,
		"rewinddir":              libcRewinddir,
		"closedir":               libcClosedir,
		"dirfd":                  libcDirfd,
		"readdir_r":              libcReaddir_r,
		"telldir":                libcTelldir,
		"seekdir":                libcSeekdir,
		"alphasort":              libcAlphasort,
		"versionsort":            libcVersionsort,
		"mkstemp":                libcMkstemp,
		"mkostemp":               libcMkostemp,
		"mkstemps":               libcMkstemps,
		"mkdtemp":                libcMkdtemp,
		"mktemp":                 libcMktemp,
		"malloc":                 libcMalloc,
		"free":                   libcFree,
		"calloc":                 libcCalloc,
		"realloc":                libcRealloc,
		"mallopt":                libcMallopt,
		"getpagesize":            libcGetpagesize,
		"mmap":                   libcMmap,
		"munmap":                 libcMunmap,
		"msync":                  libcMsync,
		"madvise":                libcMadvise,
		"mprotect":               libcMprotect,
		"mlock":                  libcMlock,
		"munlock":                libcMunlock,
		"mlockall":               libcMlockall,
		"munlockall":             libcMunlockall,
		"memalign":               libcMemalign,
		"aligned_alloc":          libcAligned_alloc,
		"valloc":                 libcValloc,
		"posix_memalign":         libcPosix_memalign,
		"malloc_usable_size":     libcMalloc_usable_size,
		"malloc_trim":            libcMalloc_trim,
		"reallocarray":           libcReallocarray,
		"strnlen":                libcStrnlen,
		"strncmp":                libcStrncmp,
		"strcasecmp":             libcStrcasecmp,
		"strncasecmp":            libcStrncasecmp,
		"strchr":                 libcStrchr,
		"strchrnul":              libcStrchrnul,
		"strrchr":                libcStrrchr,
		"strpbrk":                libcStrpbrk,
		"strstr":                 libcStrstr,
		"strcasestr":             libcStrcasestr,
		"stpcpy":                 libcStpcpy,
		"strncpy":                libcStrncpy,
		"stpncpy":                libcStpncpy,
		"mempcpy":                libcMempcpy,
		"strdup":                 libcStrdup,
		"strndup":                libcStrndup,
		"strtok_r":               libcStrtok_r,
		"strsep":                 libcStrsep,
		"strverscmp":             libcStrverscmp,
		"isspace":                libcIsspace,
		"isdigit":                libcIsdigit,
		"tolower":                libcTolower,
		"toupper":                libcToupper,
		"abs":                    libcAbs,
		"strerror":               libcStrerror,
		"strsignal":              libcStrsignal,
		"dirname":                libcDirname,
		"fnmatch":                libcFnmatch,
		"memrchr":                libcMemrchr,
		"rawmemchr":              libcRawmemchr,
		"memmem":                 libcMemmem,
		"memccpy":                libcMemccpy,
		"bcopy":                  libcBcopy,
		"bzero":                  libcBzero,
		"explicit_bzero":         libcExplicit_bzero,
		"swab":                   libcSwab,
		"strncat":                libcStrncat,
		"strlcpy":                libcStrlcpy,
		"strlcat":                libcStrlcat,
		"strtok":                 libcStrtok,
		"strcoll":                libcStrcoll,
		"strxfrm":                libcStrxfrm,
		"strerror_r":             libcStrerror_r,
		"basename":               libcBasename,
		"ffs":                    libcFfs,
		"ffsl":                   libcFfsl,
		"ffsll":                  libcFfsll,
		"labs":                   libcLabs,
		"llabs":                  libcLlabs,
		"div":                    libcDiv,
		"lldiv":                  libcLldiv,
		"index":                  libcStrchr,
		"rindex":                 libcStrrchr,
		"ldiv":                   libcDiv,
		"imaxdiv":                libcLldiv,
		"imaxabs":                libcLlabs,
		"time":                   libcTime,
		"gettimeofday":           libcGettimeofday,
		"clock_settime":          libcClock_settime,
		"localtime":              libcLocaltime,
		"localtime_r":            libcLocaltime_r,
		"tzset":                  libcTzset,
		"asctime":                libcAsctime,
		"asctime_r":              libcAsctime_r,
		"ctime_r":                libcCtime_r,
		"difftime":               libcDifftime,
		"timegm":                 libcTimegm,
		"settimeofday":           libcSettimeofday,
		"stime":                  libcStime,
		"adjtime":                libcAdjtime,
		"usleep":                 libcUsleep,
		"ualarm":                 libcUalarm,
		"gmtime":                 libcGmtime,
		"gmtime_r":               libcGmtime_r,
		"clock":                  libcClock,
		"clock_getres":           libcClock_getres,
		"clock_nanosleep":        libcClock_nanosleep,
		"setitimer":              libcSetitimer,
		"getitimer":              libcGetitimer,
		"alarm":                  libcAlarm,
		"mktime":                 libcMktime,
		"ctime":                  libcCtime,
		"strftime":               libcStrftime,
		"strptime":               libcStrptime,
		"times":                  libcTimes,
		"sleep":                  libcSleep,
		"clock_gettime":          libcClock_gettime,
		"nanosleep":              libcNanosleep,
		"regcomp":                libcRegcomp,
		"regexec":                libcRegexec,
		"regfree":                libcRegfree,
		"regerror":               libcRegerror,
		"signal":                 libcSignal,
		"sigaction":              libcSigaction,
		"sigprocmask":            libcSigprocmask,
		"pthread_sigmask":        libcPthread_sigmask,
		"sigfillset":             libcSigfillset,
		"sigemptyset":            libcSigemptyset,
		"sigaddset":              libcSigaddset,
		"sigdelset":              libcSigdelset,
		"sigismember":            libcSigismember,
		"sigisemptyset":          libcSigisemptyset,
		"sigorset":               libcSigorset,
		"sigandset":              libcSigandset,
		"sigpending":             libcSigpending,
		"sigwait":                libcSigwait,
		"sigwaitinfo":            libcSigwaitinfo,
		"sigtimedwait":           libcSigtimedwait,
		"sigaltstack":            libcSigaltstack,
		"siginterrupt":           libcSiginterrupt,
		"pause":                  libcPause,
		"sigsuspend":             libcSigsuspend,
		"raise":                  libcRaise,
		"kill":                   libcKill,
		"sigqueue":               libcSigqueue,
		"killpg":                 libcKillpg,
		"abort":                  libcAbort,
		"bsd_signal":             libcSignal,
		"getpid":                 libcGetpid,
		"getppid":                libcGetppid,
		"getpgrp":                libcGetpgrp,
		"setpgid":                libcSetpgid,
		"tcgetpgrp":              libcTcgetpgrp,
		"tcsetpgrp":              libcTcsetpgrp,
		"getuid":                 libcGetuid,
		"geteuid":                libcGeteuid,
		"getgid":                 libcGetgid,
		"getegid":                libcGetegid,
		"getgroups":              libcGetgroups,
		"uname":                  libcUname,
		"sysconf":                libcSysconf,
		"umask":                  libcUmask,
		"rand":                   libcRand,
		"srand":                  libcSrand,
		"getenv":                 libcGetenv,
		"putenv":                 libcPutenv,
		"setenv":                 libcSetenv,
		"unsetenv":               libcUnsetenv,
		"clearenv":               libcClearenv,
		"execve":                 libcExecve,
		"execvp":                 libcExecvp,
		"execv":                  libcExecv,
		"execvpe":                libcExecvpe,
		"execle":                 libcExecle,
		"fexecve":                libcFexecve,
		"execl":                  libcExecl,
		"execlp":                 libcExeclp,
		"_exit":                  libc_exit,
		"_Exit":                  libc_Exit,
		"system":                 libcSystem,
		"setjmp":                 libcSetjmp,
		"sigsetjmp":              libcSigsetjmp,
		"longjmp":                libcLongjmp,
		"_setjmp":                libcSetjmp,
		"_longjmp":               libcLongjmp,
		"siglongjmp":             libcLongjmp,
		"vfork":                  libcVfork,
		"fork":                   libcFork,
		"waitpid":                libcWaitpid,
		"wait":                   libcWait,
		"wait3":                  libcWait3,
		"wait4":                  libcWait4,
		"waitid":                 libcWaitid,
		"setlocale":              libcSetlocale,
		"localeconv":             libcLocaleconv,
		"nl_langinfo":            libcNl_langinfo,
		"isascii":                libcIsascii,
		"toascii":                libcToascii,
		"towlower":               libcTowlower,
		"towupper":               libcTowupper,
		"wctype":                 libcWctype,
		"iswctype":               libcIswctype,
		"wctrans":                libcWctrans,
		"towctrans":              libcTowctrans,
		"isalnum":                libcIsalnum,
		"isalpha":                libcIsalpha,
		"isblank":                libcIsblank,
		"iscntrl":                libcIscntrl,
		"isgraph":                libcIsgraph,
		"islower":                libcIslower,
		"isprint":                libcIsprint,
		"ispunct":                libcIspunct,
		"isupper":                libcIsupper,
		"isxdigit":               libcIsxdigit,
		"iswalnum":               libcIswalnum,
		"iswalpha":               libcIswalpha,
		"iswblank":               libcIswblank,
		"iswcntrl":               libcIswcntrl,
		"iswdigit":               libcIswdigit,
		"iswgraph":               libcIswgraph,
		"iswlower":               libcIswlower,
		"iswprint":               libcIswprint,
		"iswpunct":               libcIswpunct,
		"iswspace":               libcIswspace,
		"iswupper":               libcIswupper,
		"iswxdigit":              libcIswxdigit,
		"wcslen":                 libcWcslen,
		"wcsnlen":                libcWcsnlen,
		"wcscpy":                 libcWcscpy,
		"wcsncpy":                libcWcsncpy,
		"wcscat":                 libcWcscat,
		"wcscmp":                 libcWcscmp,
		"wcsncmp":                libcWcsncmp,
		"wcscoll":                libcWcscoll,
		"wcsxfrm":                libcWcsxfrm,
		"wcschr":                 libcWcschr,
		"wcsrchr":                libcWcsrchr,
		"wcsstr":                 libcWcsstr,
		"wcspbrk":                libcWcspbrk,
		"wcsspn":                 libcWcsspn,
		"wcscspn":                libcWcscspn,
		"wcstok":                 libcWcstok,
		"wcsdup":                 libcWcsdup,
		"wcsftime":               libcWcsftime,
		"wmemchr":                libcWmemchr,
		"wmemcmp":                libcWmemcmp,
		"wmemcpy":                libcWmemcpy,
		"wmemmove":               libcWmemmove,
		"wmemset":                libcWmemset,
		"wcwidth":                libcWcwidth,
		"wcswidth":               libcWcswidth,
		"setuid":                 libcSetuid,
		"seteuid":                libcSeteuid,
		"setreuid":               libcSetreuid,
		"setresuid":              libcSetresuid,
		"setgid":                 libcSetgid,
		"setegid":                libcSetegid,
		"setregid":               libcSetregid,
		"setresgid":              libcSetresgid,
		"getresuid":              libcGetresuid,
		"getresgid":              libcGetresgid,
		"setgroups":              libcSetgroups,
		"initgroups":             libcInitgroups,
		"setsid":                 libcSetsid,
		"getsid":                 libcGetsid,
		"getpgid":                libcGetpgid,
		"setpgrp":                libcSetpgrp,
		"nice":                   libcNice,
		"getpriority":            libcGetpriority,
		"setpriority":            libcSetpriority,
		"sched_yield":            libcSched_yield,
		"sched_get_priority_max": libcSched_get_priority_max,
		"sched_get_priority_min": libcSched_get_priority_min,
		"sched_getscheduler":     libcSched_getscheduler,
		"sched_getparam":         libcSched_getparam,
		"sched_setparam":         libcSched_setparam,
		"sched_setscheduler":     libcSched_setscheduler,
		"sched_rr_get_interval":  libcSched_rr_get_interval,
		"openlog":                libcOpenlog,
		"closelog":               libcCloselog,
		"setlogmask":             libcSetlogmask,
		"__kernel_syslog":        libc__kernel_syslog,
		"sysinfo":                libcSysinfo,
		"get_nprocs":             libcGet_nprocs,
		"get_nprocs_conf":        libcGet_nprocs_conf,
		"get_phys_pages":         libcGet_phys_pages,
		"get_avphys_pages":       libcGet_avphys_pages,
		"getloadavg":             libcGetloadavg,
		"getrusage":              libcGetrusage,
		"getrlimit":              libcGetrlimit,
		"setrlimit":              libcSetrlimit,
		"prlimit":                libcPrlimit,
		"getdtablesize":          libcGetdtablesize,
		"confstr":                libcConfstr,
		"pathconf":               libcPathconf,
		"fpathconf":              libcFpathconf,
		"getlogin":               libcGetlogin,
		"getlogin_r":             libcGetlogin_r,
		"ctermid":                libcCtermid,
		"ttyname":                libcTtyname,
		"ttyname_r":              libcTtyname_r,
		"getpwnam":               libcGetpwnam,
		"getpwuid":               libcGetpwuid,
		"getpwnam_r":             libcGetpwnam_r,
		"getpwuid_r":             libcGetpwuid_r,
		"getpwent":               libcGetpwent,
		"setpwent":               libcSetpwent,
		"endpwent":               libcEndpwent,
		"getgrnam":               libcGetgrnam,
		"getgrgid":               libcGetgrgid,
		"getgrnam_r":             libcGetgrnam_r,
		"getgrgid_r":             libcGetgrgid_r,
		"getgrent":               libcGetgrent,
		"setgrent":               libcSetgrent,
		"endgrent":               libcEndgrent,
		"getgrouplist":           libcGetgrouplist,
		"posix_openpt":           libcPosix_openpt,
		"grantpt":                libcGrantpt,
		"unlockpt":               libcUnlockpt,
		"ptsname":                libcPtsname,
		"ptsname_r":              libcPtsname_r,
		"openpty":                libcOpenpty,
		"forkpty":                libcForkpty,
		"glob":                   libcGlob,
		"globfree":               libcGlobfree,
		"random":                 libcRandom,
		"srandom":                libcSrandom,
		"initstate":              libcInitstate,
		"setstate":               libcSetstate,
		"rand_r":                 libcRand_r,
		"secure_getenv":          libcSecure_getenv,
		"getsubopt":              libcGetsubopt,
		"daemon":                 libcDaemon,
		"prctl":                  libcPrctl,
		"getpass":                libcGetpass,
		"vhangup":                libcVhangup,
		"getusershell":           libcGetusershell,
		"setusershell":           libcSetusershell,
		"endusershell":           libcEndusershell,
		"gethostname":            libcGethostname,
		"getentropy":             libcGetentropy,
		"getdomainname":          libcGetdomainname,
		"sethostname":            libcSethostname,
		"setdomainname":          libcSetdomainname,
		"gethostid":              libcGethostid,
		"sethostid":              libcSethostid,
		"psignal":                libcPsignal,
		"psiginfo":               libcPsiginfo,
		"__assert_fail":          libc__assert_fail,
		"socket":                 libcSocket,
		"socketpair":             libcSocketpair,
		"bind":                   libcBind,
		"connect":                libcConnect,
		"listen":                 libcListen,
		"accept":                 libcAccept,
		"accept4":                libcAccept4,
		"getsockname":            libcGetsockname,
		"getpeername":            libcGetpeername,
		"getsockopt":             libcGetsockopt,
		"setsockopt":             libcSetsockopt,
		"send":                   libcSend,
		"sendto":                 libcSendto,
		"recv":                   libcRecv,
		"recvfrom":               libcRecvfrom,
		"sendmsg":                libcSendmsg,
		"recvmsg":                libcRecvmsg,
		"shutdown":               libcShutdown,
		"sockatmark":             libcSockatmark,
		"getaddrinfo":            libcGetaddrinfo,
		"freeaddrinfo":           libcFreeaddrinfo,
		"gai_strerror":           libcGai_strerror,
		"getnameinfo":            libcGetnameinfo,
		"gethostbyname":          libcGethostbyname,
		"gethostbyname2":         libcGethostbyname2,
		"gethostbyaddr":          libcGethostbyaddr,
		"gethostent":             libcGethostent,
		"sethostent":             libcSethostent,
		"endhostent":             libcEndhostent,
		"getservbyname":          libcGetservbyname,
		"getservbyport":          libcGetservbyport,
		"getprotobyname":         libcGetprotobyname,
		"getprotobynumber":       libcGetprotobynumber,
		"getnetbyname":           libcGetnetbyname,
		"getnetbyaddr":           libcGetnetbyaddr,
		"hstrerror":              libcHstrerror,
		"herror":                 libcHerror,
		"inet_aton":              libcInet_aton,
		"inet_addr":              libcInet_addr,
		"inet_network":           libcInet_network,
		"inet_ntoa":              libcInet_ntoa,
		"inet_pton":              libcInet_pton,
		"inet_ntop":              libcInet_ntop,
		"inet_makeaddr":          libcInet_makeaddr,
		"inet_lnaof":             libcInet_lnaof,
		"inet_netof":             libcInet_netof,
		"if_nametoindex":         libcIf_nametoindex,
		"if_indextoname":         libcIf_indextoname,
		"if_nameindex":           libcIf_nameindex,
		"if_freenameindex":       libcIf_freenameindex,
		"crypt":                  libcCrypt,
	}
}

func libc__kernel_start(ctx uintptr, errnoAt, environAt, argcAt, argvAt, tznameAt, timezoneAt, daylightAt, herrnoAt uint32) {
	proc(ctx).libc__kernel_start(errnoAt, environAt, argcAt, argvAt, tznameAt, timezoneAt, daylightAt, herrnoAt)
}

func libc__kernel_program(ctx uintptr, buf, size uint32) uint32 {
	return proc(ctx).libc__kernel_program(buf, size)
}

func libcRead(ctx uintptr, fd, buf, n uint32) uint32 {
	return proc(ctx).libcRead(fd, buf, n)
}

func libcWrite(ctx uintptr, fd, buf, n uint32) uint32 {
	return proc(ctx).libcWrite(fd, buf, n)
}

func libcReadv(ctx uintptr, fd, iovs, iovsSize, count uint32) uint32 {
	p := proc(ctx)
	p.sized("readv", iovsSize, sizeofIovec)

	return p.libcReadv(fd, iovs, count)
}

func libcWritev(ctx uintptr, fd, iovs, iovsSize, count uint32) uint32 {
	p := proc(ctx)
	p.sized("writev", iovsSize, sizeofIovec)

	return p.libcWritev(fd, iovs, count)
}

func libcPreadv(ctx uintptr, fd, iovs, iovsSize, count uint32, off uint64) uint32 {
	p := proc(ctx)
	p.sized("preadv", iovsSize, sizeofIovec)

	return p.libcPreadv(fd, iovs, count, off)
}

func libcPwritev(ctx uintptr, fd, iovs, iovsSize, count uint32, off uint64) uint32 {
	p := proc(ctx)
	p.sized("pwritev", iovsSize, sizeofIovec)

	return p.libcPwritev(fd, iovs, count, off)
}

func libcPread(ctx uintptr, fd, buf, n uint32, off uint64) uint32 {
	return proc(ctx).libcPread(fd, buf, n, off)
}

func libcPwrite(ctx uintptr, fd, buf, n uint32, off uint64) uint32 {
	return proc(ctx).libcPwrite(fd, buf, n, off)
}

func libcCopy_file_range(ctx uintptr, in, offIn, out, offOut, n, flags uint32) uint32 {
	return proc(ctx).libcCopy_file_range(in, offIn, out, offOut, n, flags)
}

func libcFdatasync(ctx uintptr, fd uint32) uint32 {
	return proc(ctx).libcFdatasync(fd)
}

func libcSync(ctx uintptr) {
	proc(ctx).libcSync()
}

func libcSyncfs(ctx uintptr, fd uint32) uint32 {
	return proc(ctx).libcSyncfs(fd)
}

func libcFlock(ctx uintptr, fd, op uint32) uint32 {
	return proc(ctx).libcFlock(fd, op)
}

func libcLockf(ctx uintptr, fd, cmd uint32, n uint64) uint32 {
	return proc(ctx).libcLockf(fd, cmd, n)
}

func libcPosix_fadvise(ctx uintptr, fd uint32, off, n uint64, advice uint32) uint32 {
	return proc(ctx).libcPosix_fadvise(fd, off, n, advice)
}

func libcPosix_fallocate(ctx uintptr, fd uint32, off, n uint64) uint32 {
	return proc(ctx).libcPosix_fallocate(fd, off, n)
}

func libcSelect(ctx uintptr, n, r, rSize, w, wSize, e, eSize, tv, tvSize uint32) uint32 {
	p := proc(ctx)
	p.sized("select", rSize, sizeofFdSet, wSize, sizeofFdSet, eSize, sizeofFdSet, tvSize, sizeofTimeval)

	return p.libcSelect(n, r, w, e, tv)
}

func libcPselect(ctx uintptr, n, r, rSize, w, wSize, e, eSize, ts, tsSize, mask, maskSize uint32) uint32 {
	p := proc(ctx)
	p.sized("pselect", rSize, sizeofFdSet, wSize, sizeofFdSet, eSize, sizeofFdSet, tsSize, sizeofTimespec, maskSize, sizeofSigset)

	return p.libcPselect(n, r, w, e, ts, mask)
}

func libcLseek(ctx uintptr, fd uint32, off uint64, whence uint32) uint64 {
	return proc(ctx).libcLseek(fd, off, whence)
}

func libcClose(ctx uintptr, fd uint32) uint32 {
	return proc(ctx).libcClose(fd)
}

func libcDup(ctx uintptr, fd uint32) uint32 {
	return proc(ctx).libcDup(fd)
}

func libcDup2(ctx uintptr, old, new uint32) uint32 {
	return proc(ctx).libcDup2(old, new)
}

func libcDup3(ctx uintptr, old, new, flags uint32) uint32 {
	return proc(ctx).libcDup3(old, new, flags)
}

func libcPipe(ctx uintptr, fds uint32) uint32 {
	return proc(ctx).libcPipe(fds)
}

func libcPipe2(ctx uintptr, fds, flags uint32) uint32 {
	return proc(ctx).libcPipe2(fds, flags)
}

func libcTcdrain(ctx uintptr, fd uint32) uint32 {
	return proc(ctx).libcTcdrain(fd)
}

func libcTcsendbreak(ctx uintptr, fd, duration uint32) uint32 {
	return proc(ctx).libcTcsendbreak(fd, duration)
}

func libcTcflow(ctx uintptr, fd, action uint32) uint32 {
	return proc(ctx).libcTcflow(fd, action)
}

func libcTcflush(ctx uintptr, fd, queue uint32) uint32 {
	return proc(ctx).libcTcflush(fd, queue)
}

func libcTcgetsid(ctx uintptr, fd uint32) uint32 {
	return proc(ctx).libcTcgetsid(fd)
}

func libcCfgetospeed(ctx uintptr, at, atSize uint32) uint32 {
	p := proc(ctx)
	p.sized("cfgetospeed", atSize, sizeofTermios)

	return p.libcCfgetospeed(at)
}

func libcCfgetispeed(ctx uintptr, at, atSize uint32) uint32 {
	p := proc(ctx)
	p.sized("cfgetispeed", atSize, sizeofTermios)

	return p.libcCfgetispeed(at)
}

func libcCfsetospeed(ctx uintptr, at, atSize, speed uint32) uint32 {
	p := proc(ctx)
	p.sized("cfsetospeed", atSize, sizeofTermios)

	return p.libcCfsetospeed(at, speed)
}

func libcCfsetspeed(ctx uintptr, at, atSize, speed uint32) uint32 {
	p := proc(ctx)
	p.sized("cfsetspeed", atSize, sizeofTermios)

	return p.libcCfsetspeed(at, speed)
}

func libcCfsetispeed(ctx uintptr, at, atSize, speed uint32) uint32 {
	p := proc(ctx)
	p.sized("cfsetispeed", atSize, sizeofTermios)

	return p.libcCfsetispeed(at, speed)
}

func libcFsync(ctx uintptr, fd uint32) uint32 {
	return proc(ctx).libcFsync(fd)
}

func libcFtruncate(ctx uintptr, fd uint32, size uint64) uint32 {
	return proc(ctx).libcFtruncate(fd, size)
}

func libcFstat(ctx uintptr, fd, at, atSize uint32) uint32 {
	p := proc(ctx)
	p.sized("fstat", atSize, sizeofStat)

	return p.libcFstat(fd, at)
}

func libcFchmod(ctx uintptr, fd, mode uint32) uint32 {
	return proc(ctx).libcFchmod(fd, mode)
}

func libcFchown(ctx uintptr, fd, uid, gid uint32) uint32 {
	return proc(ctx).libcFchown(fd, uid, gid)
}

func libcFutimes(ctx uintptr, fd, times, timesSize uint32) uint32 {
	p := proc(ctx)
	p.sized("futimes", timesSize, sizeofTimeval)

	return p.libcFutimes(fd, times)
}

func libcFutimens(ctx uintptr, fd, times, timesSize uint32) uint32 {
	p := proc(ctx)
	p.sized("futimens", timesSize, sizeofTimespec)

	return p.libcFutimens(fd, times)
}

func libcFcntl(ctx uintptr, fd, cmd, va, size uint32) uint32 {
	p := proc(ctx)
	p.sized("fcntl", size, fcntlSize(cmd))

	return p.libcFcntl(fd, cmd, va)
}

func libcIoctl(ctx uintptr, fd, req, va, size uint32) uint32 {
	p := proc(ctx)
	p.sized("ioctl", size, ioctlSize(req))

	return p.libcIoctl(fd, req, va)
}

func libcIsatty(ctx uintptr, fd uint32) uint32 {
	return proc(ctx).libcIsatty(fd)
}

func libcTcgetattr(ctx uintptr, fd, at, atSize uint32) uint32 {
	p := proc(ctx)
	p.sized("tcgetattr", atSize, sizeofTermios)

	return p.libcTcgetattr(fd, at)
}

func libcTcsetattr(ctx uintptr, fd, action, at, atSize uint32) uint32 {
	p := proc(ctx)
	p.sized("tcsetattr", atSize, sizeofTermios)

	return p.libcTcsetattr(fd, action, at)
}

func libcCfmakeraw(ctx uintptr, at, atSize uint32) {
	p := proc(ctx)
	p.sized("cfmakeraw", atSize, sizeofTermios)

	p.libcCfmakeraw(at)
}

func libcPoll(ctx uintptr, at, atSize, n, timeout uint32) uint32 {
	p := proc(ctx)
	p.sized("poll", atSize, sizeofPollfd)

	return p.libcPoll(at, n, timeout)
}

func libcPpoll(ctx uintptr, at, atSize, n, ts, tsSize, mask, maskSize uint32) uint32 {
	p := proc(ctx)
	p.sized("ppoll", atSize, sizeofPollfd, tsSize, sizeofTimespec, maskSize, sizeofSigset)

	return p.libcPpoll(at, n, ts, mask)
}

func libcOpen(ctx uintptr, path, flags, va uint32) uint32 {
	return proc(ctx).libcOpen(path, flags, va)
}

func libcOpenat(ctx uintptr, dirfd, path, flags, va uint32) uint32 {
	return proc(ctx).libcOpenat(dirfd, path, flags, va)
}

func libcStat(ctx uintptr, path, at, atSize uint32) uint32 {
	p := proc(ctx)
	p.sized("stat", atSize, sizeofStat)

	return p.libcStat(path, at)
}

func libcLstat(ctx uintptr, path, at, atSize uint32) uint32 {
	p := proc(ctx)
	p.sized("lstat", atSize, sizeofStat)

	return p.libcLstat(path, at)
}

func libcFstatat(ctx uintptr, dirfd, path, at, atSize, flags uint32) uint32 {
	p := proc(ctx)
	p.sized("fstatat", atSize, sizeofStat)

	return p.libcFstatat(dirfd, path, at, flags)
}

func libcAccess(ctx uintptr, path, mode uint32) uint32 {
	return proc(ctx).libcAccess(path, mode)
}

func libcFaccessat(ctx uintptr, dirfd, path, mode, flags uint32) uint32 {
	return proc(ctx).libcFaccessat(dirfd, path, mode, flags)
}

func libcTruncate(ctx uintptr, path uint32, size uint64) uint32 {
	return proc(ctx).libcTruncate(path, size)
}

func libcStatvfs(ctx uintptr, path, at, atSize uint32) uint32 {
	p := proc(ctx)
	p.sized("statvfs", atSize, sizeofStatvfs)

	return p.libcStatvfs(path, at)
}

func libcFstatvfs(ctx uintptr, fd, at, atSize uint32) uint32 {
	p := proc(ctx)
	p.sized("fstatvfs", atSize, sizeofStatvfs)

	return p.libcFstatvfs(fd, at)
}

func libcFchdir(ctx uintptr, fd uint32) uint32 {
	return proc(ctx).libcFchdir(fd)
}

func libcChroot(ctx uintptr, path uint32) uint32 {
	return proc(ctx).libcChroot(path)
}

func libcChdir(ctx uintptr, path uint32) uint32 {
	return proc(ctx).libcChdir(path)
}

func libcGetcwd(ctx uintptr, buf, size uint32) uint32 {
	return proc(ctx).libcGetcwd(buf, size)
}

func libcMkdir(ctx uintptr, path, mode uint32) uint32 {
	return proc(ctx).libcMkdir(path, mode)
}

func libcMkdirat(ctx uintptr, dirfd, path, mode uint32) uint32 {
	return proc(ctx).libcMkdirat(dirfd, path, mode)
}

func libcRmdir(ctx uintptr, path uint32) uint32 {
	return proc(ctx).libcRmdir(path)
}

func libcUnlink(ctx uintptr, path uint32) uint32 {
	return proc(ctx).libcUnlink(path)
}

func libcUnlinkat(ctx uintptr, dirfd, path, flags uint32) uint32 {
	return proc(ctx).libcUnlinkat(dirfd, path, flags)
}

func libcRemove(ctx uintptr, path uint32) uint32 {
	return proc(ctx).libcRemove(path)
}

func libcRename(ctx uintptr, old, new uint32) uint32 {
	return proc(ctx).libcRename(old, new)
}

func libcRenameat(ctx uintptr, olddir, old, newdir, new uint32) uint32 {
	return proc(ctx).libcRenameat(olddir, old, newdir, new)
}

func libcLink(ctx uintptr, old, new uint32) uint32 {
	return proc(ctx).libcLink(old, new)
}

func libcLinkat(ctx uintptr, olddir, old, newdir, new, flags uint32) uint32 {
	return proc(ctx).libcLinkat(olddir, old, newdir, new, flags)
}

func libcSymlink(ctx uintptr, target, path uint32) uint32 {
	return proc(ctx).libcSymlink(target, path)
}

func libcSymlinkat(ctx uintptr, target, dirfd, path uint32) uint32 {
	return proc(ctx).libcSymlinkat(target, dirfd, path)
}

func libcReadlink(ctx uintptr, path, buf, size uint32) uint32 {
	return proc(ctx).libcReadlink(path, buf, size)
}

func libcReadlinkat(ctx uintptr, dirfd, path, buf, size uint32) uint32 {
	return proc(ctx).libcReadlinkat(dirfd, path, buf, size)
}

func libcCanonicalize_file_name(ctx uintptr, path uint32) uint32 {
	return proc(ctx).libcCanonicalize_file_name(path)
}

func libcGet_current_dir_name(ctx uintptr) uint32 {
	return proc(ctx).libcGet_current_dir_name()
}

func libcCreat(ctx uintptr, path, mode uint32) uint32 {
	return proc(ctx).libcCreat(path, mode)
}

func libcEaccess(ctx uintptr, path, mode uint32) uint32 {
	return proc(ctx).libcEaccess(path, mode)
}

func libcEuidaccess(ctx uintptr, path, mode uint32) uint32 {
	return proc(ctx).libcEuidaccess(path, mode)
}

func libcStatfs(ctx uintptr, path, at, atSize uint32) uint32 {
	p := proc(ctx)
	p.sized("statfs", atSize, sizeofStatfs)

	return p.libcStatfs(path, at)
}

func libcFstatfs(ctx uintptr, fd, at, atSize uint32) uint32 {
	p := proc(ctx)
	p.sized("fstatfs", atSize, sizeofStatfs)

	return p.libcFstatfs(fd, at)
}

func libcAcct(ctx uintptr, path uint32) uint32 {
	return proc(ctx).libcAcct(path)
}

func libcRealpath(ctx uintptr, path, resolved uint32) uint32 {
	return proc(ctx).libcRealpath(path, resolved)
}

func libcChmod(ctx uintptr, path, mode uint32) uint32 {
	return proc(ctx).libcChmod(path, mode)
}

func libcFchmodat(ctx uintptr, dirfd, path, mode, flags uint32) uint32 {
	return proc(ctx).libcFchmodat(dirfd, path, mode, flags)
}

func libcChown(ctx uintptr, path, uid, gid uint32) uint32 {
	return proc(ctx).libcChown(path, uid, gid)
}

func libcLchown(ctx uintptr, path, uid, gid uint32) uint32 {
	return proc(ctx).libcLchown(path, uid, gid)
}

func libcFchownat(ctx uintptr, dirfd, path, uid, gid, flags uint32) uint32 {
	return proc(ctx).libcFchownat(dirfd, path, uid, gid, flags)
}

func libcMknod(ctx uintptr, path, mode uint32, dev uint64) uint32 {
	return proc(ctx).libcMknod(path, mode, dev)
}

func libcMknodat(ctx uintptr, dirfd, path, mode uint32, dev uint64) uint32 {
	return proc(ctx).libcMknodat(dirfd, path, mode, dev)
}

func libcMkfifo(ctx uintptr, path, mode uint32) uint32 {
	return proc(ctx).libcMkfifo(path, mode)
}

func libcMkfifoat(ctx uintptr, dirfd, path, mode uint32) uint32 {
	return proc(ctx).libcMkfifoat(dirfd, path, mode)
}

func libcUtimes(ctx uintptr, path, times, timesSize uint32) uint32 {
	p := proc(ctx)
	p.sized("utimes", timesSize, sizeofTimeval)

	return p.libcUtimes(path, times)
}

func libcLutimes(ctx uintptr, path, times, timesSize uint32) uint32 {
	p := proc(ctx)
	p.sized("lutimes", timesSize, sizeofTimeval)

	return p.libcLutimes(path, times)
}

func libcUtime(ctx uintptr, path, times, timesSize uint32) uint32 {
	p := proc(ctx)
	p.sized("utime", timesSize, sizeofUtimbuf)

	return p.libcUtime(path, times)
}

func libcUtimensat(ctx uintptr, dirfd, path, times, timesSize, flags uint32) uint32 {
	p := proc(ctx)
	p.sized("utimensat", timesSize, sizeofTimespec)

	return p.libcUtimensat(dirfd, path, times, flags)
}

func libcDirfd(ctx uintptr, dir uint32) uint32 {
	return proc(ctx).libcDirfd(dir)
}

func libcReaddir_r(ctx uintptr, dir, entry, entrySize, result, resultSize uint32) uint32 {
	p := proc(ctx)
	p.sized("readdir_r", entrySize, sizeofDirent, resultSize, sizeofDirent)

	return p.libcReaddir_r(dir, entry, result)
}

func libcTelldir(ctx uintptr, dir uint32) uint32 {
	return proc(ctx).libcTelldir(dir)
}

func libcSeekdir(ctx uintptr, dir, at uint32) {
	proc(ctx).libcSeekdir(dir, at)
}

func libcAlphasort(ctx uintptr, a, aSize, b, bSize uint32) uint32 {
	p := proc(ctx)
	p.sized("alphasort", aSize, sizeofDirent, bSize, sizeofDirent)

	return p.libcAlphasort(a, b)
}

func libcVersionsort(ctx uintptr, a, aSize, b, bSize uint32) uint32 {
	p := proc(ctx)
	p.sized("versionsort", aSize, sizeofDirent, bSize, sizeofDirent)

	return p.libcVersionsort(a, b)
}

func libcMkstemp(ctx uintptr, template uint32) uint32 {
	return proc(ctx).libcMkstemp(template)
}

func libcMkostemp(ctx uintptr, template, flags uint32) uint32 {
	return proc(ctx).libcMkostemp(template, flags)
}

func libcMkstemps(ctx uintptr, template, suffix uint32) uint32 {
	return proc(ctx).libcMkstemps(template, suffix)
}

func libcMkdtemp(ctx uintptr, template uint32) uint32 {
	return proc(ctx).libcMkdtemp(template)
}

func libcMktemp(ctx uintptr, template uint32) uint32 {
	return proc(ctx).libcMktemp(template)
}

func libcOpendir(ctx uintptr, path uint32) uint32 {
	return proc(ctx).libcOpendir(path)
}

func libcFdopendir(ctx uintptr, fd uint32) uint32 {
	return proc(ctx).libcFdopendir(fd)
}

func libcRewinddir(ctx uintptr, dir uint32) {
	proc(ctx).libcRewinddir(dir)
}

func libcReaddir(ctx uintptr, dir, size uint32) uint32 {
	p := proc(ctx)
	p.sized("readdir", size, sizeofDirent)

	return p.libcReaddir(dir)
}

func libcClosedir(ctx uintptr, dir uint32) uint32 {
	return proc(ctx).libcClosedir(dir)
}

func libcMalloc(ctx uintptr, n uint32) uint32 {
	return proc(ctx).libcMalloc(n)
}

func libcFree(ctx uintptr, ptr uint32) {
	proc(ctx).libcFree(ptr)
}

func libcCalloc(ctx uintptr, n, size uint32) uint32 {
	return proc(ctx).libcCalloc(n, size)
}

func libcRealloc(ctx uintptr, ptr, n uint32) uint32 {
	return proc(ctx).libcRealloc(ptr, n)
}

func libcMallopt(ctx uintptr, param, value uint32) uint32 {
	return proc(ctx).libcMallopt(param, value)
}

func libcGetpagesize(ctx uintptr) uint32 {
	return proc(ctx).libcGetpagesize()
}

func libcMmap(ctx uintptr, addr, n, prot, flags, fd uint32, off uint64) uint32 {
	return proc(ctx).libcMmap(addr, n, prot, flags, fd, off)
}

func libcMunmap(ctx uintptr, addr, n uint32) uint32 {
	return proc(ctx).libcMunmap(addr, n)
}

func libcMsync(ctx uintptr, addr, n, flags uint32) uint32 {
	return proc(ctx).libcMsync(addr, n, flags)
}

func libcMadvise(ctx uintptr, addr, n, advice uint32) uint32 {
	return proc(ctx).libcMadvise(addr, n, advice)
}

func libcMprotect(ctx uintptr, addr, n, prot uint32) uint32 {
	return proc(ctx).libcMprotect(addr, n, prot)
}

func libcMlock(ctx uintptr, addr, n uint32) uint32 {
	return proc(ctx).libcMlock(addr, n)
}

func libcMunlock(ctx uintptr, addr, n uint32) uint32 {
	return proc(ctx).libcMunlock(addr, n)
}

func libcMlockall(ctx uintptr, flags uint32) uint32 {
	return proc(ctx).libcMlockall(flags)
}

func libcMunlockall(ctx uintptr) uint32 {
	return proc(ctx).libcMunlockall()
}

func libcMemalign(ctx uintptr, align, n uint32) uint32 {
	return proc(ctx).libcMemalign(align, n)
}

func libcAligned_alloc(ctx uintptr, align, n uint32) uint32 {
	return proc(ctx).libcAligned_alloc(align, n)
}

func libcValloc(ctx uintptr, n uint32) uint32 {
	return proc(ctx).libcValloc(n)
}

func libcPosix_memalign(ctx uintptr, out, align, n uint32) uint32 {
	return proc(ctx).libcPosix_memalign(out, align, n)
}

func libcMalloc_usable_size(ctx uintptr, ptr uint32) uint32 {
	return proc(ctx).libcMalloc_usable_size(ptr)
}

func libcMalloc_trim(ctx uintptr, pad uint32) uint32 {
	return proc(ctx).libcMalloc_trim(pad)
}

func libcReallocarray(ctx uintptr, ptr, n, size uint32) uint32 {
	return proc(ctx).libcReallocarray(ptr, n, size)
}

func libcStrnlen(ctx uintptr, s, n uint32) uint32 {
	return proc(ctx).libcStrnlen(s, n)
}

func libcStrncmp(ctx uintptr, a, b, n uint32) uint32 {
	return proc(ctx).libcStrncmp(a, b, n)
}

func libcStrcasecmp(ctx uintptr, a, b uint32) uint32 {
	return proc(ctx).libcStrcasecmp(a, b)
}

func libcStrncasecmp(ctx uintptr, a, b, n uint32) uint32 {
	return proc(ctx).libcStrncasecmp(a, b, n)
}

func libcStrchr(ctx uintptr, s, c uint32) uint32 {
	return proc(ctx).libcStrchr(s, c)
}

func libcStrchrnul(ctx uintptr, s, c uint32) uint32 {
	return proc(ctx).libcStrchrnul(s, c)
}

func libcStrrchr(ctx uintptr, s, c uint32) uint32 {
	return proc(ctx).libcStrrchr(s, c)
}

func libcStrpbrk(ctx uintptr, s, accept uint32) uint32 {
	return proc(ctx).libcStrpbrk(s, accept)
}

func libcStrstr(ctx uintptr, h, n uint32) uint32 {
	return proc(ctx).libcStrstr(h, n)
}

func libcStrcasestr(ctx uintptr, h, n uint32) uint32 {
	return proc(ctx).libcStrcasestr(h, n)
}

func libcStpcpy(ctx uintptr, d, s uint32) uint32 {
	return proc(ctx).libcStpcpy(d, s)
}

func libcStrncpy(ctx uintptr, d, s, n uint32) uint32 {
	return proc(ctx).libcStrncpy(d, s, n)
}

func libcStpncpy(ctx uintptr, d, s, n uint32) uint32 {
	return proc(ctx).libcStpncpy(d, s, n)
}

func libcMempcpy(ctx uintptr, d, s, n uint32) uint32 {
	return proc(ctx).libcMempcpy(d, s, n)
}

func libcStrdup(ctx uintptr, s uint32) uint32 {
	return proc(ctx).libcStrdup(s)
}

func libcStrndup(ctx uintptr, s, n uint32) uint32 {
	return proc(ctx).libcStrndup(s, n)
}

func libcStrtok_r(ctx uintptr, s, delim, save uint32) uint32 {
	return proc(ctx).libcStrtok_r(s, delim, save)
}

func libcStrsep(ctx uintptr, at, delim uint32) uint32 {
	return proc(ctx).libcStrsep(at, delim)
}

func libcStrverscmp(ctx uintptr, a, b uint32) uint32 {
	return proc(ctx).libcStrverscmp(a, b)
}

func libcIsspace(ctx uintptr, c uint32) uint32 {
	return proc(ctx).libcIsspace(c)
}

func libcIsdigit(ctx uintptr, c uint32) uint32 {
	return proc(ctx).libcIsdigit(c)
}

func libcTolower(ctx uintptr, c uint32) uint32 {
	return proc(ctx).libcTolower(c)
}

func libcToupper(ctx uintptr, c uint32) uint32 {
	return proc(ctx).libcToupper(c)
}

func libcAbs(ctx uintptr, n uint32) uint32 {
	return proc(ctx).libcAbs(n)
}

func libcStrerror(ctx uintptr, e uint32) uint32 {
	return proc(ctx).libcStrerror(e)
}

func libcStrsignal(ctx uintptr, sig uint32) uint32 {
	return proc(ctx).libcStrsignal(sig)
}

func libcDirname(ctx uintptr, s uint32) uint32 {
	return proc(ctx).libcDirname(s)
}

func libcFnmatch(ctx uintptr, pattern, s, flags uint32) uint32 {
	return proc(ctx).libcFnmatch(pattern, s, flags)
}

func libcMemrchr(ctx uintptr, s, c, n uint32) uint32 {
	return proc(ctx).libcMemrchr(s, c, n)
}

func libcRawmemchr(ctx uintptr, s, c uint32) uint32 {
	return proc(ctx).libcRawmemchr(s, c)
}

func libcMemmem(ctx uintptr, h, hn, n, nn uint32) uint32 {
	return proc(ctx).libcMemmem(h, hn, n, nn)
}

func libcMemccpy(ctx uintptr, d, s, c, n uint32) uint32 {
	return proc(ctx).libcMemccpy(d, s, c, n)
}

func libcBcopy(ctx uintptr, s, d, n uint32) {
	proc(ctx).libcBcopy(s, d, n)
}

func libcBzero(ctx uintptr, d, n uint32) {
	proc(ctx).libcBzero(d, n)
}

func libcExplicit_bzero(ctx uintptr, d, n uint32) {
	proc(ctx).libcExplicit_bzero(d, n)
}

func libcSwab(ctx uintptr, s, d, n uint32) {
	proc(ctx).libcSwab(s, d, n)
}

func libcStrncat(ctx uintptr, d, s, n uint32) uint32 {
	return proc(ctx).libcStrncat(d, s, n)
}

func libcStrlcpy(ctx uintptr, d, s, n uint32) uint32 {
	return proc(ctx).libcStrlcpy(d, s, n)
}

func libcStrlcat(ctx uintptr, d, s, n uint32) uint32 {
	return proc(ctx).libcStrlcat(d, s, n)
}

func libcStrtok(ctx uintptr, s, delim uint32) uint32 {
	return proc(ctx).libcStrtok(s, delim)
}

func libcStrcoll(ctx uintptr, a, b uint32) uint32 {
	return proc(ctx).libcStrcoll(a, b)
}

func libcStrxfrm(ctx uintptr, d, s, n uint32) uint32 {
	return proc(ctx).libcStrxfrm(d, s, n)
}

func libcStrerror_r(ctx uintptr, e, buf, n uint32) uint32 {
	return proc(ctx).libcStrerror_r(e, buf, n)
}

func libcBasename(ctx uintptr, s uint32) uint32 {
	return proc(ctx).libcBasename(s)
}

func libcFfs(ctx uintptr, x uint32) uint32 {
	return proc(ctx).libcFfs(x)
}

func libcFfsl(ctx uintptr, x uint32) uint32 {
	return proc(ctx).libcFfsl(x)
}

func libcFfsll(ctx uintptr, x uint64) uint32 {
	return proc(ctx).libcFfsll(x)
}

func libcLabs(ctx uintptr, n uint32) uint32 {
	return proc(ctx).libcLabs(n)
}

func libcLlabs(ctx uintptr, n uint64) uint64 {
	return proc(ctx).libcLlabs(n)
}

func libcDiv(ctx uintptr, ret, a, b uint32) {
	proc(ctx).libcDiv(ret, a, b)
}

func libcLldiv(ctx uintptr, ret uint32, a, b uint64) {
	proc(ctx).libcLldiv(ret, a, b)
}

func libcTime(ctx uintptr, at uint32) uint64 {
	return proc(ctx).libcTime(at)
}

func libcGettimeofday(ctx uintptr, tv, tvSize, tz uint32) uint32 {
	p := proc(ctx)
	p.sized("gettimeofday", tvSize, sizeofTimeval)

	return p.libcGettimeofday(tv, tz)
}

func libcClock_settime(ctx uintptr, clock, ts, tsSize uint32) uint32 {
	p := proc(ctx)
	p.sized("clock_settime", tsSize, sizeofTimespec)

	return p.libcClock_settime(clock, ts)
}

func libcLocaltime(ctx uintptr, t, size uint32) uint32 {
	p := proc(ctx)
	p.sized("localtime", size, sizeofTm)

	return p.libcLocaltime(t)
}

func libcLocaltime_r(ctx uintptr, t, at, atSize, size uint32) uint32 {
	p := proc(ctx)
	p.sized("localtime_r", atSize, sizeofTm, size, sizeofTm)

	return p.libcLocaltime_r(t, at)
}

func libcTzset(ctx uintptr) {
	proc(ctx).libcTzset()
}

func libcAsctime(ctx uintptr, tm, tmSize uint32) uint32 {
	p := proc(ctx)
	p.sized("asctime", tmSize, sizeofTm)

	return p.libcAsctime(tm)
}

func libcAsctime_r(ctx uintptr, tm, tmSize, buf uint32) uint32 {
	p := proc(ctx)
	p.sized("asctime_r", tmSize, sizeofTm)

	return p.libcAsctime_r(tm, buf)
}

func libcCtime_r(ctx uintptr, t, buf uint32) uint32 {
	return proc(ctx).libcCtime_r(t, buf)
}

func libcDifftime(ctx uintptr, t1, t0 uint64) float64 {
	return proc(ctx).libcDifftime(t1, t0)
}

func libcTimegm(ctx uintptr, at, atSize uint32) uint64 {
	p := proc(ctx)
	p.sized("timegm", atSize, sizeofTm)

	return p.libcTimegm(at)
}

func libcSettimeofday(ctx uintptr, tv, tvSize, tz, tzSize uint32) uint32 {
	p := proc(ctx)
	p.sized("settimeofday", tvSize, sizeofTimeval, tzSize, sizeofTimezone)

	return p.libcSettimeofday(tv, tz)
}

func libcStime(ctx uintptr, t uint32) uint32 {
	return proc(ctx).libcStime(t)
}

func libcAdjtime(ctx uintptr, delta, deltaSize, old, oldSize uint32) uint32 {
	p := proc(ctx)
	p.sized("adjtime", deltaSize, sizeofTimeval, oldSize, sizeofTimeval)

	return p.libcAdjtime(delta, old)
}

func libcUsleep(ctx uintptr, us uint32) uint32 {
	return proc(ctx).libcUsleep(us)
}

func libcUalarm(ctx uintptr, value, interval uint32) uint32 {
	return proc(ctx).libcUalarm(value, interval)
}

func libcGmtime(ctx uintptr, t, size uint32) uint32 {
	p := proc(ctx)
	p.sized("gmtime", size, sizeofTm)

	return p.libcGmtime(t)
}

func libcGmtime_r(ctx uintptr, t, at, atSize, size uint32) uint32 {
	p := proc(ctx)
	p.sized("gmtime_r", atSize, sizeofTm, size, sizeofTm)

	return p.libcGmtime_r(t, at)
}

func libcClock(ctx uintptr) uint64 {
	return proc(ctx).libcClock()
}

func libcClock_getres(ctx uintptr, clock, res, resSize uint32) uint32 {
	p := proc(ctx)
	p.sized("clock_getres", resSize, sizeofTimespec)

	return p.libcClock_getres(clock, res)
}

func libcClock_nanosleep(ctx uintptr, clock, flags, req, reqSize, rem, remSize uint32) uint32 {
	p := proc(ctx)
	p.sized("clock_nanosleep", reqSize, sizeofTimespec, remSize, sizeofTimespec)

	return p.libcClock_nanosleep(clock, flags, req, rem)
}

func libcSetitimer(ctx uintptr, which, value, valueSize, old, oldSize uint32) uint32 {
	p := proc(ctx)
	p.sized("setitimer", valueSize, sizeofItimerval, oldSize, sizeofItimerval)

	return p.libcSetitimer(which, value, old)
}

func libcGetitimer(ctx uintptr, which, at, atSize uint32) uint32 {
	p := proc(ctx)
	p.sized("getitimer", atSize, sizeofItimerval)

	return p.libcGetitimer(which, at)
}

func libcAlarm(ctx uintptr, seconds uint32) uint32 {
	return proc(ctx).libcAlarm(seconds)
}

func libcMktime(ctx uintptr, at, atSize uint32) uint64 {
	p := proc(ctx)
	p.sized("mktime", atSize, sizeofTm)

	return p.libcMktime(at)
}

func libcCtime(ctx uintptr, t uint32) uint32 {
	return proc(ctx).libcCtime(t)
}

func libcStrftime(ctx uintptr, s, max, format, tm, tmSize uint32) uint32 {
	p := proc(ctx)
	p.sized("strftime", tmSize, sizeofTm)

	return p.libcStrftime(s, max, format, tm)
}

func libcStrptime(ctx uintptr, s, format, tm, tmSize uint32) uint32 {
	p := proc(ctx)
	p.sized("strptime", tmSize, sizeofTm)

	return p.libcStrptime(s, format, tm)
}

func libcTimes(ctx uintptr, at, atSize uint32) uint64 {
	p := proc(ctx)
	p.sized("times", atSize, sizeofTms)

	return p.libcTimes(at)
}

func libcSleep(ctx uintptr, seconds uint32) uint32 {
	return proc(ctx).libcSleep(seconds)
}

func libcClock_gettime(ctx uintptr, clock, ts, tsSize uint32) uint32 {
	p := proc(ctx)
	p.sized("clock_gettime", tsSize, sizeofTimespec)

	return p.libcClock_gettime(clock, ts)
}

func libcNanosleep(ctx uintptr, req, reqSize, rem, remSize uint32) uint32 {
	p := proc(ctx)
	p.sized("nanosleep", reqSize, sizeofTimespec, remSize, sizeofTimespec)

	return p.libcNanosleep(req, rem)
}

func libcRegcomp(ctx uintptr, preg, pregSize, pattern, flags uint32) uint32 {
	p := proc(ctx)
	p.sized("regcomp", pregSize, sizeofRegex)

	return p.libcRegcomp(preg, pattern, flags)
}

func libcRegexec(ctx uintptr, preg, pregSize, s, nmatch, pmatch, pmatchSize, eflags uint32) uint32 {
	p := proc(ctx)
	p.sized("regexec", pregSize, sizeofRegex, pmatchSize, sizeofRegmatch)

	return p.libcRegexec(preg, s, nmatch, pmatch, eflags)
}

func libcRegfree(ctx uintptr, preg, pregSize uint32) {
	p := proc(ctx)
	p.sized("regfree", pregSize, sizeofRegex)

	p.libcRegfree(preg)
}

func libcRegerror(ctx uintptr, code, preg, pregSize, buf, size uint32) uint32 {
	p := proc(ctx)
	p.sized("regerror", pregSize, sizeofRegex)

	return p.libcRegerror(code, preg, buf, size)
}

func libcSignal(ctx uintptr, sig, handler uint32) uint32 {
	return proc(ctx).libcSignal(sig, handler)
}

func libcSigaction(ctx uintptr, sig, act, actSize, old, oldSize, infoSize uint32) uint32 {
	p := proc(ctx)
	p.sized("sigaction", actSize, sizeofSigaction, oldSize, sizeofSigaction, infoSize, sizeofSiginfo)

	return p.libcSigaction(sig, act, old)
}

func libcSigprocmask(ctx uintptr, how, set, setSize, old, oldSize uint32) uint32 {
	p := proc(ctx)
	p.sized("sigprocmask", setSize, sizeofSigset, oldSize, sizeofSigset)

	return p.libcSigprocmask(how, set, old)
}

func libcPthread_sigmask(ctx uintptr, how, set, setSize, old, oldSize uint32) uint32 {
	p := proc(ctx)
	p.sized("pthread_sigmask", setSize, sizeofSigset, oldSize, sizeofSigset)

	return p.libcPthread_sigmask(how, set, old)
}

func libcSigfillset(ctx uintptr, set, setSize uint32) uint32 {
	p := proc(ctx)
	p.sized("sigfillset", setSize, sizeofSigset)

	return p.libcSigfillset(set)
}

func libcSigemptyset(ctx uintptr, set, setSize uint32) uint32 {
	p := proc(ctx)
	p.sized("sigemptyset", setSize, sizeofSigset)

	return p.libcSigemptyset(set)
}

func libcSigaddset(ctx uintptr, set, setSize, sig uint32) uint32 {
	p := proc(ctx)
	p.sized("sigaddset", setSize, sizeofSigset)

	return p.libcSigaddset(set, sig)
}

func libcSigdelset(ctx uintptr, set, setSize, sig uint32) uint32 {
	p := proc(ctx)
	p.sized("sigdelset", setSize, sizeofSigset)

	return p.libcSigdelset(set, sig)
}

func libcSigismember(ctx uintptr, set, setSize, sig uint32) uint32 {
	p := proc(ctx)
	p.sized("sigismember", setSize, sizeofSigset)

	return p.libcSigismember(set, sig)
}

func libcSigisemptyset(ctx uintptr, set, setSize uint32) uint32 {
	p := proc(ctx)
	p.sized("sigisemptyset", setSize, sizeofSigset)

	return p.libcSigisemptyset(set)
}

func libcSigorset(ctx uintptr, d, dSize, a, aSize, b, bSize uint32) uint32 {
	p := proc(ctx)
	p.sized("sigorset", dSize, sizeofSigset, aSize, sizeofSigset, bSize, sizeofSigset)

	return p.libcSigorset(d, a, b)
}

func libcSigandset(ctx uintptr, d, dSize, a, aSize, b, bSize uint32) uint32 {
	p := proc(ctx)
	p.sized("sigandset", dSize, sizeofSigset, aSize, sizeofSigset, bSize, sizeofSigset)

	return p.libcSigandset(d, a, b)
}

func libcSigpending(ctx uintptr, set, setSize uint32) uint32 {
	p := proc(ctx)
	p.sized("sigpending", setSize, sizeofSigset)

	return p.libcSigpending(set)
}

func libcSigwait(ctx uintptr, set, setSize, at uint32) uint32 {
	p := proc(ctx)
	p.sized("sigwait", setSize, sizeofSigset)

	return p.libcSigwait(set, at)
}

func libcSigwaitinfo(ctx uintptr, set, setSize, info, infoSize uint32) uint32 {
	p := proc(ctx)
	p.sized("sigwaitinfo", setSize, sizeofSigset, infoSize, sizeofSiginfo)

	return p.libcSigwaitinfo(set, info)
}

func libcSigtimedwait(ctx uintptr, set, setSize, info, infoSize, timeout, timeoutSize uint32) uint32 {
	p := proc(ctx)
	p.sized("sigtimedwait", setSize, sizeofSigset, infoSize, sizeofSiginfo, timeoutSize, sizeofTimespec)

	return p.libcSigtimedwait(set, info, timeout)
}

func libcSigaltstack(ctx uintptr, ss, ssSize, old, oldSize uint32) uint32 {
	p := proc(ctx)
	p.sized("sigaltstack", ssSize, sizeofStack, oldSize, sizeofStack)

	return p.libcSigaltstack(ss, old)
}

func libcSiginterrupt(ctx uintptr, sig, flag uint32) uint32 {
	return proc(ctx).libcSiginterrupt(sig, flag)
}

func libcPause(ctx uintptr) uint32 {
	return proc(ctx).libcPause()
}

func libcSigsuspend(ctx uintptr, set, setSize uint32) uint32 {
	p := proc(ctx)
	p.sized("sigsuspend", setSize, sizeofSigset)

	return p.libcSigsuspend(set)
}

func libcRaise(ctx uintptr, sig uint32) uint32 {
	return proc(ctx).libcRaise(sig)
}

func libcKill(ctx uintptr, pid, sig uint32) uint32 {
	return proc(ctx).libcKill(pid, sig)
}

func libcSigqueue(ctx uintptr, pid, sig, value uint32) uint32 {
	return proc(ctx).libcSigqueue(pid, sig, value)
}

func libcKillpg(ctx uintptr, pgrp, sig uint32) uint32 {
	return proc(ctx).libcKillpg(pgrp, sig)
}

func libcAbort(ctx uintptr) {
	proc(ctx).libcAbort()
}

func libcGetpid(ctx uintptr) uint32 {
	return proc(ctx).libcGetpid()
}

func libcGetppid(ctx uintptr) uint32 {
	return proc(ctx).libcGetppid()
}

func libcGetpgrp(ctx uintptr) uint32 {
	return proc(ctx).libcGetpgrp()
}

func libcGetuid(ctx uintptr) uint32 {
	return proc(ctx).libcGetuid()
}

func libcGeteuid(ctx uintptr) uint32 {
	return proc(ctx).libcGeteuid()
}

func libcGetgid(ctx uintptr) uint32 {
	return proc(ctx).libcGetgid()
}

func libcGetegid(ctx uintptr) uint32 {
	return proc(ctx).libcGetegid()
}

func libcGetgroups(ctx uintptr, n, list uint32) uint32 {
	return proc(ctx).libcGetgroups(n, list)
}

func libcUname(ctx uintptr, at, atSize uint32) uint32 {
	p := proc(ctx)
	p.sized("uname", atSize, sizeofUtsname)

	return p.libcUname(at)
}

func libcSysconf(ctx uintptr, name uint32) uint32 {
	return proc(ctx).libcSysconf(name)
}

func libcUmask(ctx uintptr, mask uint32) uint32 {
	return proc(ctx).libcUmask(mask)
}

func libcRand(ctx uintptr) uint32 {
	return proc(ctx).libcRand()
}

func libcSrand(ctx uintptr, seed uint32) {
	proc(ctx).libcSrand(seed)
}

func libcGetenv(ctx uintptr, name uint32) uint32 {
	return proc(ctx).libcGetenv(name)
}

func libcPutenv(ctx uintptr, s uint32) uint32 {
	return proc(ctx).libcPutenv(s)
}

func libcSetenv(ctx uintptr, at, value, overwrite uint32) uint32 {
	return proc(ctx).libcSetenv(at, value, overwrite)
}

func libcUnsetenv(ctx uintptr, at uint32) uint32 {
	return proc(ctx).libcUnsetenv(at)
}

func libcClearenv(ctx uintptr) uint32 {
	return proc(ctx).libcClearenv()
}

func libcExecve(ctx uintptr, path, argv, envp uint32) uint32 {
	return proc(ctx).libcExecve(path, argv, envp)
}

func libcExecvp(ctx uintptr, file, argv uint32) uint32 {
	return proc(ctx).libcExecvp(file, argv)
}

func libcExecv(ctx uintptr, path, argv uint32) uint32 {
	return proc(ctx).libcExecv(path, argv)
}

func libcExecvpe(ctx uintptr, file, argv, envp uint32) uint32 {
	return proc(ctx).libcExecvpe(file, argv, envp)
}

func libcExecle(ctx uintptr, path, arg0, va uint32) uint32 {
	return proc(ctx).libcExecle(path, arg0, va)
}

func libcFexecve(ctx uintptr, fd, argv, envp uint32) uint32 {
	return proc(ctx).libcFexecve(fd, argv, envp)
}

func libcExecl(ctx uintptr, path, arg0, va uint32) uint32 {
	return proc(ctx).libcExecl(path, arg0, va)
}

func libcExeclp(ctx uintptr, file, arg0, va uint32) uint32 {
	return proc(ctx).libcExeclp(file, arg0, va)
}

func libc_exit(ctx uintptr, code uint32) {
	proc(ctx).libc_exit(code)
}

func libc_Exit(ctx uintptr, code uint32) {
	proc(ctx).libc_Exit(code)
}

func libcSystem(ctx uintptr, cmd uint32) uint32 {
	return proc(ctx).libcSystem(cmd)
}

func libcSetpgid(ctx uintptr, pid, pgid uint32) uint32 {
	return proc(ctx).libcSetpgid(pid, pgid)
}

func libcTcgetpgrp(ctx uintptr, fd uint32) uint32 {
	return proc(ctx).libcTcgetpgrp(fd)
}

func libcTcsetpgrp(ctx uintptr, fd, pgrp uint32) uint32 {
	return proc(ctx).libcTcsetpgrp(fd, pgrp)
}

func libcSetjmp(ctx uintptr, buf, bufSize uint32) uint32 {
	p := proc(ctx)
	p.sized("setjmp", bufSize, sizeofJmpBuf)

	return p.libcSetjmp(buf)
}

func libcSigsetjmp(ctx uintptr, buf, bufSize, saveMask uint32) uint32 {
	p := proc(ctx)
	p.sized("sigsetjmp", bufSize, sizeofJmpBuf)

	return p.libcSigsetjmp(buf, saveMask)
}

func libcLongjmp(ctx uintptr, buf, bufSize, val uint32) {
	p := proc(ctx)
	p.sized("longjmp", bufSize, sizeofJmpBuf)

	p.libcLongjmp(buf, val)
}

func libcVfork(ctx uintptr) uint32 {
	return proc(ctx).libcVfork()
}

func libcFork(ctx uintptr) uint32 {
	return proc(ctx).libcFork()
}

func libcWaitpid(ctx uintptr, pid, status, options uint32) uint32 {
	return proc(ctx).libcWaitpid(pid, status, options)
}

func libcWait(ctx uintptr, status uint32) uint32 {
	return proc(ctx).libcWait(status)
}

func libcWait3(ctx uintptr, status, options, usage, usageSize uint32) uint32 {
	p := proc(ctx)
	p.sized("wait3", usageSize, sizeofRusage)

	return p.libcWait3(status, options, usage)
}

func libcWait4(ctx uintptr, pid, status, options, usage, usageSize uint32) uint32 {
	p := proc(ctx)
	p.sized("wait4", usageSize, sizeofRusage)

	return p.libcWait4(pid, status, options, usage)
}

func libcWaitid(ctx uintptr, idtype, id, info, infoSize, options uint32) uint32 {
	p := proc(ctx)
	p.sized("waitid", infoSize, sizeofSiginfo)

	return p.libcWaitid(idtype, id, info, options)
}

func libcSetlocale(ctx uintptr, cat, name uint32) uint32 {
	return proc(ctx).libcSetlocale(cat, name)
}

func libcLocaleconv(ctx uintptr, size uint32) uint32 {
	p := proc(ctx)
	p.sized("localeconv", size, sizeofLconv)

	return p.libcLocaleconv()
}

func libcNl_langinfo(ctx uintptr, item uint32) uint32 {
	return proc(ctx).libcNl_langinfo(item)
}

func libcIsascii(ctx uintptr, c uint32) uint32 {
	return proc(ctx).libcIsascii(c)
}

func libcToascii(ctx uintptr, c uint32) uint32 {
	return proc(ctx).libcToascii(c)
}

func libcTowlower(ctx uintptr, c uint32) uint32 {
	return proc(ctx).libcTowlower(c)
}

func libcTowupper(ctx uintptr, c uint32) uint32 {
	return proc(ctx).libcTowupper(c)
}

func libcWctype(ctx uintptr, name uint32) uint32 {
	return proc(ctx).libcWctype(name)
}

func libcIswctype(ctx uintptr, c, class uint32) uint32 {
	return proc(ctx).libcIswctype(c, class)
}

func libcWctrans(ctx uintptr, name uint32) uint32 {
	return proc(ctx).libcWctrans(name)
}

func libcTowctrans(ctx uintptr, c, trans uint32) uint32 {
	return proc(ctx).libcTowctrans(c, trans)
}

func libcIsalnum(ctx uintptr, c uint32) uint32 {
	return proc(ctx).libcIsalnum(c)
}

func libcIsalpha(ctx uintptr, c uint32) uint32 {
	return proc(ctx).libcIsalpha(c)
}

func libcIsblank(ctx uintptr, c uint32) uint32 {
	return proc(ctx).libcIsblank(c)
}

func libcIscntrl(ctx uintptr, c uint32) uint32 {
	return proc(ctx).libcIscntrl(c)
}

func libcIsgraph(ctx uintptr, c uint32) uint32 {
	return proc(ctx).libcIsgraph(c)
}

func libcIslower(ctx uintptr, c uint32) uint32 {
	return proc(ctx).libcIslower(c)
}

func libcIsprint(ctx uintptr, c uint32) uint32 {
	return proc(ctx).libcIsprint(c)
}

func libcIspunct(ctx uintptr, c uint32) uint32 {
	return proc(ctx).libcIspunct(c)
}

func libcIsupper(ctx uintptr, c uint32) uint32 {
	return proc(ctx).libcIsupper(c)
}

func libcIsxdigit(ctx uintptr, c uint32) uint32 {
	return proc(ctx).libcIsxdigit(c)
}

func libcIswalnum(ctx uintptr, c uint32) uint32 {
	return proc(ctx).libcIswalnum(c)
}

func libcIswalpha(ctx uintptr, c uint32) uint32 {
	return proc(ctx).libcIswalpha(c)
}

func libcIswblank(ctx uintptr, c uint32) uint32 {
	return proc(ctx).libcIswblank(c)
}

func libcIswcntrl(ctx uintptr, c uint32) uint32 {
	return proc(ctx).libcIswcntrl(c)
}

func libcIswdigit(ctx uintptr, c uint32) uint32 {
	return proc(ctx).libcIswdigit(c)
}

func libcIswgraph(ctx uintptr, c uint32) uint32 {
	return proc(ctx).libcIswgraph(c)
}

func libcIswlower(ctx uintptr, c uint32) uint32 {
	return proc(ctx).libcIswlower(c)
}

func libcIswprint(ctx uintptr, c uint32) uint32 {
	return proc(ctx).libcIswprint(c)
}

func libcIswpunct(ctx uintptr, c uint32) uint32 {
	return proc(ctx).libcIswpunct(c)
}

func libcIswspace(ctx uintptr, c uint32) uint32 {
	return proc(ctx).libcIswspace(c)
}

func libcIswupper(ctx uintptr, c uint32) uint32 {
	return proc(ctx).libcIswupper(c)
}

func libcIswxdigit(ctx uintptr, c uint32) uint32 {
	return proc(ctx).libcIswxdigit(c)
}

func libcWcslen(ctx uintptr, s uint32) uint32 {
	return proc(ctx).libcWcslen(s)
}

func libcWcsnlen(ctx uintptr, s, n uint32) uint32 {
	return proc(ctx).libcWcsnlen(s, n)
}

func libcWcscpy(ctx uintptr, d, s uint32) uint32 {
	return proc(ctx).libcWcscpy(d, s)
}

func libcWcsncpy(ctx uintptr, d, s, n uint32) uint32 {
	return proc(ctx).libcWcsncpy(d, s, n)
}

func libcWcscat(ctx uintptr, d, s uint32) uint32 {
	return proc(ctx).libcWcscat(d, s)
}

func libcWcscmp(ctx uintptr, a, b uint32) uint32 {
	return proc(ctx).libcWcscmp(a, b)
}

func libcWcsncmp(ctx uintptr, a, b, n uint32) uint32 {
	return proc(ctx).libcWcsncmp(a, b, n)
}

func libcWcscoll(ctx uintptr, a, b uint32) uint32 {
	return proc(ctx).libcWcscoll(a, b)
}

func libcWcsxfrm(ctx uintptr, d, s, n uint32) uint32 {
	return proc(ctx).libcWcsxfrm(d, s, n)
}

func libcWcschr(ctx uintptr, s, c uint32) uint32 {
	return proc(ctx).libcWcschr(s, c)
}

func libcWcsrchr(ctx uintptr, s, c uint32) uint32 {
	return proc(ctx).libcWcsrchr(s, c)
}

func libcWcsstr(ctx uintptr, h, n uint32) uint32 {
	return proc(ctx).libcWcsstr(h, n)
}

func libcWcspbrk(ctx uintptr, s, set uint32) uint32 {
	return proc(ctx).libcWcspbrk(s, set)
}

func libcWcsspn(ctx uintptr, s, set uint32) uint32 {
	return proc(ctx).libcWcsspn(s, set)
}

func libcWcscspn(ctx uintptr, s, set uint32) uint32 {
	return proc(ctx).libcWcscspn(s, set)
}

func libcWcstok(ctx uintptr, s, sep, save uint32) uint32 {
	return proc(ctx).libcWcstok(s, sep, save)
}

func libcWcsdup(ctx uintptr, s uint32) uint32 {
	return proc(ctx).libcWcsdup(s)
}

func libcWcsftime(ctx uintptr, s, max, format, tm, tmSize uint32) uint32 {
	p := proc(ctx)
	p.sized("wcsftime", tmSize, sizeofTm)

	return p.libcWcsftime(s, max, format, tm)
}

func libcWmemchr(ctx uintptr, s, c, n uint32) uint32 {
	return proc(ctx).libcWmemchr(s, c, n)
}

func libcWmemcmp(ctx uintptr, a, b, n uint32) uint32 {
	return proc(ctx).libcWmemcmp(a, b, n)
}

func libcWmemcpy(ctx uintptr, d, s, n uint32) uint32 {
	return proc(ctx).libcWmemcpy(d, s, n)
}

func libcWmemmove(ctx uintptr, d, s, n uint32) uint32 {
	return proc(ctx).libcWmemmove(d, s, n)
}

func libcWmemset(ctx uintptr, d, c, n uint32) uint32 {
	return proc(ctx).libcWmemset(d, c, n)
}

func libcWcwidth(ctx uintptr, c uint32) uint32 {
	return proc(ctx).libcWcwidth(c)
}

func libcWcswidth(ctx uintptr, s, n uint32) uint32 {
	return proc(ctx).libcWcswidth(s, n)
}

func libcSetuid(ctx uintptr, uid uint32) uint32 {
	return proc(ctx).libcSetuid(uid)
}

func libcSeteuid(ctx uintptr, uid uint32) uint32 {
	return proc(ctx).libcSeteuid(uid)
}

func libcSetreuid(ctx uintptr, r, e uint32) uint32 {
	return proc(ctx).libcSetreuid(r, e)
}

func libcSetresuid(ctx uintptr, r, e, s uint32) uint32 {
	return proc(ctx).libcSetresuid(r, e, s)
}

func libcSetgid(ctx uintptr, gid uint32) uint32 {
	return proc(ctx).libcSetgid(gid)
}

func libcSetegid(ctx uintptr, gid uint32) uint32 {
	return proc(ctx).libcSetegid(gid)
}

func libcSetregid(ctx uintptr, r, e uint32) uint32 {
	return proc(ctx).libcSetregid(r, e)
}

func libcSetresgid(ctx uintptr, r, e, s uint32) uint32 {
	return proc(ctx).libcSetresgid(r, e, s)
}

func libcGetresuid(ctx uintptr, r, e, s uint32) uint32 {
	return proc(ctx).libcGetresuid(r, e, s)
}

func libcGetresgid(ctx uintptr, r, e, s uint32) uint32 {
	return proc(ctx).libcGetresgid(r, e, s)
}

func libcSetgroups(ctx uintptr, n, list uint32) uint32 {
	return proc(ctx).libcSetgroups(n, list)
}

func libcInitgroups(ctx uintptr, user, gid uint32) uint32 {
	return proc(ctx).libcInitgroups(user, gid)
}

func libcSetsid(ctx uintptr) uint32 {
	return proc(ctx).libcSetsid()
}

func libcGetsid(ctx uintptr, pid uint32) uint32 {
	return proc(ctx).libcGetsid(pid)
}

func libcGetpgid(ctx uintptr, pid uint32) uint32 {
	return proc(ctx).libcGetpgid(pid)
}

func libcSetpgrp(ctx uintptr) uint32 {
	return proc(ctx).libcSetpgrp()
}

func libcNice(ctx uintptr, inc uint32) uint32 {
	return proc(ctx).libcNice(inc)
}

func libcGetpriority(ctx uintptr, which, who uint32) uint32 {
	return proc(ctx).libcGetpriority(which, who)
}

func libcSetpriority(ctx uintptr, which, who, prio uint32) uint32 {
	return proc(ctx).libcSetpriority(which, who, prio)
}

func libcSched_yield(ctx uintptr) uint32 {
	return proc(ctx).libcSched_yield()
}

func libcSched_get_priority_max(ctx uintptr, policy uint32) uint32 {
	return proc(ctx).libcSched_get_priority_max(policy)
}

func libcSched_get_priority_min(ctx uintptr, policy uint32) uint32 {
	return proc(ctx).libcSched_get_priority_min(policy)
}

func libcSched_getscheduler(ctx uintptr, pid uint32) uint32 {
	return proc(ctx).libcSched_getscheduler(pid)
}

func libcSched_getparam(ctx uintptr, pid, param, paramSize uint32) uint32 {
	p := proc(ctx)
	p.sized("sched_getparam", paramSize, sizeofSchedParam)

	return p.libcSched_getparam(pid, param)
}

func libcSched_setparam(ctx uintptr, pid, param, paramSize uint32) uint32 {
	p := proc(ctx)
	p.sized("sched_setparam", paramSize, sizeofSchedParam)

	return p.libcSched_setparam(pid, param)
}

func libcSched_setscheduler(ctx uintptr, pid, policy, param, paramSize uint32) uint32 {
	p := proc(ctx)
	p.sized("sched_setscheduler", paramSize, sizeofSchedParam)

	return p.libcSched_setscheduler(pid, policy, param)
}

func libcSched_rr_get_interval(ctx uintptr, pid, ts, tsSize uint32) uint32 {
	p := proc(ctx)
	p.sized("sched_rr_get_interval", tsSize, sizeofTimespec)

	return p.libcSched_rr_get_interval(pid, ts)
}

func libcOpenlog(ctx uintptr, ident, option, facility uint32) {
	proc(ctx).libcOpenlog(ident, option, facility)
}

func libcCloselog(ctx uintptr) {
	proc(ctx).libcCloselog()
}

func libcSetlogmask(ctx uintptr, mask uint32) uint32 {
	return proc(ctx).libcSetlogmask(mask)
}

func libc__kernel_syslog(ctx uintptr, priority, message uint32) {
	proc(ctx).libc__kernel_syslog(priority, message)
}

func libcSysinfo(ctx uintptr, at, atSize uint32) uint32 {
	p := proc(ctx)
	p.sized("sysinfo", atSize, sizeofSysinfo)

	return p.libcSysinfo(at)
}

func libcGet_nprocs(ctx uintptr) uint32 {
	return proc(ctx).libcGet_nprocs()
}

func libcGet_nprocs_conf(ctx uintptr) uint32 {
	return proc(ctx).libcGet_nprocs_conf()
}

func libcGet_phys_pages(ctx uintptr) uint32 {
	return proc(ctx).libcGet_phys_pages()
}

func libcGet_avphys_pages(ctx uintptr) uint32 {
	return proc(ctx).libcGet_avphys_pages()
}

func libcGetloadavg(ctx uintptr, at, n uint32) uint32 {
	return proc(ctx).libcGetloadavg(at, n)
}

func libcGetrusage(ctx uintptr, who, at, atSize uint32) uint32 {
	p := proc(ctx)
	p.sized("getrusage", atSize, sizeofRusage)

	return p.libcGetrusage(who, at)
}

func libcGetrlimit(ctx uintptr, res, at, atSize uint32) uint32 {
	p := proc(ctx)
	p.sized("getrlimit", atSize, sizeofRlimit)

	return p.libcGetrlimit(res, at)
}

func libcSetrlimit(ctx uintptr, res, at, atSize uint32) uint32 {
	p := proc(ctx)
	p.sized("setrlimit", atSize, sizeofRlimit)

	return p.libcSetrlimit(res, at)
}

func libcPrlimit(ctx uintptr, pid, res, set, setSize, old, oldSize uint32) uint32 {
	p := proc(ctx)
	p.sized("prlimit", setSize, sizeofRlimit, oldSize, sizeofRlimit)

	return p.libcPrlimit(pid, res, set, old)
}

func libcGetdtablesize(ctx uintptr) uint32 {
	return proc(ctx).libcGetdtablesize()
}

func libcConfstr(ctx uintptr, name, buf, n uint32) uint32 {
	return proc(ctx).libcConfstr(name, buf, n)
}

func libcPathconf(ctx uintptr, path, name uint32) uint32 {
	return proc(ctx).libcPathconf(path, name)
}

func libcFpathconf(ctx uintptr, fd, name uint32) uint32 {
	return proc(ctx).libcFpathconf(fd, name)
}

func libcGetlogin(ctx uintptr) uint32 {
	return proc(ctx).libcGetlogin()
}

func libcGetlogin_r(ctx uintptr, buf, n uint32) uint32 {
	return proc(ctx).libcGetlogin_r(buf, n)
}

func libcCtermid(ctx uintptr, s uint32) uint32 {
	return proc(ctx).libcCtermid(s)
}

func libcTtyname(ctx uintptr, fd uint32) uint32 {
	return proc(ctx).libcTtyname(fd)
}

func libcTtyname_r(ctx uintptr, fd, buf, n uint32) uint32 {
	return proc(ctx).libcTtyname_r(fd, buf, n)
}

func libcGetpwnam(ctx uintptr, name, size uint32) uint32 {
	p := proc(ctx)
	p.sized("getpwnam", size, sizeofPasswd)

	return p.libcGetpwnam(name)
}

func libcGetpwuid(ctx uintptr, uid, size uint32) uint32 {
	p := proc(ctx)
	p.sized("getpwuid", size, sizeofPasswd)

	return p.libcGetpwuid(uid)
}

func libcGetpwnam_r(ctx uintptr, name, pwd, pwdSize, buf, size, result, resultSize uint32) uint32 {
	p := proc(ctx)
	p.sized("getpwnam_r", pwdSize, sizeofPasswd, resultSize, sizeofPasswd)

	return p.libcGetpwnam_r(name, pwd, buf, size, result)
}

func libcGetpwuid_r(ctx uintptr, uid, pwd, pwdSize, buf, size, result, resultSize uint32) uint32 {
	p := proc(ctx)
	p.sized("getpwuid_r", pwdSize, sizeofPasswd, resultSize, sizeofPasswd)

	return p.libcGetpwuid_r(uid, pwd, buf, size, result)
}

func libcGetpwent(ctx uintptr, size uint32) uint32 {
	p := proc(ctx)
	p.sized("getpwent", size, sizeofPasswd)

	return p.libcGetpwent()
}

func libcSetpwent(ctx uintptr) {
	proc(ctx).libcSetpwent()
}

func libcEndpwent(ctx uintptr) {
	proc(ctx).libcEndpwent()
}

func libcGetgrnam(ctx uintptr, name, size uint32) uint32 {
	p := proc(ctx)
	p.sized("getgrnam", size, sizeofGroup)

	return p.libcGetgrnam(name)
}

func libcGetgrgid(ctx uintptr, gid, size uint32) uint32 {
	p := proc(ctx)
	p.sized("getgrgid", size, sizeofGroup)

	return p.libcGetgrgid(gid)
}

func libcGetgrnam_r(ctx uintptr, name, grp, grpSize, buf, size, result, resultSize uint32) uint32 {
	p := proc(ctx)
	p.sized("getgrnam_r", grpSize, sizeofGroup, resultSize, sizeofGroup)

	return p.libcGetgrnam_r(name, grp, buf, size, result)
}

func libcGetgrgid_r(ctx uintptr, gid, grp, grpSize, buf, size, result, resultSize uint32) uint32 {
	p := proc(ctx)
	p.sized("getgrgid_r", grpSize, sizeofGroup, resultSize, sizeofGroup)

	return p.libcGetgrgid_r(gid, grp, buf, size, result)
}

func libcGetgrent(ctx uintptr, size uint32) uint32 {
	p := proc(ctx)
	p.sized("getgrent", size, sizeofGroup)

	return p.libcGetgrent()
}

func libcSetgrent(ctx uintptr) {
	proc(ctx).libcSetgrent()
}

func libcEndgrent(ctx uintptr) {
	proc(ctx).libcEndgrent()
}

func libcGetgrouplist(ctx uintptr, user, group, groups, count uint32) uint32 {
	return proc(ctx).libcGetgrouplist(user, group, groups, count)
}

func libcPosix_openpt(ctx uintptr, flags uint32) uint32 {
	return proc(ctx).libcPosix_openpt(flags)
}

func libcGrantpt(ctx uintptr, fd uint32) uint32 {
	return proc(ctx).libcGrantpt(fd)
}

func libcUnlockpt(ctx uintptr, fd uint32) uint32 {
	return proc(ctx).libcUnlockpt(fd)
}

func libcPtsname(ctx uintptr, fd uint32) uint32 {
	return proc(ctx).libcPtsname(fd)
}

func libcPtsname_r(ctx uintptr, fd, buf, size uint32) uint32 {
	return proc(ctx).libcPtsname_r(fd, buf, size)
}

func libcOpenpty(ctx uintptr, amaster, aslave, name, termp, termpSize, winp, winpSize uint32) uint32 {
	p := proc(ctx)
	p.sized("openpty", termpSize, sizeofTermios, winpSize, sizeofWinsize)

	return p.libcOpenpty(amaster, aslave, name, termp, winp)
}

func libcForkpty(ctx uintptr, amaster, name, termp, termpSize, winp, winpSize uint32) uint32 {
	p := proc(ctx)
	p.sized("forkpty", termpSize, sizeofTermios, winpSize, sizeofWinsize)

	return p.libcForkpty(amaster, name, termp, winp)
}

func libcGlob(ctx uintptr, pattern, flags, errfunc, out, outSize uint32) uint32 {
	p := proc(ctx)
	p.sized("glob", outSize, sizeofGlob)

	return p.libcGlob(pattern, flags, errfunc, out)
}

func libcGlobfree(ctx uintptr, out, outSize uint32) {
	p := proc(ctx)
	p.sized("globfree", outSize, sizeofGlob)

	p.libcGlobfree(out)
}

func libcRandom(ctx uintptr) uint32 {
	return proc(ctx).libcRandom()
}

func libcSrandom(ctx uintptr, seed uint32) {
	proc(ctx).libcSrandom(seed)
}

func libcInitstate(ctx uintptr, seed, state, size uint32) uint32 {
	return proc(ctx).libcInitstate(seed, state, size)
}

func libcSetstate(ctx uintptr, state uint32) uint32 {
	return proc(ctx).libcSetstate(state)
}

func libcRand_r(ctx uintptr, seed uint32) uint32 {
	return proc(ctx).libcRand_r(seed)
}

func libcSecure_getenv(ctx uintptr, name uint32) uint32 {
	return proc(ctx).libcSecure_getenv(name)
}

func libcGetsubopt(ctx uintptr, opt, keys, val uint32) uint32 {
	return proc(ctx).libcGetsubopt(opt, keys, val)
}

func libcDaemon(ctx uintptr, nochdir, noclose uint32) uint32 {
	return proc(ctx).libcDaemon(nochdir, noclose)
}

func libcPrctl(ctx uintptr, option, va uint32) uint32 {
	return proc(ctx).libcPrctl(option, va)
}

func libcGetpass(ctx uintptr, prompt uint32) uint32 {
	return proc(ctx).libcGetpass(prompt)
}

func libcVhangup(ctx uintptr) uint32 {
	return proc(ctx).libcVhangup()
}

func libcGetusershell(ctx uintptr) uint32 {
	return proc(ctx).libcGetusershell()
}

func libcSetusershell(ctx uintptr) {
	proc(ctx).libcSetusershell()
}

func libcEndusershell(ctx uintptr) {
	proc(ctx).libcEndusershell()
}

func libcGethostname(ctx uintptr, name, n uint32) uint32 {
	return proc(ctx).libcGethostname(name, n)
}

func libcGetentropy(ctx uintptr, buf, n uint32) uint32 {
	return proc(ctx).libcGetentropy(buf, n)
}

func libcGetdomainname(ctx uintptr, name, n uint32) uint32 {
	return proc(ctx).libcGetdomainname(name, n)
}

func libcSethostname(ctx uintptr, name, n uint32) uint32 {
	return proc(ctx).libcSethostname(name, n)
}

func libcSetdomainname(ctx uintptr, name, n uint32) uint32 {
	return proc(ctx).libcSetdomainname(name, n)
}

func libcGethostid(ctx uintptr) uint32 {
	return proc(ctx).libcGethostid()
}

func libcSethostid(ctx uintptr, id uint32) uint32 {
	return proc(ctx).libcSethostid(id)
}

func libcPsignal(ctx uintptr, sig, s uint32) {
	proc(ctx).libcPsignal(sig, s)
}

func libcPsiginfo(ctx uintptr, info, infoSize, s uint32) {
	p := proc(ctx)
	p.sized("psiginfo", infoSize, sizeofSiginfo)

	p.libcPsiginfo(info, s)
}

func libc__assert_fail(ctx uintptr, expr, file, line, function uint32) {
	proc(ctx).libc__assert_fail(expr, file, line, function)
}

func libcSocket(ctx uintptr, domain, kind, protocol uint32) uint32 {
	return proc(ctx).libcSocket(domain, kind, protocol)
}

func libcSocketpair(ctx uintptr, domain, kind, protocol, at uint32) uint32 {
	return proc(ctx).libcSocketpair(domain, kind, protocol, at)
}

func libcBind(ctx uintptr, fd, addr, n uint32) uint32 {
	return proc(ctx).libcBind(fd, addr, n)
}

func libcConnect(ctx uintptr, fd, addr, n uint32) uint32 {
	return proc(ctx).libcConnect(fd, addr, n)
}

func libcListen(ctx uintptr, fd, backlog uint32) uint32 {
	return proc(ctx).libcListen(fd, backlog)
}

func libcAccept(ctx uintptr, fd, addr, lenAt uint32) uint32 {
	return proc(ctx).libcAccept(fd, addr, lenAt)
}

func libcAccept4(ctx uintptr, fd, addr, lenAt, flags uint32) uint32 {
	return proc(ctx).libcAccept4(fd, addr, lenAt, flags)
}

func libcGetsockname(ctx uintptr, fd, addr, lenAt uint32) uint32 {
	return proc(ctx).libcGetsockname(fd, addr, lenAt)
}

func libcGetpeername(ctx uintptr, fd, addr, lenAt uint32) uint32 {
	return proc(ctx).libcGetpeername(fd, addr, lenAt)
}

func libcGetsockopt(ctx uintptr, fd, level, name, at, lenAt uint32) uint32 {
	return proc(ctx).libcGetsockopt(fd, level, name, at, lenAt)
}

func libcSetsockopt(ctx uintptr, fd, level, name, at, n uint32) uint32 {
	return proc(ctx).libcSetsockopt(fd, level, name, at, n)
}

func libcSend(ctx uintptr, fd, buf, n, flags uint32) uint32 {
	return proc(ctx).libcSend(fd, buf, n, flags)
}

func libcSendto(ctx uintptr, fd, buf, n, flags, addr, alen uint32) uint32 {
	return proc(ctx).libcSendto(fd, buf, n, flags, addr, alen)
}

func libcRecv(ctx uintptr, fd, buf, n, flags uint32) uint32 {
	return proc(ctx).libcRecv(fd, buf, n, flags)
}

func libcRecvfrom(ctx uintptr, fd, buf, n, flags, addr, lenAt uint32) uint32 {
	return proc(ctx).libcRecvfrom(fd, buf, n, flags, addr, lenAt)
}

func libcSendmsg(ctx uintptr, fd, msg, msgSize, iovSize, flags uint32) uint32 {
	p := proc(ctx)
	p.sized("sendmsg", msgSize, sizeofMsghdr, iovSize, sizeofIovec)

	return p.libcSendmsg(fd, msg, flags)
}

func libcRecvmsg(ctx uintptr, fd, msg, msgSize, iovSize, flags uint32) uint32 {
	p := proc(ctx)
	p.sized("recvmsg", msgSize, sizeofMsghdr, iovSize, sizeofIovec)

	return p.libcRecvmsg(fd, msg, flags)
}

func libcShutdown(ctx uintptr, fd, how uint32) uint32 {
	return proc(ctx).libcShutdown(fd, how)
}

func libcSockatmark(ctx uintptr, fd uint32) uint32 {
	return proc(ctx).libcSockatmark(fd)
}

func libcGetaddrinfo(ctx uintptr, node, service, hints, hintsSize, res, resSize uint32) uint32 {
	p := proc(ctx)
	p.sized("getaddrinfo", hintsSize, sizeofAddrinfo, resSize, sizeofAddrinfo)

	return p.libcGetaddrinfo(node, service, hints, res)
}

func libcFreeaddrinfo(ctx uintptr, res, resSize uint32) {
	p := proc(ctx)
	p.sized("freeaddrinfo", resSize, sizeofAddrinfo)

	p.libcFreeaddrinfo(res)
}

func libcGai_strerror(ctx uintptr, code uint32) uint32 {
	return proc(ctx).libcGai_strerror(code)
}

func libcGetnameinfo(ctx uintptr, sa, salen, host, hostlen, serv, servlen, flags uint32) uint32 {
	return proc(ctx).libcGetnameinfo(sa, salen, host, hostlen, serv, servlen, flags)
}

func libcGethostbyname(ctx uintptr, name, size uint32) uint32 {
	p := proc(ctx)
	p.sized("gethostbyname", size, sizeofHostent)

	return p.libcGethostbyname(name)
}

func libcGethostbyname2(ctx uintptr, name, family, size uint32) uint32 {
	p := proc(ctx)
	p.sized("gethostbyname2", size, sizeofHostent)

	return p.libcGethostbyname2(name, family)
}

func libcGethostbyaddr(ctx uintptr, addr, n, family, size uint32) uint32 {
	p := proc(ctx)
	p.sized("gethostbyaddr", size, sizeofHostent)

	return p.libcGethostbyaddr(addr, n, family)
}

func libcGethostent(ctx uintptr, size uint32) uint32 {
	p := proc(ctx)
	p.sized("gethostent", size, sizeofHostent)

	return p.libcGethostent()
}

func libcSethostent(ctx uintptr, stay uint32) {
	proc(ctx).libcSethostent(stay)
}

func libcEndhostent(ctx uintptr) {
	proc(ctx).libcEndhostent()
}

func libcGetservbyname(ctx uintptr, name, proto, size uint32) uint32 {
	p := proc(ctx)
	p.sized("getservbyname", size, sizeofServent)

	return p.libcGetservbyname(name, proto)
}

func libcGetservbyport(ctx uintptr, port, proto, size uint32) uint32 {
	p := proc(ctx)
	p.sized("getservbyport", size, sizeofServent)

	return p.libcGetservbyport(port, proto)
}

func libcGetprotobyname(ctx uintptr, name, size uint32) uint32 {
	p := proc(ctx)
	p.sized("getprotobyname", size, sizeofProtoent)

	return p.libcGetprotobyname(name)
}

func libcGetprotobynumber(ctx uintptr, number, size uint32) uint32 {
	p := proc(ctx)
	p.sized("getprotobynumber", size, sizeofProtoent)

	return p.libcGetprotobynumber(number)
}

func libcGetnetbyname(ctx uintptr, name, size uint32) uint32 {
	p := proc(ctx)
	p.sized("getnetbyname", size, sizeofNetent)

	return p.libcGetnetbyname(name)
}

func libcGetnetbyaddr(ctx uintptr, net, family, size uint32) uint32 {
	p := proc(ctx)
	p.sized("getnetbyaddr", size, sizeofNetent)

	return p.libcGetnetbyaddr(net, family)
}

func libcHstrerror(ctx uintptr, code uint32) uint32 {
	return proc(ctx).libcHstrerror(code)
}

func libcHerror(ctx uintptr, s uint32) {
	proc(ctx).libcHerror(s)
}

func libcInet_aton(ctx uintptr, s, at, atSize uint32) uint32 {
	p := proc(ctx)
	p.sized("inet_aton", atSize, sizeofInAddr)

	return p.libcInet_aton(s, at)
}

func libcInet_addr(ctx uintptr, s uint32) uint32 {
	return proc(ctx).libcInet_addr(s)
}

func libcInet_network(ctx uintptr, s uint32) uint32 {
	return proc(ctx).libcInet_network(s)
}

func libcInet_ntoa(ctx uintptr, in uint32) uint32 {
	return proc(ctx).libcInet_ntoa(in)
}

func libcInet_pton(ctx uintptr, family, s, dst uint32) uint32 {
	return proc(ctx).libcInet_pton(family, s, dst)
}

func libcInet_ntop(ctx uintptr, family, src, dst, size uint32) uint32 {
	return proc(ctx).libcInet_ntop(family, src, dst, size)
}

func libcInet_makeaddr(ctx uintptr, n, h uint32) uint32 {
	return proc(ctx).libcInet_makeaddr(n, h)
}

func libcInet_lnaof(ctx uintptr, in uint32) uint32 {
	return proc(ctx).libcInet_lnaof(in)
}

func libcInet_netof(ctx uintptr, in uint32) uint32 {
	return proc(ctx).libcInet_netof(in)
}

func libcIf_nametoindex(ctx uintptr, name uint32) uint32 {
	return proc(ctx).libcIf_nametoindex(name)
}

func libcIf_indextoname(ctx uintptr, index, buf uint32) uint32 {
	return proc(ctx).libcIf_indextoname(index, buf)
}

func libcIf_nameindex(ctx uintptr, size uint32) uint32 {
	p := proc(ctx)
	p.sized("if_nameindex", size, sizeofIfNameindex)

	return p.libcIf_nameindex()
}

func libcIf_freenameindex(ctx uintptr, block, blockSize uint32) {
	p := proc(ctx)
	p.sized("if_freenameindex", blockSize, sizeofIfNameindex)

	p.libcIf_freenameindex(block)
}

func libcCrypt(ctx uintptr, key, setting uint32) uint32 {
	return proc(ctx).libcCrypt(key, setting)
}

func libcEpoll_create(ctx uintptr, size uint32) uint32 {
	return proc(ctx).libcEpoll_create(size)
}

func libcEpoll_create1(ctx uintptr, flags uint32) uint32 {
	return proc(ctx).libcEpoll_create1(flags)
}

func libcEpoll_ctl(ctx uintptr, epfd, op, fd, at, atSize uint32) uint32 {
	p := proc(ctx)
	p.sized("epoll_ctl", atSize, sizeofEpollEvent)

	return p.libcEpoll_ctl(epfd, op, fd, at)
}

func libcEpoll_wait(ctx uintptr, epfd, at, atSize, max, timeout uint32) uint32 {
	p := proc(ctx)
	p.sized("epoll_wait", atSize, sizeofEpollEvent)

	return p.libcEpoll_wait(epfd, at, max, timeout)
}

func libcEpoll_pwait(ctx uintptr, epfd, at, atSize, max, timeout, mask, maskSize uint32) uint32 {
	p := proc(ctx)
	p.sized("epoll_pwait", atSize, sizeofEpollEvent, maskSize, sizeofSigset)

	return p.libcEpoll_pwait(epfd, at, max, timeout, mask)
}
