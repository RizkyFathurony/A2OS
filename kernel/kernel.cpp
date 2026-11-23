using u8  = unsigned char;
using u16 = unsigned short;
using u32 = unsigned int;

static volatile u16* const VGA = reinterpret_cast<volatile u16*>(0xB8000);
static const u8 COLOR = 0x0F;
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
    if (cursor_x > 79) cursor_x = 79;
    if (cursor_y < 0) cursor_y = 0;
    if (cursor_y > 24) cursor_y = 24;

    u16 position = static_cast<u16>(cursor_y * 80 + cursor_x);

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
            (static_cast<u16>(COLOR) << 8) | static_cast<u8>(c);
        ++cursor_x;
    }

    if (cursor_y >= 25)
        scroll_screen();

    update_cursor();
}

static void print(const char* s) {
    while (*s)
        put_char(*s++);
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

static int read_number(const char* prompt) {
    char input[16];

    print(prompt);
    read_line(input, 16);

    if (input[0] >= '0' && input[0] <= '9')
        return input[0] - '0';

    return -1;
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
    print("  clear     - membersihkan layar\n");
    print("  echo      - menampilkan teks\n");
    print("  ls        - melihat daftar file RAMDisk\n");
    print("  create    - membuat file teks\n");
    print("  cat       - membaca isi file\n");
    print("  write     - mengubah isi file\n");
    print("  rm        - menghapus file\n");
    print("  whoami    - melihat akun aktif\n");
    print("  logout    - keluar dari akun\n");
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
            print(current_is_admin ? "Akun aktif: ADMIN\\n\\n" : "Akun aktif: USER\\n\\n");
        } else if (equals(input, "logout")) {
            login();
            clear_screen();
            print_logo();
            print("Ketik 'help' untuk melihat perintah.\\n\\n");
        } else if (equals(input, "clear")) {
            clear_screen();
            print_logo();
            print("Ketik 'help' untuk melihat perintah.\n\n");
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

    // Login ditampilkan pertama kali setelah kernel siap.
    // Menu utama baru dibuka setelah username dan password valid.
    login();
    main_menu();

    for (;;)
        asm volatile ("cli; hlt");
}
