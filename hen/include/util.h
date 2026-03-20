#pragma once
#ifndef UTIL_H
#define UTIL_H

#include <sys/types.h>

typedef struct _cpuset cpuset_t;
extern "C" int cpuset_setaffinity(int level, int which, long id, size_t setsize, const cpuset_t *mask);

uint64_t get_dmap_addr(uint64_t pa);

void *find_proc_by_name(const char *name);
void *get_proc_vmmap(void *proc);

void memcpy(void *dest, const void *src, size_t n);
size_t strlen(const char *str);
char *strstr(const char *str, const char *substring);
int strncmp(const char * s1, const char * s2, size_t n);

int pin_to_core(int num);
void pin_to_first_available_core();

extern void *curthread;

#endif // UTIL_H