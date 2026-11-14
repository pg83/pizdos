#pragma once

#include <__base.h>

__BEGIN_DECLS

#define MNT_FORCE 1
#define MNT_DETACH 2
#define MNT_EXPIRE 4
#define UMOUNT_NOFOLLOW 8

/*
 * mount mounts source, a directory of a server, on target, a directory:
 * a walk into target, and a call on its name, go on to source's server
 * from then on. fstype, flags and data are not used. umount and umount2
 * unmount it; of umount2's flags only UMOUNT_NOFOLLOW is.
 */
int mount(const char*, const char*, const char*, unsigned long, const void*);
int umount(const char*);
int umount2(const char*, int);

__END_DECLS
