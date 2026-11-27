#include "scheduler.h"

// ------------------------------------------------------------------
// Mutex: hanya satu tugas boleh memegang kunci. Tugas yang gagal masuk
// diblok (BLOCKED), bukan busy-wait, lalu dibangunkan saat kunci dilepas.
// Bagian kritis dilindungi dengan cli/sti (irq_save/irq_restore) karena
// hanya ada 1 CPU.
// ------------------------------------------------------------------
bool mutex_trylock(Mutex* m) {
    u32 f = irq_save();
    bool ok = (m->locked == 0);
    if (ok) { m->locked = 1; m->owner = task_current(); task_lock_delta(1); task_held_add(m); }
    irq_restore(f);
    return ok;
}

void mutex_lock(Mutex* m) {
    for (;;) {
        u32 f = irq_save();
        if (m->locked == 0) {
            m->locked = 1;
            m->owner  = task_current();
            task_lock_delta(1);
            task_held_add(m);
            irq_restore(f);
            return;
        }
        task_block_on(m, WAIT_MUTEX);       // masuk antrean tunggu mutex (tercatat untuk deteksi deadlock)
        irq_restore(f);
        task_yield();                       // serahkan CPU, coba lagi saat dibangunkan
    }
}

void mutex_unlock(Mutex* m) {
    u32 f = irq_save();
    if (m->locked && m->owner == task_current()) {
        m->locked = 0;
        m->owner  = -1;
        task_lock_delta(-1);
        task_held_remove(m);
        task_wake_all(m);
    }
    irq_restore(f);
}

// ------------------------------------------------------------------
// Semaphore berhitung (counting semaphore).
// ------------------------------------------------------------------
void sem_init(Semaphore* s, int initial) {
    s->count = initial;
}

void sem_wait(Semaphore* s) {
    for (;;) {
        u32 f = irq_save();
        if (s->count > 0) {
            --s->count;
            irq_restore(f);
            return;
        }
        task_block_on(s);
        irq_restore(f);
        task_yield();
    }
}

void sem_post(Semaphore* s) {
    u32 f = irq_save();
    ++s->count;
    task_wake_all(s);
    irq_restore(f);
}