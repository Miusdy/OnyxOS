#ifndef MINIOS_MMAN_H
#define MINIOS_MMAN_H
#define PROT_READ     1
#define PROT_WRITE    2
#define MAP_PRIVATE   1
#define MAP_ANONYMOUS 2
#define MAP_FAILED    ((void *)-1)

// Preferred mmap arena; heap growth must not cross a live mapping.
#define MMAPBASE (1L << 30)
#define MMAPEND  (2L << 30)
#define MMAPMAX  (64L << 20)
#define NVMA     16
#endif
