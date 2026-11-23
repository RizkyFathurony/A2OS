#include "scheduler.h"

extern "C" void isr_timer();
extern "C" void isr_yield();

static inline void outb(u16 port, u8 value) {
    asm volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

// ------------------------------------------------------------------
// IDT
// ------------------------------------------------------------------
struct __attribute__((packed)) IdtEntry {
    u16 offset_low;
    u16 selector;
    u8  zero;
    u8  flags;
    u16 offset_high;
};

struct __attribute__((packed)) IdtPtr {
    u16 limit;
    u32 base;
};

static IdtEntry idt[256];
static IdtPtr   idtr;

static void idt_set(int vec, void (*handler)(), u16 selector) {
    u32 addr = reinterpret_cast<u32>(handler);
    idt[vec].offset_low  = static_cast<u16>(addr & 0xFFFF);
    idt[vec].selector    = selector;
    idt[vec].zero        = 0;
    idt[vec].flags       = 0x8E;            // present, DPL 0, interrupt gate (IF otomatis 0)
    idt[vec].offset_high = static_cast<u16>((addr >> 16) & 0xFFFF);
}

// ------------------------------------------------------------------
// PIC & PIT
// ------------------------------------------------------------------
static void pic_remap() {
    outb(0x20, 0x11); outb(0xA0, 0x11);     // ICW1: init
    outb(0x21, 0x20); outb(0xA1, 0x28);     // ICW2: IRQ0-7 -> vektor 32-39, IRQ8-15 -> 40-47
    outb(0x21, 0x04); outb(0xA1, 0x02);     // ICW3: kaskade
    outb(0x21, 0x01); outb(0xA1, 0x01);     // ICW4: mode 8086
    outb(0x21, 0xFE);                       // hanya IRQ0 (timer) aktif; keyboard tetap polling
    outb(0xA1, 0xFF);
}

static void pit_init(u32 hz) {
    u32 divisor = 1193182 / hz;
    outb(0x43, 0x36);                       // channel 0, lo/hi byte, mode 3 (square wave)
    outb(0x40, static_cast<u8>(divisor & 0xFF));
    outb(0x40, static_cast<u8>((divisor >> 8) & 0xFF));
}

// ------------------------------------------------------------------
// Tabel tugas (PCB sederhana)
// ------------------------------------------------------------------
struct Task {
    u32         esp;          // stack pointer tersimpan saat tidak berjalan
    int         state;
    const char* name;
    const void* waiting;      // objek sinkronisasi yang ditunggu (jika BLOCKED)
    u32         cpu_ticks;
    u32         switches;
};

static Task tasks[MAX_TASKS];
alignas(16) static u8 stacks[MAX_TASKS][TASK_STACK];

static volatile int current = 0;
static volatile u32 ticks = 0;
static u32 slice_left = QUANTUM_TICKS;
static u16 code_selector = 0x08;

// ------------------------------------------------------------------
// Fungsi yang dipanggil stub assembly: inilah inti penjadwal Round Robin.
// Mengembalikan ESP milik tugas yang akan berjalan berikutnya.
// ------------------------------------------------------------------
extern "C" u32 sched_isr(u32 esp, u32 from_timer) {
    if (from_timer) {
        outb(0x20, 0x20);                   // EOI ke PIC
        ++ticks;
        ++tasks[current].cpu_ticks;

        // Quantum belum habis dan tugas masih bisa jalan -> lanjutkan tanpa pindah.
        if (--slice_left > 0 && tasks[current].state == TASK_RUNNING)
            return esp;
    }

    // Simpan konteks tugas sekarang. Jika quantum habis, ia kembali ke belakang antrean.
    tasks[current].esp = esp;
    if (tasks[current].state == TASK_RUNNING)
        tasks[current].state = TASK_READY;

    // Round Robin: cari tugas READY berikutnya, berputar mulai dari tugas setelah current.
    int next = -1;
    for (int i = 1; i <= MAX_TASKS; ++i) {
        int idx = (current + i) % MAX_TASKS;
        if (tasks[idx].state == TASK_READY) { next = idx; break; }
    }

    if (next < 0) {
        // Tidak ada yang READY (semua menunggu). Tetap di tugas ini; ia akan memeriksa ulang kondisinya.
        next = current;
    }

    if (next != current) ++tasks[next].switches;
    tasks[next].state = TASK_RUNNING;
    current = next;
    slice_left = QUANTUM_TICKS;
    return tasks[next].esp;
}

// ------------------------------------------------------------------
// API
// ------------------------------------------------------------------
u32 irq_save() {
    u32 flags;
    asm volatile ("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

void irq_restore(u32 flags) {
    asm volatile ("push %0; popf" : : "r"(flags) : "memory", "cc");
}

static void task_exit() {
    u32 f = irq_save();
    tasks[current].state = TASK_DEAD;
    irq_restore(f);
    for (;;) task_yield();                  // tidak pernah dijadwalkan lagi
}

int task_create(const char* name, void (*entry)()) {
    u32 f = irq_save();

    int id = -1;
    for (int i = 1; i < MAX_TASKS; ++i) {   // id 0 = tugas utama (kernel_main)
        if (tasks[i].state == TASK_FREE || tasks[i].state == TASK_DEAD) { id = i; break; }
    }
    if (id < 0) { irq_restore(f); return -1; }

    // Bangun frame awal di stack baru supaya terlihat seperti tugas yang pernah diinterupsi:
    //   [return -> task_exit] [EFLAGS][CS][EIP] [EAX..EDI hasil pushad]
    u32* sp = reinterpret_cast<u32*>(&stacks[id][TASK_STACK]);
    *--sp = reinterpret_cast<u32>(&task_exit);   // alamat kembali jika entry() selesai
    *--sp = 0x202;                               // EFLAGS: IF = 1
    *--sp = code_selector;                       // CS
    *--sp = reinterpret_cast<u32>(entry);        // EIP
    for (int r = 0; r < 8; ++r) *--sp = 0;       // EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI

    tasks[id].esp       = reinterpret_cast<u32>(sp);
    tasks[id].state     = TASK_READY;
    tasks[id].name      = name;
    tasks[id].waiting   = nullptr;
    tasks[id].cpu_ticks = 0;
    tasks[id].switches  = 0;

    irq_restore(f);
    return id;
}

void task_yield() {
    asm volatile ("int $0x80" : : : "memory");
}

int  task_current() { return current; }
u32  sched_ticks()  { return ticks; }

bool task_get_info(int id, TaskInfo* out) {
    if (id < 0 || id >= MAX_TASKS || tasks[id].state == TASK_FREE) return false;
    out->state     = tasks[id].state;
    out->name      = tasks[id].name;
    out->cpu_ticks = tasks[id].cpu_ticks;
    out->switches  = tasks[id].switches;
    return true;
}

bool sched_all_done(int first_id) {
    for (int i = first_id; i < MAX_TASKS; ++i) {
        int s = tasks[i].state;
        if (s == TASK_READY || s == TASK_RUNNING || s == TASK_BLOCKED) return false;
    }
    return true;
}

void task_block_on(const void* obj) {
    tasks[current].state   = TASK_BLOCKED;
    tasks[current].waiting = obj;
}

void task_wake_all(const void* obj) {
    for (int i = 0; i < MAX_TASKS; ++i) {
        if (tasks[i].state == TASK_BLOCKED && tasks[i].waiting == obj) {
            tasks[i].state   = TASK_READY;
            tasks[i].waiting = nullptr;
        }
    }
}

void sched_init() {
    // Selector kode dibaca dari CS saat ini supaya cocok dengan GDT bawaan GRUB.
    u16 cs;
    asm volatile ("mov %%cs, %0" : "=r"(cs));
    code_selector = cs;

    // Tugas utama (kernel_main / shell) = id 0; stack-nya adalah stack boot.
    tasks[0].state     = TASK_RUNNING;
    tasks[0].name      = "main";
    tasks[0].waiting   = nullptr;
    tasks[0].cpu_ticks = 0;
    tasks[0].switches  = 1;
    current    = 0;
    slice_left = QUANTUM_TICKS;

    idt_set(32,   isr_timer, code_selector);
    idt_set(0x80, isr_yield, code_selector);

    idtr.limit = sizeof(idt) - 1;
    idtr.base  = reinterpret_cast<u32>(&idt[0]);
    asm volatile ("lidt %0" : : "m"(idtr));

    pic_remap();
    pit_init(TIMER_HZ);

    asm volatile ("sti");                   // mulai preemption
}
