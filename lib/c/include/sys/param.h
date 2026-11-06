#pragma once

#include <endian.h>
#include <limits.h>

#define MAXSYMLINKS 20
#define MAXHOSTNAMELEN 64
#define MAXNAMLEN 255
#define MAXPATHLEN 4096
#define NOFILE 256
#define NOGROUP (-1)
#define NGROUPS 32
#define NBBY 8
#define HZ 100
#define DEV_BSIZE 512

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))

#define setbit(x, i) ((x)[(i) / NBBY] |= 1 << ((i) % NBBY))
#define clrbit(x, i) ((x)[(i) / NBBY] &= ~(1 << ((i) % NBBY)))
#define isset(x, i) ((x)[(i) / NBBY] & (1 << ((i) % NBBY)))
#define isclr(x, i) (!(((x)[(i) / NBBY]) & (1 << ((i) % NBBY))))

#define howmany(n, d) (((n) + ((d) - 1)) / (d))
#define roundup(n, d) (howmany(n, d) * (d))
#define powerof2(n) !(((n) - 1) & (n))
