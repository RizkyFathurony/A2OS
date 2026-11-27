#include "memory.h"

struct Block {
    u32    magic;     // penanda header valid
    u32    size;      // ukuran muatan (byte), kelipatan 8
    u32    is_free;   // 1 = bebas, 0 = terpakai
    Block* next;      // blok berikutnya (urut alamat), nullptr = blok terakhir
};

static const u32 HDR   = sizeof(Block);
static const u32 MAGIC = 0xA20A20A2;

alignas(16) static u8 heap[HEAP_SIZE];
static Block* head = nullptr;
static int    policy = MEM_FIRST_FIT;

// Heap dipakai banyak thread (penjadwal preemptive), jadi semua operasi dikunci.
static Mutex mem_lock = {0, -1};

static u32 align8(u32 n) { return (n + 7u) & ~7u; }

void mem_init() {
    head = reinterpret_cast<Block*>(heap);
    head->magic   = MAGIC;
    head->size    = HEAP_SIZE - HDR;
    head->is_free = 1;
    head->next    = nullptr;
}

void* kmalloc(u32 size) {
    if (size == 0 || size > HEAP_SIZE)
        return nullptr;
    size = align8(size);

    mutex_lock(&mem_lock);

    // Pilih blok: first-fit = yang pertama cukup, best-fit = yang paling pas.
    Block* chosen = nullptr;
    for (Block* b = head; b; b = b->next) {
        if (!b->is_free || b->size < size)
            continue;
        if (policy == MEM_FIRST_FIT) { chosen = b; break; }
        if (!chosen || b->size < chosen->size)
            chosen = b;
    }

    void* result = nullptr;
    if (chosen) {
        // Pecah blok jika sisanya cukup untuk satu header + muatan minimal 8 byte.
        if (chosen->size >= size + HDR + 8) {
            Block* rest = reinterpret_cast<Block*>(reinterpret_cast<u8*>(chosen + 1) + size);
            rest->magic   = MAGIC;
            rest->size    = chosen->size - size - HDR;
            rest->is_free = 1;
            rest->next    = chosen->next;
            chosen->size  = size;
            chosen->next  = rest;
        }
        chosen->is_free = 0;
        result = chosen + 1;
    }

    mutex_unlock(&mem_lock);
    return result;
}

void* kcalloc(u32 count, u32 size) {
    if (count != 0 && size > HEAP_SIZE / count)
        return nullptr;                               // mencegah overflow count * size
    u32 total = count * size;
    u8* p = static_cast<u8*>(kmalloc(total));
    if (p)
        for (u32 i = 0; i < total; ++i) p[i] = 0;
    return p;
}

int kfree(void* ptr) {
    if (!ptr)
        return MEM_FREE_INVALID;

    mutex_lock(&mem_lock);

    // Pastikan ptr benar-benar awal muatan sebuah blok milik heap ini.
    Block* target = nullptr;
    for (Block* b = head; b; b = b->next) {
        if (static_cast<void*>(b + 1) == ptr) { target = b; break; }
    }

    int result;
    if (!target || target->magic != MAGIC) {
        result = MEM_FREE_INVALID;
    } else if (target->is_free) {
        result = MEM_FREE_DOUBLE;
    } else {
        target->is_free = 1;

        // Coalescing: gabungkan blok bebas yang bertetangga.
        for (Block* b = head; b; b = b->next) {
            while (b->is_free && b->next && b->next->is_free) {
                b->size += HDR + b->next->size;
                b->next->magic = 0;
                b->next = b->next->next;
            }
        }
        result = MEM_FREE_OK;
    }

    mutex_unlock(&mem_lock);
    return result;
}

void mem_get_stats(MemStats* out) {
    mutex_lock(&mem_lock);

    out->total        = HEAP_SIZE;
    out->used_bytes   = 0;
    out->free_bytes   = 0;
    out->used_blocks  = 0;
    out->free_blocks  = 0;
    out->largest_free = 0;

    for (Block* b = head; b; b = b->next) {
        if (b->is_free) {
            out->free_bytes += b->size;
            ++out->free_blocks;
            if (b->size > out->largest_free)
                out->largest_free = b->size;
        } else {
            out->used_bytes += b->size;
            ++out->used_blocks;
        }
    }
    out->overhead = HDR * (out->used_blocks + out->free_blocks);

    mutex_unlock(&mem_lock);
}

bool mem_block_info(int index, u32* offset, u32* size, bool* is_free) {
    bool found = false;

    mutex_lock(&mem_lock);
    int i = 0;
    for (Block* b = head; b; b = b->next, ++i) {
        if (i == index) {
            *offset  = static_cast<u32>(reinterpret_cast<u8*>(b + 1) - heap);
            *size    = b->size;
            *is_free = (b->is_free != 0);
            found = true;
            break;
        }
    }
    mutex_unlock(&mem_lock);
    return found;
}

u32 mem_offset(const void* ptr) {
    return static_cast<u32>(static_cast<const u8*>(ptr) - heap);
}

void mem_set_policy(int p) { policy = p; }
int  mem_get_policy()      { return policy; }