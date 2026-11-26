#pragma once

using u8  = unsigned char;
using u16 = unsigned short;
using u32 = unsigned int;

// ---------- Penjadwal CPU Preemptive (Round Robin) ----------
enum TaskState : int { TASK_FREE = 0, TASK_READY, TASK_RUNNING, TASK_BLOCKED, TASK_DEAD };

constexpr int MAX_TASKS     = 6;
constexpr int TASK_STACK    = 4096;   // byte per tugas
constexpr u32 TIMER_HZ      = 100;    // 1 tick = 10 ms
constexpr u32 QUANTUM_TICKS = 5;      // time quantum = 5 tick = 50 ms

struct TaskInfo {
    int         state;
    const char* name;
    u32         cpu_ticks;   // total tick yang dipakai tugas ini
    u32         switches;    // berapa kali tugas ini mendapat CPU
};

void sched_init();                                  // IDT + PIC + PIT + tugas utama (id 0)
int  task_create(const char* name, void (*entry)()); // -> id tugas, -1 jika penuh
void task_yield();                                  // serahkan CPU (int 0x80)
int  task_current();
u32  sched_ticks();                                 // jumlah tick sejak boot
bool task_get_info(int id, TaskInfo* out);
bool sched_all_done(int first_id);                  // semua tugas id >= first_id sudah selesai?

// Hentikan paksa sebuah thread. Ditolak jika thread itu main (id 0), thread yang sedang
// berjalan, atau thread yang sedang memegang mutex (agar mutex tidak terkunci selamanya).
enum KillResult : int { KILL_OK = 0, KILL_INVALID, KILL_MAIN, KILL_SELF, KILL_HOLDS_LOCK };
int  task_kill(int id);

// ---------- Sinkronisasi ----------
u32  irq_save();                 // matikan interrupt, kembalikan EFLAGS lama
void irq_restore(u32 flags);

struct Mutex     { volatile int locked; int owner; };
struct Semaphore { volatile int count; };

void mutex_lock(Mutex* m);
bool mutex_trylock(Mutex* m);
void mutex_unlock(Mutex* m);

void sem_init(Semaphore* s, int initial);
void sem_wait(Semaphore* s);     // P / down: blok jika count == 0
void sem_post(Semaphore* s);     // V / up

// Dipakai internal oleh sync.cpp
void task_lock_delta(int delta);       // catat mutex dipegang (+1) / dilepas (-1) oleh tugas sekarang (IF=0)
void task_block_on(const void* obj);   // tandai tugas sekarang BLOCKED menunggu obj (panggil saat IF=0)
void task_wake_all(const void* obj);   // bangunkan semua tugas yang menunggu obj (panggil saat IF=0)
