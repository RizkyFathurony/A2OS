#include "scheduler.h"
#include "memory.h"

static volatile u16* const VGA = reinterpret_cast<volatile u16*>(0xB8000);
static const u8 COLOR = 0x0F;

static const u8 COLOR_GREEN  = 0x0A;  // hijau terang
static const u8 COLOR_YELLOW = 0x0E;  // kuning
static const u8 COLOR_RED    = 0x0C;  // merah terang

static u8 text_color = COLOR;         // warna teks yang sedang aktif

static int cursor_x = 0;
static int cursor_y = 0;

// Status keyboard dibuat global agar tetap konsisten di shell dan menu diagnosis.
static bool shift_down   = false;
static bool caps_lock = false;


// Hak akses sederhana: Admin memiliki akses penuh, User hanya mengelola
// berkas yang dibuat oleh akunnya sendiri.
static bool current_is_admin = false;
static const int MAX_FILES = 8;
static const int FILE_NAME_SIZE = 24;
static const int FILE_CONTENT_SIZE = 160;

struct RamFile {
    bool used;
    bool owner_is_admin;
    char name[FILE_NAME_SIZE];
    char content[FILE_CONTENT_SIZE];
};

static RamFile ram_files[MAX_FILES];

static inline u8 inb(u16 port) {
    u8 value;
    asm volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outb(u16 port, u8 value) {
    asm volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outl(u16 port, u32 value) {
    asm volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

// Perbarui posisi hardware cursor VGA (mode teks 80x25).
static void update_cursor() {
    if (cursor_x < 0) cursor_x = 0;
    if (cursor_y < 0) cursor_y = 0;

    // cursor_x == 80 adalah keadaan sah ("pindah baris di karakter berikutnya").
    // Jangan diubah di sini; cukup batasi posisi cursor hardware.
    int hw_x = (cursor_x > 79) ? 79 : cursor_x;
    int hw_y = (cursor_y > 24) ? 24 : cursor_y;

    u16 position = static_cast<u16>(hw_y * 80 + hw_x);

    outb(0x3D4, 0x0F);
    outb(0x3D5, static_cast<u8>(position & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, static_cast<u8>((position >> 8) & 0xFF));
}

// Aktifkan cursor dan atur bentuknya agar terlihat.
static void enable_cursor() {
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x00);
    outb(0x3D4, 0x0B);
    outb(0x3D5, 0x0F);
    update_cursor();
}

static void scroll_screen() {
    for (int i = 0; i < 24 * 80; ++i)
        VGA[i] = VGA[i + 80];

    for (int i = 24 * 80; i < 25 * 80; ++i)
        VGA[i] = (static_cast<u16>(COLOR) << 8) | ' ';

    cursor_y = 24;
}

static void clear_screen() {
    for (int i = 0; i < 80 * 25; ++i)
        VGA[i] = (static_cast<u16>(COLOR) << 8) | ' ';

    cursor_x = 0;
    cursor_y = 0;
    update_cursor();
}

static void put_char(char c) {
    if (c == '\n') {
        cursor_x = 0;
        ++cursor_y;
    } else if (c == '\r') {
        cursor_x = 0;
    } else {
        if (cursor_x >= 80) {
            cursor_x = 0;
            ++cursor_y;
        }

        if (cursor_y >= 25)
            scroll_screen();

        VGA[cursor_y * 80 + cursor_x] =
            (static_cast<u16>(text_color) << 8) | static_cast<u8>(c);
        ++cursor_x;
    }

    if (cursor_y >= 25)
        scroll_screen();

    update_cursor();
}

// Mutex konsol: mencegah keluaran beberapa tugas saling tercampur di layar VGA.
static Mutex console_lock = {0, -1};

static void print(const char* s) {
    mutex_lock(&console_lock);
    while (*s)
        put_char(*s++);
    mutex_unlock(&console_lock);
}

// Cetak teks dengan warna tertentu, lalu kembalikan warna semula.
// Warna diubah di dalam console_lock agar tidak bocor ke tugas lain.
static void print_colored(const char* s, u8 color) {
    mutex_lock(&console_lock);
    u8 old = text_color;
    text_color = color;
    while (*s)
        put_char(*s++);
    text_color = old;
    mutex_unlock(&console_lock);
}

// Tingkat kendala hasil diagnosis.
enum Severity { RINGAN, SEDANG, BERAT };

// Menentukan tingkat kendala dari kategori dan jumlah jawaban "ya".
static Severity determine_severity(int category, int yes_count, bool answer_three) {
    // Baterai membengkak/sangat panas berbahaya, langsung dianggap berat.
    if (category == 1 && answer_three)
        return BERAT;

    if (yes_count >= 3)
        return BERAT;
    if (yes_count == 2)
        return SEDANG;
    return RINGAN;
}

static void print_severity(Severity level) {
    print("Tingkat   : ");

    if (level == BERAT) {
        print_colored("BERAT (merah)\n", COLOR_RED);
        print("            Segera bawa ke teknisi untuk pemeriksaan.\n");
    } else if (level == SEDANG) {
        print_colored("SEDANG (kuning)\n", COLOR_YELLOW);
        print("            Sebaiknya diperiksa dalam waktu dekat.\n");
    } else {
        print_colored("RINGAN (hijau)\n", COLOR_GREEN);
        print("            Coba penanganan dasar terlebih dahulu.\n");
    }
}

static bool equals(const char* a, const char* b) {
    while (*a && *b && *a == *b) {
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static void print_logo() {
    print("      _    ____   ___  ____  \n");
    print("     / \\  |___ \\ / _ \\/ ___| \n");
    print("    / _ \\   __) | | | \\___ \\ \n");
    print("   / ___ \\ / __/| |_| |___) |\n");
    print("  /_/   \\_\\_____|\\___/|____/ \n\n");
}

// Peta scancode keyboard PS/2 dengan layout US.
static char scancode_to_ascii(u8 code, bool shift, bool caps) {
    static const char normal[128] = {
        0, 27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
        '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',0,
        'a','s','d','f','g','h','j','k','l',';','\'','`',0,'\\',
        'z','x','c','v','b','n','m',',','.','/',0,'*',0,' '
    };

    static const char shifted[128] = {
        0, 27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
        '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,
        'A','S','D','F','G','H','J','K','L',':','"','~',0,'|',
        'Z','X','C','V','B','N','M','<','>','?',0,'*',0,' '
    };

    if (code >= 128)
        return 0;

    char c = normal[code];

    if (c >= 'a' && c <= 'z') {
        if (shift != caps)
            c = shifted[code];
    } else if (shift) {
        c = shifted[code];
    }

    return c;
}

// Membaca satu baris dari keyboard. Mendukung Enter dan Backspace.
static void read_line(char* buffer, int max_length) {
    int length = 0;
    buffer[0] = '\0';

    while (true) {
        if ((inb(0x64) & 1) == 0)
            continue;

        u8 scan = inb(0x60);

        if (scan == 0x2A || scan == 0x36) {
            shift_down = true;
            continue;
        }

        if (scan == 0xAA || scan == 0xB6) {
            shift_down = false;
            continue;
        }

        if (scan == 0xE0)
            continue;

        if (scan == 0x3A) {
            caps_lock = !caps_lock;
            continue;
        }

        if (scan & 0x80)
            continue;

        if (scan == 0x1C) {
            put_char('\n');
            buffer[length] = '\0';
            return;
        }

        char c = scancode_to_ascii(scan, shift_down, caps_lock);

        if (c == '\b') {
            if (length > 0) {
                --length;

                if (cursor_x > 0) {
                    --cursor_x;
                } else if (cursor_y > 0) {
                    --cursor_y;
                    cursor_x = 79;
                }

                VGA[cursor_y * 80 + cursor_x] =
                    (static_cast<u16>(COLOR) << 8) | ' ';
                update_cursor();
            }
        } else if (c >= 32 && c <= 126 && length < max_length - 1) {
            buffer[length++] = c;
            put_char(c);
        }
    }
}

// Membaca bilangan bulat tak negatif (maks 4 digit). Mengembalikan -1 jika kosong,
// mengandung karakter selain angka, atau terlalu panjang.
static int parse_number(const char* s) {
    if (s[0] == '\0')
        return -1;

    int value = 0;
    for (int i = 0; s[i] != '\0'; ++i) {
        if (s[i] < '0' || s[i] > '9' || i >= 4)
            return -1;
        value = value * 10 + (s[i] - '0');
    }
    return value;
}

static int read_number(const char* prompt) {
    char input[16];

    print(prompt);
    read_line(input, 16);

    return parse_number(input);
}

static bool ask_yes_no(const char* question) {
    char answer[8];

    while (true) {
        print(question);
        print(" (y/n atau yes/no): ");
        read_line(answer, 8);

        if ((answer[0] == 'y' || answer[0] == 'Y') && answer[1] == '\0')
            return true;

        if ((answer[0] == 'y' || answer[0] == 'Y') &&
            (answer[1] == 'e' || answer[1] == 'E') &&
            (answer[2] == 's' || answer[2] == 'S') &&
            answer[3] == '\0')
            return true;

        if ((answer[0] == 'n' || answer[0] == 'N') && answer[1] == '\0')
            return false;

        if ((answer[0] == 'n' || answer[0] == 'N') &&
            (answer[1] == 'o' || answer[1] == 'O') &&
            answer[2] == '\0')
            return false;

        print("Input tidak valid. Masukkan y, yes, n, atau no.\n");
    }
}

// ---------- VFS RAMDisk sederhana ----------

static void copy_string(char* destination, const char* source, int max_size) {
    int i = 0;
    while (source[i] && i < max_size - 1) {
        destination[i] = source[i];
        ++i;
    }
    destination[i] = '\0';
}

static int find_file(const char* name) {
    for (int i = 0; i < MAX_FILES; ++i) {
        if (ram_files[i].used && equals(ram_files[i].name, name))
            return i;
    }
    return -1;
}

static bool can_access_file(int index) {
    if (index < 0 || index >= MAX_FILES || !ram_files[index].used)
        return false;

    return current_is_admin ||
           ram_files[index].owner_is_admin == current_is_admin;
}

static void vfs_list() {
    bool found = false;
    print("Daftar berkas:\n");

    for (int i = 0; i < MAX_FILES; ++i) {
        if (!ram_files[i].used)
            continue;

        if (!current_is_admin && ram_files[i].owner_is_admin)
            continue;

        print("  ");
        print(ram_files[i].name);
        print(ram_files[i].owner_is_admin ? " [ADMIN]\n" : " [USER]\n");
        found = true;
    }

    if (!found)
        print("  Belum ada berkas yang dapat ditampilkan.\n");
    print("\n");
}

static void vfs_create() {
    char name[FILE_NAME_SIZE];
    char content[FILE_CONTENT_SIZE];

    print("Nama file (tanpa spasi): ");
    read_line(name, FILE_NAME_SIZE);

    if (name[0] == '\0') {
        print("Nama file tidak boleh kosong.\n\n");
        return;
    }

    if (find_file(name) >= 0) {
        print("Nama file sudah digunakan.\n\n");
        return;
    }

    int slot = -1;
    for (int i = 0; i < MAX_FILES; ++i) {
        if (!ram_files[i].used) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        print("RAMDisk penuh. Hapus berkas yang tidak diperlukan.\n\n");
        return;
    }

    print("Isi file: ");
    read_line(content, FILE_CONTENT_SIZE);

    ram_files[slot].used = true;
    ram_files[slot].owner_is_admin = current_is_admin;
    copy_string(ram_files[slot].name, name, FILE_NAME_SIZE);
    copy_string(ram_files[slot].content, content, FILE_CONTENT_SIZE);

    print("File berhasil dibuat.\n\n");
}

static void vfs_read() {
    char name[FILE_NAME_SIZE];
    print("Nama file: ");
    read_line(name, FILE_NAME_SIZE);

    int index = find_file(name);
    if (index < 0) {
        print("File tidak ditemukan.\n\n");
        return;
    }

    if (!can_access_file(index)) {
        print("Akses ditolak. File ini milik Admin.\n\n");
        return;
    }

    print("Isi ");
    print(ram_files[index].name);
    print(":\n");
    print(ram_files[index].content);
    print("\n\n");
}

static void vfs_write() {
    char name[FILE_NAME_SIZE];
    char content[FILE_CONTENT_SIZE];

    print("Nama file: ");
    read_line(name, FILE_NAME_SIZE);

    int index = find_file(name);
    if (index < 0) {
        print("File tidak ditemukan.\n\n");
        return;
    }

    if (!can_access_file(index)) {
        print("Akses ditolak. Anda bukan pemilik file.\n\n");
        return;
    }

    print("Isi baru: ");
    read_line(content, FILE_CONTENT_SIZE);
    copy_string(ram_files[index].content, content, FILE_CONTENT_SIZE);
    print("File berhasil diperbarui.\n\n");
}

static void vfs_delete() {
    char name[FILE_NAME_SIZE];
    print("Nama file: ");
    read_line(name, FILE_NAME_SIZE);

    int index = find_file(name);
    if (index < 0) {
        print("File tidak ditemukan.\n\n");
        return;
    }

    if (!can_access_file(index)) {
        print("Akses ditolak. Anda bukan pemilik file.\n\n");
        return;
    }

    ram_files[index].used = false;
    ram_files[index].name[0] = '\0';
    ram_files[index].content[0] = '\0';
    print("File berhasil dihapus.\n\n");
}

static bool login() {
    char username[24];
    char password[24];

    while (true) {
        clear_screen();
        print_logo();
        print("          A2OS LOGIN\n\n");
        print("Akun demo:\n");
        print("  Admin : admin / admin123\n");
        print("  User  : user  / user123\n\n");

        print("Username: ");
        read_line(username, 24);
        print("Password: ");
        read_line(password, 24);

        if (equals(username, "admin") && equals(password, "admin123")) {
            current_is_admin = true;
            print("Login sebagai Admin berhasil.\n");
            return true;
        }

        if (equals(username, "user") && equals(password, "user123")) {
            current_is_admin = false;
            print("Login sebagai User berhasil.\n");
            return true;
        }

        print("Username atau password salah.\n");
        print("Tekan Enter untuk mencoba lagi...");
        char wait[4];
        read_line(wait, 4);
    }
}

static void show_help() {
    print("Perintah yang tersedia:\n");
    print("  help      - menampilkan daftar perintah\n");
    print("  about     - informasi tentang A2OS\n");
    print("  diagnosis - membuka asisten diagnosis\n");
    print("  threads   - menampilkan daftar thread (alias: ps)\n");
    print("  spawn     - membuat thread latar belakang (aktif ~10 detik)\n");
    print("  kill <id> - menghentikan thread dengan id tersebut\n");
    print("  menu      - kembali ke menu utama\n");
    print("  sched     - demo penjadwalan Round Robin preemptive\n");
    print("  race      - demo race condition (tanpa kunci)\n");
    print("  race lock - demo sinkronisasi dengan mutex\n");
    print("  prodcons  - demo semaphore (produsen-konsumen)\n");
    print("  clear     - membersihkan layar\n");
    print("  echo      - menampilkan teks\n");
    print("  ls        - melihat daftar file RAMDisk\n");
    print("  create    - membuat file teks\n");
    print("  cat       - membaca isi file\n");
    print("  write     - mengubah isi file\n");
    print("  rm        - menghapus file\n");
    print("  whoami    - melihat akun aktif\n");
    print("  logout    - keluar dari akun\n");
    print("  mem       - statistik heap kernel (allocator)\n");
    print("  memmap    - peta blok heap\n");
    print("  alloc <n> - kmalloc n byte, memberi id\n");
    print("  free <id> - kfree alokasi dengan id tersebut\n");
    print("  memtest   - demo fragmentasi dan coalescing\n");
    print("  mempolicy - first / best (kebijakan alokasi)\n");
    print("  deadlock  - demo deadlock (juga: check, recover, order)\n");
    print("  shutdown  - mematikan A2OS \n\n");
}

// Hanya bekerja jika QEMU dijalankan dengan perangkat isa-debug-exit.
static void shutdown_qemu() {
    print("A2OS sedang dimatikan...\n");
    print("Terima kasih telah menggunakan A2OS!\n");

    outl(0xF4, 0x10);

    for (;;)
        asm volatile ("cli; hlt");
}

static void show_diagnosis_result(int device, int category, int yes_count, bool answer_three) {
    print("\n========== HASIL ANALISIS AWAL ==========\n");

    if (device == 1)
        print("Perangkat : Handphone\n");
    else
        print("Perangkat : Laptop\n");

    print_severity(determine_severity(category, yes_count, answer_three));

    if (category == 1) {
        print("Kategori  : Baterai / pengisian daya\n");
        if (answer_three) {
            print("Temuan    : Ada indikasi kondisi baterai tidak normal.\n");
            print("Saran     : Hentikan penggunaan jika baterai tampak membengkak atau terlalu panas.\n");
            print("            Minta teknisi memeriksa baterai dan rangkaian daya.\n");
        } else if (yes_count >= 2) {
            print("Kemungkinan: Baterai, kabel/adaptor, konektor, atau rangkaian charging bermasalah.\n");
            print("Saran      : Uji aksesori pengisi daya yang sesuai dan periksa kesehatan baterai.\n");
        } else {
            print("Kemungkinan: Gangguan daya belum dapat dipastikan dari gejala ini.\n");
            print("Saran      : Periksa adaptor, konektor, dan kondisi baterai lebih lanjut.\n");
        }
    } else if (category == 2) {
        print("Kategori  : Layar / komponen input\n");
        if (yes_count >= 2) {
            print("Kemungkinan: Layar, komponen input, kabel fleksibel, atau driver bermasalah.\n");
            print("Saran      : Periksa kondisi layar dan uji fungsi sentuh/keyboard/touchpad.\n");
        } else {
            print("Kemungkinan: Gangguan layar atau komponen input belum dapat dipastikan.\n");
            print("Saran      : Mulai ulang perangkat dan lakukan pengujian komponen.\n");
        }
    } else {
        print("Kategori  : Performa / sistem operasi\n");
        if (yes_count >= 2) {
            print("Kemungkinan: Beban aplikasi tinggi, penyimpanan penuh, atau sistem operasi bermasalah.\n");
            print("Saran      : Periksa ruang penyimpanan, aplikasi berjalan, dan pesan error sistem.\n");
        } else {
            print("Kemungkinan: Gangguan performa atau sistem belum menunjukkan penyebab pasti.\n");
            print("Saran      : Mulai ulang perangkat dan amati apakah masalah muncul kembali.\n");
        }
    }

    print("-----------------------------------------\n\n");
}

static void run_diagnosis() {
    int device = -1;
    int category = -1;

    clear_screen();
    print_logo();
    print("        A2OS - ASISTEN DIAGNOSIS TEKNISI\n\n");

    print("Pilih perangkat:\n");
    print("  1. Handphone\n");
    print("  2. Laptop\n");
    print("  0. Kembali\n\n");

    // Gunakan do-while agar input pertama benar-benar dibaca.
    do {
        device = read_number("Pilihan: ");
        if (device < 0 || device > 2)
            print("Pilihan tidak valid. Masukkan 0, 1, atau 2.\n");
    } while (device < 0 || device > 2);

    if (device == 0)
        return;

    clear_screen();
    print("PILIH KATEGORI MASALAH\n\n");
    print("  1. Baterai / pengisian daya bermasalah\n");
    print("  2. Layar / komponen input bermasalah\n");
    print("  3. Performa lambat / sistem operasi bermasalah\n");
    print("  0. Kembali\n\n");

    do {
        category = read_number("Kategori: ");
        if (category < 0 || category > 3)
            print("Pilihan tidak valid. Masukkan angka 0 sampai 3.\n");
    } while (category < 0 || category > 3);

    if (category == 0)
        return;

    clear_screen();
    print("PERTANYAAN DIAGNOSIS\n");

    int yes_count = 0;
    bool answer_three = false;

    if (category == 1) {
        if (ask_yes_no("1. Apakah baterai cepat habis atau perangkat sering mati?"))
            ++yes_count;
        if (ask_yes_no("2. Apakah perangkat sulit mengisi daya atau pengisian sering terputus?"))
            ++yes_count;
        answer_three = ask_yes_no("3. Apakah baterai terlihat membengkak atau terasa sangat panas?");
        if (answer_three) ++yes_count;
    } else if (category == 2) {
        if (ask_yes_no("1. Apakah layar bermasalah (gelap, bergaris, berkedip, atau berubah warna)?"))
            ++yes_count;

        if (device == 1) {
            if (ask_yes_no("2. Apakah layar sentuh tidak merespons atau bergerak sendiri?"))
                ++yes_count;
        } else {
            if (ask_yes_no("2. Apakah keyboard atau touchpad tidak merespons dengan normal?"))
                ++yes_count;
        }

        if (ask_yes_no("3. Apakah masalah tetap terjadi setelah perangkat dimulai ulang?"))
            ++yes_count;
    } else {
        if (ask_yes_no("1. Apakah perangkat terasa lambat saat digunakan atau membuka aplikasi?"))
            ++yes_count;
        if (ask_yes_no("2. Apakah perangkat gagal masuk ke sistem operasi atau menampilkan pesan error?"))
            ++yes_count;
        if (ask_yes_no("3. Apakah ruang penyimpanan hampir penuh atau masalah muncul setelah update?"))
            ++yes_count;
    }

    show_diagnosis_result(device, category, yes_count, answer_three);
    print("Tekan Enter untuk kembali ke menu utama...");
    char wait[4];
    read_line(wait, 4);
}

static void show_about() {
    print("A2OS - Mini OS sederhana berbasis kernel C++.\n");
    print("A2OS dikembangkan sebagai asisten diagnosis awal\n");
    print("untuk membantu teknisi mengelompokkan keluhan HP dan laptop.\n");
}


// ======================================================================
// Demo penjadwalan CPU preemptive dan sinkronisasi (Nama 2)
// ======================================================================
static void u32_to_str(u32 v, char* out) {
    char tmp[12];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) { tmp[n++] = static_cast<char>('0' + v % 10); v /= 10; }
    int i = 0;
    while (n > 0) out[i++] = tmp[--n];
    out[i] = '\0';
}

static void append(char* dst, const char* src) {
    while (*dst) ++dst;
    while (*src) *dst++ = *src++;
    *dst = '\0';
}

static void print_uint(u32 v) {
    char buf[12];
    u32_to_str(v, buf);
    print(buf);
}

// Menunggu n tick berdasarkan jam timer (bukan waktu CPU), jadi tugas pasti
// terpotong oleh preemption selama menunggu.
static void busy_wait_ticks(u32 n) {
    u32 start = sched_ticks();
    while (sched_ticks() - start < n) { }
}

static void wait_all_tasks() {
    while (!sched_all_done(1))
        task_yield();
}

// ---- Demo 1: Round Robin ----
static void rr_body(const char* tag) {
    for (u32 i = 1; i <= 5; ++i) {
        busy_wait_ticks(20);                       // 200 ms > quantum 50 ms -> pasti di-preempt
        char line[64];
        char num[12];
        line[0] = '\0';
        append(line, "  [Tugas ");
        append(line, tag);
        append(line, "] iterasi ");
        u32_to_str(i, num);    append(line, num);
        append(line, " pada tick ");
        u32_to_str(sched_ticks(), num);  append(line, num);
        append(line, "\n");
        print(line);                               // print() sudah dilindungi console_lock
    }
}
static void rr_task_a() { rr_body("A"); }
static void rr_task_b() { rr_body("B"); }
static void rr_task_c() { rr_body("C"); }

static const char* state_name(int s) {
    switch (s) {
        case TASK_READY:   return "READY  ";
        case TASK_RUNNING: return "RUNNING";
        case TASK_BLOCKED: return "BLOCKED";
        case TASK_DEAD:    return "SELESAI";
        default:           return "-      ";
    }
}

static void demo_sched() {
    print("Round Robin: quantum = 5 tick (50 ms), timer 100 Hz.\n");
    print("Membuat 3 tugas A, B, C...\n");
    task_create("A", rr_task_a);
    task_create("B", rr_task_b);
    task_create("C", rr_task_c);
    wait_all_tasks();

    print("\n  Tugas   Status   CPU(tick)  Dijadwalkan\n");
    for (int id = 0; id < MAX_TASKS; ++id) {
        TaskInfo ti;
        if (!task_get_info(id, &ti)) continue;
        char row[64];
        char num[12];
        row[0] = '\0';
        append(row, "  ");
        append(row, ti.name);
        int len = 0; while (row[len]) ++len;
        while (len < 10) { row[len++] = ' '; row[len] = '\0'; }   // kolom nama selebar 8
        append(row, state_name(ti.state));
        append(row, "  ");
        u32_to_str(ti.cpu_ticks, num); append(row, num);
        int len2 = 0; while (row[len2]) ++len2;
        while (len2 < 33) { row[len2++] = ' '; row[len2] = '\0'; }
        u32_to_str(ti.switches, num);  append(row, num);
        append(row, "\n");
        print(row);
    }
    print("\n");
}

// ---- Demo 2: race condition vs mutex ----
static volatile int shared_counter = 0;
static Mutex race_lock = {0, -1};
static bool race_use_lock = false;
static const int RACE_LOOPS = 300;

static void race_body() {
    for (int i = 0; i < RACE_LOOPS; ++i) {
        if (race_use_lock) mutex_lock(&race_lock);
        int tmp = shared_counter;                  // read
        for (volatile int k = 0; k < 200000; ++k) { }   // "pekerjaan" -> peluang di-preempt
        shared_counter = tmp + 1;                  // modify-write
        if (race_use_lock) mutex_unlock(&race_lock);
    }
}
static void race_task_1() { race_body(); }
static void race_task_2() { race_body(); }

static void demo_race(bool use_lock) {
    shared_counter = 0;
    race_use_lock = use_lock;
    print(use_lock ? "Mode: DENGAN mutex.\n" : "Mode: TANPA kunci.\n");
    print("2 tugas masing-masing menambah counter 300 kali. Harapan: 600.\n");
    print("Menjalankan... (tunggu beberapa detik)\n");
    task_create("R1", race_task_1);
    task_create("R2", race_task_2);
    wait_all_tasks();
    print("Hasil counter = ");
    print_uint(static_cast<u32>(shared_counter));
    if (shared_counter == 2 * RACE_LOOPS)
        print("  -> BENAR (tidak ada update yang hilang)\n\n");
    else
        print("  -> SALAH (terjadi race condition, update hilang)\n\n");
}

// ---- Demo 3: semaphore, produsen-konsumen dengan buffer 3 slot ----
static int pc_buf[3];
static int pc_in = 0, pc_out = 0;
static Semaphore pc_empty, pc_full;
static Mutex pc_lock = {0, -1};

static void pc_say(const char* who, int item) {
    char line[48];
    char num[12];
    line[0] = '\0';
    append(line, "  ");
    append(line, who);
    u32_to_str(static_cast<u32>(item), num);
    append(line, num);
    append(line, "\n");
    print(line);
}

static void producer_task() {
    for (int item = 1; item <= 8; ++item) {
        sem_wait(&pc_empty);                       // tunggu slot kosong
        mutex_lock(&pc_lock);
        pc_buf[pc_in] = item;
        pc_in = (pc_in + 1) % 3;
        mutex_unlock(&pc_lock);
        sem_post(&pc_full);
        pc_say("Produsen membuat item ", item);
        busy_wait_ticks(5);
    }
}

static void consumer_task() {
    for (int n = 0; n < 8; ++n) {
        sem_wait(&pc_full);                        // tunggu ada item
        mutex_lock(&pc_lock);
        int item = pc_buf[pc_out];
        pc_out = (pc_out + 1) % 3;
        mutex_unlock(&pc_lock);
        sem_post(&pc_empty);
        pc_say("                 Konsumen mengambil item ", item);
        busy_wait_ticks(20);                       // konsumen lebih lambat -> produsen sempat diblok
    }
}

static void demo_prodcons() {
    pc_in = pc_out = 0;
    sem_init(&pc_empty, 3);
    sem_init(&pc_full, 0);
    print("Produsen cepat, konsumen lambat, buffer 3 slot (semaphore).\n");
    task_create("prod", producer_task);
    task_create("cons", consumer_task);
    wait_all_tasks();
    print("Selesai.\n\n");
}

// ======================================================================
// Pengelolaan thread: tampilkan daftar thread dan buat thread latar belakang
// ======================================================================

// Tambahkan teks lalu isi spasi sampai teks yang ditambahkan selebar `width`.
static void append_padded(char* dst, const char* src, int width) {
    while (*dst) ++dst;
    int n = 0;
    while (*src) { *dst++ = *src++; ++n; }
    while (n < width) { *dst++ = ' '; ++n; }
    *dst = '\0';
}

static void append_uint_padded(char* dst, u32 v, int width) {
    char num[12];
    u32_to_str(v, num);
    append_padded(dst, num, width);
}

// Perintah `threads` / `ps`: tabel semua thread beserta statusnya.
static void show_threads() {
    u32 now = sched_ticks();

    print("Uptime penjadwal: ");
    print_uint(now / TIMER_HZ);
    print(" detik (");
    print_uint(now);
    print(" tick), quantum ");
    print_uint(QUANTUM_TICKS);
    print(" tick\n\n");

    print("  ID  Nama      Status   CPU (tick)  CPU%  Dijadwalkan\n");
    print("  --  --------  -------  ----------  ----  -----------\n");

    int shown = 0;
    for (int id = 0; id < MAX_TASKS; ++id) {
        TaskInfo ti;
        if (!task_get_info(id, &ti)) continue;

        u32 pct = (now > 0) ? (ti.cpu_ticks * 100u) / now : 0;

        char row[80];
        row[0] = '\0';
        append_padded(row, "", 2);
        append_uint_padded(row, static_cast<u32>(id), 4);
        append_padded(row, ti.name, 10);
        append_padded(row, state_name(ti.state), 9);
        append_uint_padded(row, ti.cpu_ticks, 12);
        append_uint_padded(row, pct, 6);
        append_uint_padded(row, ti.switches, 0);
        append(row, "\n");
        print(row);
        ++shown;
    }

    print("\nTotal thread terdaftar: ");
    print_uint(static_cast<u32>(shown));
    print(" dari ");
    print_uint(MAX_TASKS);
    print(" slot. Id 0 = main (shell).\n");
    print("Status: RUNNING = sedang memakai CPU, READY = menunggu giliran,\n");
    print("        BLOCKED = menunggu mutex/semaphore, SELESAI = sudah berakhir.\n\n");
}

// Thread latar belakang: berputar ~10 detik berdasarkan jam timer lalu selesai.
// Tidak mencetak apa pun supaya tidak mengganggu shell; amati lewat `threads`.
static void worker_task() {
    u32 start = sched_ticks();
    while (sched_ticks() - start < 10 * TIMER_HZ) { }
}

// Perintah `spawn`: buat satu thread worker.
static void spawn_worker() {
    int id = task_create("worker", worker_task);
    if (id < 0) {
        print("Gagal membuat thread: semua slot terpakai. Tunggu thread selesai.\n\n");
        return;
    }
    print("Thread 'worker' dibuat dengan id ");
    print_uint(static_cast<u32>(id));
    print(". Jalankan 'threads' untuk melihatnya (aktif ~10 detik).\n\n");
}

// Perintah `kill <id>`: hentikan thread. Pengaman ada di task_kill().
static void kill_thread(const char* arg) {
    int id = parse_number(arg);
    if (id < 0) {
        print("Penggunaan: kill <id>   (lihat id lewat 'threads')\n\n");
        return;
    }

    int result = task_kill(id);
    if (result == KILL_OK) {
        print("Thread ");
        print_uint(static_cast<u32>(id));
        print(" dihentikan.\n\n");
    } else if (result == KILL_MAIN) {
        print("Ditolak: thread main (id 0) adalah shell dan tidak boleh dihentikan.\n\n");
    } else if (result == KILL_SELF) {
        print("Ditolak: thread yang sedang berjalan tidak bisa menghentikan dirinya sendiri.\n\n");
    } else if (result == KILL_HOLDS_LOCK) {
        print("Ditolak: thread sedang memegang mutex. Coba lagi sebentar lagi.\n\n");
    } else {
        print("Thread tidak ditemukan atau sudah selesai.\n\n");
    }
}

// ======================================================================
// Memory allocator (kmalloc/kfree) dan deadlock (deteksi + pemulihan)
// ======================================================================
static bool starts_with(const char* s, const char* prefix) {
    while (*prefix) {
        if (*s != *prefix) return false;
        ++s;
        ++prefix;
    }
    return true;
}

// ---------- Memory allocator ----------
static void print_stat(const char* label, u32 bytes, u32 blocks) {
    print(label);
    print_uint(bytes);
    print(" byte (");
    print_uint(blocks);
    print(" blok)\n");
}

static void show_mem() {
    MemStats st;
    mem_get_stats(&st);

    u32 frag = 0;
    if (st.free_bytes > 0)
        frag = 100u - (st.largest_free * 100u) / st.free_bytes;

    print("Heap kernel : ");
    print_uint(st.total);
    print(" byte, kebijakan ");
    print(mem_get_policy() == MEM_BEST_FIT ? "best-fit\n" : "first-fit\n");
    print_stat("Terpakai    : ", st.used_bytes, st.used_blocks);
    print_stat("Bebas       : ", st.free_bytes, st.free_blocks);
    print("Overhead    : ");
    print_uint(st.overhead);
    print(" byte (header blok)\n");
    print("Blok bebas terbesar: ");
    print_uint(st.largest_free);
    print(" byte, fragmentasi ");
    print_uint(frag);
    print("%\n\n");
}

static void show_memmap() {
    print("  No  Offset  Ukuran  Status\n");
    print("  --  ------  ------  --------\n");

    for (int i = 0; ; ++i) {
        u32 offset, size;
        bool is_free;
        if (!mem_block_info(i, &offset, &size, &is_free))
            break;

        char row[64];
        row[0] = '\0';
        append_padded(row, "", 2);
        append_uint_padded(row, static_cast<u32>(i), 4);
        append_uint_padded(row, offset, 8);
        append_uint_padded(row, size, 8);
        append(row, is_free ? "BEBAS\n" : "TERPAKAI\n");
        print(row);
    }
    print("\n");
}

static const int MAX_HANDLES = 16;
static void* alloc_handles[MAX_HANDLES];

static void alloc_cmd(const char* arg) {
    int n = parse_number(arg);
    if (n <= 0) {
        print("Penggunaan: alloc <ukuran byte>   contoh: alloc 128\n\n");
        return;
    }

    int slot = -1;
    for (int i = 0; i < MAX_HANDLES; ++i) {
        if (!alloc_handles[i]) { slot = i; break; }
    }
    if (slot < 0) {
        print("Tabel alokasi penuh (16). Bebaskan dulu dengan 'free <id>'.\n\n");
        return;
    }

    void* p = kmalloc(static_cast<u32>(n));
    if (!p) {
        print("Gagal: tidak ada blok bebas yang cukup besar. Lihat 'mem' dan 'memmap'.\n\n");
        return;
    }

    u8* bytes = static_cast<u8*>(p);
    for (int i = 0; i < n; ++i)
        bytes[i] = 0xAA;                            // bukti memori hasil alokasi bisa dipakai

    alloc_handles[slot] = p;
    print("Alokasi id ");
    print_uint(static_cast<u32>(slot));
    print(": ");
    print_uint(static_cast<u32>(n));
    print(" byte di offset ");
    print_uint(mem_offset(p));
    print("\n\n");
}

static void free_cmd(const char* arg) {
    int id = parse_number(arg);
    if (id < 0 || id >= MAX_HANDLES || !alloc_handles[id]) {
        print("Id alokasi tidak ditemukan. Penggunaan: free <id>\n\n");
        return;
    }

    int result = kfree(alloc_handles[id]);
    alloc_handles[id] = nullptr;

    if (result == MEM_FREE_OK) {
        print("Alokasi id ");
        print_uint(static_cast<u32>(id));
        print(" dibebaskan.\n\n");
    } else {
        print("Gagal membebaskan: blok tidak valid.\n\n");
    }
}

static void policy_cmd(const char* arg) {
    if (equals(arg, "first")) {
        mem_set_policy(MEM_FIRST_FIT);
        print("Kebijakan alokasi: first-fit.\n\n");
    } else if (equals(arg, "best")) {
        mem_set_policy(MEM_BEST_FIT);
        print("Kebijakan alokasi: best-fit.\n\n");
    } else {
        print("Penggunaan: mempolicy first | mempolicy best\n\n");
    }
}

// Demo: fragmentasi lalu coalescing.
static void demo_memtest() {
    static const u32 sizes[6] = { 512, 1024, 512, 1024, 512, 1024 };
    void* p[6];

    print("Demo fragmentasi dan coalescing.\n");
    print("1) Alokasi 6 blok: 512, 1024, 512, 1024, 512, 1024 byte.\n");
    for (int i = 0; i < 6; ++i)
        p[i] = kmalloc(sizes[i]);

    print("2) Bebaskan blok 1 dan 3 (tidak bersebelahan) -> terbentuk 2 lubang.\n");
    kfree(p[1]);
    kfree(p[3]);
    show_memmap();

    print("3) Bebaskan blok 2 -> lubang bertetangga digabung (coalescing).\n");
    kfree(p[2]);
    show_memmap();

    print("4) Bebaskan sisanya -> heap kembali menjadi satu blok bebas.\n");
    kfree(p[0]);
    kfree(p[4]);
    kfree(p[5]);
    show_mem();
}

// ---------- Deadlock ----------
static Mutex dl_a = {0, -1};
static Mutex dl_b = {0, -1};
static bool dl_ordered = false;

static void dl_body(Mutex* first, Mutex* second) {
    mutex_lock(first);
    busy_wait_ticks(20);            // beri waktu thread lain mengunci mutex pertamanya
    mutex_lock(second);
    busy_wait_ticks(5);
    mutex_unlock(second);
    mutex_unlock(first);
}

static void dl_task_1() { dl_body(&dl_a, &dl_b); }
static void dl_task_2() {
    if (dl_ordered) dl_body(&dl_a, &dl_b);   // pencegahan: urutan kunci sama (A lalu B)
    else            dl_body(&dl_b, &dl_a);   // penyebab: urutan terbalik (B lalu A)
}

static void print_task_ref(int id) {
    TaskInfo ti;
    if (task_get_info(id, &ti))
        print(ti.name);
    print("(id ");
    print_uint(static_cast<u32>(id));
    print(")");
}

static void report_deadlock() {
    int cycle[MAX_TASKS];
    int n = sched_find_deadlock(cycle, MAX_TASKS);

    if (n == 0) {
        print("Tidak ada deadlock terdeteksi.\n\n");
        return;
    }

    print_colored("DEADLOCK TERDETEKSI!\n", COLOR_RED);
    print("Siklus tunggu: ");
    for (int i = 0; i <= n; ++i) {
        print_task_ref(cycle[i % n]);
        if (i < n) print(" -> ");
    }
    print("\n");
    print("Tiap thread menunggu mutex yang dipegang thread berikutnya, jadi tidak ada yang bisa maju.\n");
    print("Jalankan 'threads' untuk melihat status BLOCKED, lalu 'deadlock recover'.\n\n");
}

// Pemulihan: pilih korban (id terbesar = thread termuda), hentikan, lepas semua mutex-nya.
static void recover_deadlock() {
    int cycle[MAX_TASKS];
    int n = sched_find_deadlock(cycle, MAX_TASKS);

    if (n == 0) {
        print("Tidak ada deadlock untuk dipulihkan.\n\n");
        return;
    }

    int victim = cycle[0];
    for (int i = 1; i < n; ++i)
        if (cycle[i] > victim) victim = cycle[i];

    print("Korban dipilih: ");
    print_task_ref(victim);
    print("\n");

    int result = task_force_kill(victim);
    if (result == KILL_OK) {
        print_colored("Pulih.", COLOR_GREEN);
        print(" Korban dihentikan dan semua mutex miliknya dilepas.\n");
        print("Thread lain kini bisa melanjutkan. Jalankan 'threads' untuk melihat hasilnya.\n\n");
    } else {
        print("Gagal memulihkan: korban tidak boleh dihentikan (thread main atau sedang berjalan).\n\n");
    }
}

static void demo_deadlock(bool ordered) {
    if (!sched_all_done(1)) {
        print("Masih ada thread aktif atau terblokir. Selesaikan dulu ('threads', 'kill <id>', 'deadlock recover').\n\n");
        return;
    }

    dl_a.locked = 0; dl_a.owner = -1;
    dl_b.locked = 0; dl_b.owner = -1;
    dl_ordered = ordered;

    if (ordered)
        print("Mode: PENCEGAHAN. D1 dan D2 sama-sama mengunci A lalu B (urutan kunci seragam).\n");
    else
        print("Mode: PENYEBAB. D1 mengunci A lalu B, D2 mengunci B lalu A.\n");
    print("Menjalankan... (maks. 3 detik)\n");

    task_create("D1", dl_task_1);
    task_create("D2", dl_task_2);

    u32 start = sched_ticks();
    while (!sched_all_done(1) && sched_ticks() - start < 3 * TIMER_HZ)
        task_yield();

    if (sched_all_done(1))
        print("Kedua thread selesai tanpa deadlock.\n\n");
    else
        report_deadlock();
}

static void shell() {
    char input[128];

    print("Welcome to A2OS!\n");
    print("Ketik 'help' untuk melihat daftar perintah.\n\n");

    while (true) {
        print(current_is_admin ? "A2OS[ADMIN]> " : "A2OS[USER]> ");
        read_line(input, 128);

        if (equals(input, "help")) {
            show_help();
        } else if (equals(input, "about")) {
            show_about();
        } else if (equals(input, "diagnosis")) {
            run_diagnosis();
        } else if (equals(input, "threads") || equals(input, "ps")) {
            show_threads();
        } else if (equals(input, "spawn")) {
            spawn_worker();
        } else if (equals(input, "kill")) {
            kill_thread("");
        } else if (input[0] == 'k' && input[1] == 'i' && input[2] == 'l' &&
                   input[3] == 'l' && input[4] == ' ') {
            kill_thread(input + 5);
        } else if (equals(input, "menu")) {
            return;                                 // kembali ke main_menu()
        } else if (equals(input, "sched")) {
            demo_sched();
        } else if (equals(input, "race")) {
            demo_race(false);
        } else if (equals(input, "race lock")) {
            demo_race(true);
        } else if (equals(input, "prodcons")) {
            demo_prodcons();
        } else if (equals(input, "mem")) {
            show_mem();
        } else if (equals(input, "memmap")) {
            show_memmap();
        } else if (equals(input, "memtest")) {
            demo_memtest();
        } else if (equals(input, "mempolicy")) {
            policy_cmd("");
        } else if (starts_with(input, "mempolicy ")) {
            policy_cmd(input + 10);
        } else if (equals(input, "alloc")) {
            alloc_cmd("");
        } else if (starts_with(input, "alloc ")) {
            alloc_cmd(input + 6);
        } else if (equals(input, "free")) {
            free_cmd("");
        } else if (starts_with(input, "free ")) {
            free_cmd(input + 5);
        } else if (equals(input, "deadlock")) {
            demo_deadlock(false);
        } else if (equals(input, "deadlock order")) {
            demo_deadlock(true);
        } else if (equals(input, "deadlock check")) {
            report_deadlock();
        } else if (equals(input, "deadlock recover")) {
            recover_deadlock();
        } else if (equals(input, "ls")) {
            vfs_list();
        } else if (equals(input, "create")) {
            vfs_create();
        } else if (equals(input, "cat")) {
            vfs_read();
        } else if (equals(input, "write")) {
            vfs_write();
        } else if (equals(input, "rm")) {
            vfs_delete();
        } else if (equals(input, "whoami")) {
            print(current_is_admin ? "Akun aktif: ADMIN\n\n" : "Akun aktif: USER\n\n");
        } else if (equals(input, "logout")) {
            login();
            clear_screen();
            print_logo();
            print("Ketik 'help' untuk melihat perintah.\n\n");
        } else if (equals(input, "clear")) {
            clear_screen();
            print_logo();
            print("Ketik 'help' untuk melihat perintah.\n\n");
        } else if (equals(input, "echo")) {
            print("\n");
        } else if (input[0] == 'e' && input[1] == 'c' &&
                input[2] == 'h' && input[3] == 'o' &&
                input[4] == ' ') {
            print(input + 5);
            print("\n\n");
        } else if (equals(input, "shutdown") || equals(input, "exit")) {
            shutdown_qemu();
        } else if (input[0] == '\0') {
            // Abaikan baris kosong.
        } else {
            print("Perintah tidak dikenal. Ketik 'help'.\n\n");
        }
    }
}

static void main_menu() {
    while (true) {
        clear_screen();
        print_logo();
        print("        ASISTEN DIAGNOSIS TEKNISI\n\n");
        print("  1. Mulai diagnosis perangkat\n");
        print("  2. Terminal A2OS\n");
        print("  3. Tentang A2OS\n");
        print("  4. Matikan A2OS\n\n");

        int choice = read_number("Pilih menu: ");

        if (choice == 1) {
            run_diagnosis();
        } else if (choice == 2) {
            clear_screen();
            print_logo();
            shell();
        } else if (choice == 3) {
            clear_screen();
            print_logo();
            show_about();
            print("Tekan Enter untuk kembali...");
            char wait[4];
            read_line(wait, 4);
        } else if (choice == 4) {
            shutdown_qemu();
        } else {
            print("Pilihan tidak valid. Tekan Enter untuk mencoba lagi...");
            char wait[4];
            read_line(wait, 4);
        }
    }
}

extern "C" void kernel_main() {
    clear_screen();
    enable_cursor();

    // Aktifkan IDT, PIC, PIT, dan penjadwal preemptive (tugas utama = id 0).
    mem_init();            // siapkan heap kernel (kmalloc/kfree)
    sched_init();

    // Login ditampilkan pertama kali setelah kernel siap.
    // Menu utama baru dibuka setelah username dan password valid.
    login();
    main_menu();

    for (;;)
        asm volatile ("cli; hlt");
}