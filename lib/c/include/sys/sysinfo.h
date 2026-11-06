#pragma once

#include <__base.h>

__BEGIN_DECLS

#define SI_LOAD_SHIFT 16

struct sysinfo {
    unsigned long uptime;
    unsigned long loads[3];
    unsigned long totalram;
    unsigned long freeram;
    unsigned long sharedram;
    unsigned long bufferram;
    unsigned long totalswap;
    unsigned long freeswap;
    unsigned short procs;
    unsigned short pad;
    unsigned long totalhigh;
    unsigned long freehigh;
    unsigned mem_unit;
    char __reserved[256];
};

int sysinfo(struct sysinfo*) __KERNEL(sysinfo);
int get_nprocs_conf(void) __KERNEL(get_nprocs_conf);
int get_nprocs(void) __KERNEL(get_nprocs);
long get_phys_pages(void) __KERNEL(get_phys_pages);
long get_avphys_pages(void) __KERNEL(get_avphys_pages);

__END_DECLS
