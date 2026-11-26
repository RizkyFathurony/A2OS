# A2OS — tahap pertama

Prototype kernel 32-bit x86 dengan C++ dan NASM. Boot melalui GRUB Multiboot,
menampilkan logo ASCII A2OS, lalu menyediakan shell internal dengan perintah
`help`, `about`, `clear`, dan `echo`.

## 1. Install dependency di Ubuntu WSL2

```bash
sudo apt update
sudo apt install build-essential g++-multilib nasm grub-pc-bin grub-common xorriso qemu-system-x86 -y
```

## 2. Build

Jalankan dari folder proyek:

```bash
make
```

Periksa header Multiboot:

```bash
make check
```

## 3. Jalankan di QEMU

```bash
make run
```

Shell menerima input dari keyboard QEMU. Klik jendela QEMU jika keyboard belum
terfokus. Keluar dari QEMU dengan `Ctrl+Alt+G` atau tutup jendelanya.

## Catatan

- Sudah ada: boot GRUB Multiboot, login, asisten diagnosis, penjadwal Round Robin
  preemptive, mutex, semaphore, dan RAMDisk sederhana dengan hak akses Admin/User.
- Belum ada: alokator memori dinamis dan deteksi deadlock.
- Kernel berjalan dalam mode x86 32-bit dan memakai VGA text mode serta polling
  keyboard PS/2 (hanya IRQ0 timer yang aktif untuk penjadwal).
- Jangan menganggap pesan boot sebagai bukti fitur yang belum diimplementasikan.

## Pengelolaan thread (shell: menu 2 "Terminal A2OS")

- `threads` (alias `ps`): tabel semua thread: id, nama, status (RUNNING / READY /
  BLOCKED / SELESAI), CPU (tick dan persen), dan berapa kali dijadwalkan. Id 0 = `main` (shell).
- `spawn`: membuat thread `worker` yang aktif sekitar 10 detik. Jalankan `spawn`
  beberapa kali lalu `threads` untuk melihat thread bergantian memakai CPU.
  Maksimal 6 slot thread, termasuk `main`.
- `kill <id>`: menghentikan thread. Ditolak untuk `main` (id 0), thread yang sedang
  berjalan, dan thread yang sedang memegang mutex (supaya mutex tidak terkunci selamanya).
  Thread yang menunggu semaphore boleh dihentikan, tetapi hitungan semaphore tidak dipulihkan.
- `menu`: kembali ke menu utama.

## Demo penjadwalan & sinkronisasi (shell: menu 2 "Terminal A2OS")

- `sched`: 3 tugas (A, B, C) dijadwalkan Round Robin secara preemptive.
- `race`: dua tugas menambah counter bersama tanpa kunci (hasil salah, race condition).
- `race lock`: sama, tetapi dilindungi mutex (hasil benar).
- `prodcons`: produsen-konsumen dengan semaphore dan buffer 3 slot.

Berkas terkait: `boot/isr.asm` (context switch), `kernel/scheduler.cpp` (IDT, PIC, PIT, Round Robin), `kernel/sync.cpp` (mutex, semaphore).
