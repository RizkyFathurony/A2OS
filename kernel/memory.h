#pragma once
#include "scheduler.h"

constexpr u32 HEAP_SIZE = 32 * 1024;     // 32 KB

enum MemPolicy : int { MEM_FIRST_FIT = 0, MEM_BEST_FIT = 1 };
enum MemFreeResult : int { MEM_FREE_OK = 0, MEM_FREE_INVALID, MEM_FREE_DOUBLE };

struct MemStats {
    u32 total;          // ukuran heap
    u32 used_bytes;     // total muatan blok terpakai
    u32 free_bytes;     // total muatan blok bebas
    u32 used_blocks;
    u32 free_blocks;
    u32 largest_free;   // blok bebas terbesar (alokasi terbesar yang masih bisa dipenuhi)
    u32 overhead;       // byte yang dipakai header blok
};

void  mem_init();                         // panggil sekali saat boot
void* kmalloc(u32 size);                  // nullptr jika gagal
void* kcalloc(u32 count, u32 size);       // seperti kmalloc, lalu diisi nol
int   kfree(void* ptr);                   // -> MemFreeResult

void  mem_get_stats(MemStats* out);
bool  mem_block_info(int index, u32* offset, u32* size, bool* is_free);  // false jika index di luar daftar
u32   mem_offset(const void* ptr);        // posisi ptr dari awal heap

void  mem_set_policy(int policy);
int   mem_get_policy();