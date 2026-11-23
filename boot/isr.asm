BITS 32

; Stub interrupt untuk penjadwal (Nama 2).
; Vektor 32 (IRQ0 / timer PIT) -> isr_timer  : preemption
; Vektor 128 (int 0x80)        -> isr_yield  : tugas menyerahkan CPU secara sukarela
;
; Alur: simpan semua register (pushad), panggil sched_isr(esp, dari_timer),
; ganti ESP dengan nilai kembalian (= stack tugas berikutnya), pulihkan register, iret.
; Itulah context switch.

section .text

extern sched_isr
global isr_timer
global isr_yield

isr_timer:
    pushad
    mov eax, esp
    push dword 1            ; dari_timer = 1
    push eax                ; esp tugas yang sedang berjalan
    call sched_isr
    add esp, 8
    mov esp, eax            ; pindah ke stack tugas berikutnya
    popad
    iret

isr_yield:
    pushad
    mov eax, esp
    push dword 0            ; dari_timer = 0
    push eax
    call sched_isr
    add esp, 8
    mov esp, eax
    popad
    iret

section .note.GNU-stack noalloc noexec nowrite progbits
