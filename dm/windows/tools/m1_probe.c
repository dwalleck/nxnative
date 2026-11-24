// Loader and dispatcher for the synthetic M1 fixture only, not for game images.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static __declspec(thread) struct {
    uint64_t x18, tpidr, tpidrro;
    void (*gate)(void);
    unsigned calls;
} slots;
extern unsigned long _tls_index; // PE TLS index provided by the MSVC CRT/linker.
void m1_gate(void);
void m1_clobber_volatile(void);

#define ENTRY 0x100
#define PAGE 0x1000
static unsigned char *image;
static struct { unsigned ret, svc; } gates[2];

static void fail(const char *message) {
    fprintf(stderr, "M1: %s\n", message);
    exit(1);
}

void m1_dispatch(uintptr_t ret, uint64_t *regs) {
    uintptr_t offset = ret - (uintptr_t)image;
    unsigned i = slots.calls++;
    if (i >= ARRAYSIZE(gates) || offset != gates[i].ret || regs[9] != 0x123 ||
        slots.tpidr != slots.tpidrro + 3 || slots.x18 != slots.tpidrro)
        fail("gate did not resolve to the expected return address and thread state");
    regs[0] += gates[i].svc;
    m1_clobber_volatile();
}

static DWORD WINAPI run_guest(void *argument) {
    typedef uint64_t (*guest_fn)(uint64_t, uint64_t);
    uint64_t seed = (uintptr_t)argument;
    if (slots.x18 || slots.tpidr || slots.tpidrro || slots.gate || slots.calls) fail("thread-local state was not clean");
    slots.gate = m1_gate;
    slots.tpidrro = seed;
    // Convert through a union to keep MSVC's object/function pointer warning enabled.
    union { void *pointer; guest_fn call; } entry = { .pointer = image + ENTRY };
    uint64_t result = entry.call(seed, seed);
    if (slots.calls != ARRAYSIZE(gates) || result != seed + 0x42 + 0x43)
        fail("guest state or return value was not preserved");
    return 0;
}

static FILE *open_read(const char *path, const char *mode) {
    FILE *f;
    if (fopen_s(&f, path, mode)) fail(path);
    return f;
}

static void set_keys(unsigned page, const unsigned *idx) {
    // Matches the compiler's ARM64 PE TLS sequence: TEB + 0x58 is the TLS vector.
    unsigned char **tls;
    memcpy(&tls, (const unsigned char *)NtCurrentTeb() + 0x58, sizeof(tls));
    uintptr_t base = (uintptr_t)tls[_tls_index];
    const uint64_t values[] = { (uintptr_t)&slots.x18 - base, (uintptr_t)&slots.tpidr - base,
        (uintptr_t)&slots.tpidrro - base, (uintptr_t)&slots.gate - base, (uint64_t)_tls_index * 8 };
    for (unsigned i = 0; i < ARRAYSIZE(values); ++i) {
        if (idx[i] >= PAGE / sizeof(uint64_t) || (i < 4 && values[i] > 0xffff)) fail("bad TLS slot offset");
        memcpy(image + page + idx[i] * sizeof(uint64_t), &values[i], sizeof(uint64_t));
    }
}

int main(int argc, char **argv) {
    USHORT process, native;
    if (!IsWow64Process2(GetCurrentProcess(), &process, &native) ||
        process != IMAGE_FILE_MACHINE_UNKNOWN || native != IMAGE_FILE_MACHINE_ARM64)
        fail("not a native ARM64 process");
    if (argc != 4) fail("usage: m1_probe <patched.bin> <fixups.tsv> <gates.tsv>");
    FILE *f = open_read(argv[2], "r");
    unsigned patch = 0, key = 0, key_size = 0, text_end = 0, ro = 0, data = 0, idx[5];
    char line[256];
    if (!fgets(line, sizeof(line), f) || strcmp(line, "# TARGET\twindows-arm64\n") ||
        fscanf_s(f, "# PATCH_OFF\t%x\n# KEY_PAGE_OFF\t%x\n# KEY_PAGE_SIZE\t%x\n# TEXT_END\t%x\n# RO_OFF\t%x\n# DATA_OFF\t%x\n",
                 &patch, &key, &key_size, &text_end, &ro, &data) != 6 ||
        fscanf_s(f, "# KEY_K18_IDX\t%x\n# KEY_KTLS_IDX\t%x\n# KEY_KTLR_IDX\t%x\n# KEY_KGATE_IDX\t%x\n# KEY_WIN_TLS_IDX\t%x\n",
                 &idx[0], &idx[1], &idx[2], &idx[3], &idx[4]) != 5)
        fail("incompatible fixture fixups.tsv");
    fclose(f);
    // The rewriter owns the layout and moves writable data onto a 16 KB page of its own (dm/tools/mod_layout.py), so the key
    // page follows the image instead of sitting at a fixed offset. Check what the key filling and the sealing below need, and
    // the shared rule that data starts at or past the code's first 16 KB page: a module whose data still shares the code page
    // would be sealed read-only by the Apple host (docs/reference/memory.md#module-data-on-the-codes-16-kb-page).
    if (key_size != 0x4000 || (key & 0x3fff) || ro < text_end || data < ((text_end + 0x3fff) & ~0x3fffu) ||
        key < data || patch < key + key_size || (patch & (PAGE - 1)))
        fail("unexpected fixture layout");

    f = open_read(argv[1], "rb");
    if (fseek(f, 0, SEEK_END)) fail("cannot size image");
    long end = ftell(f);
    if (end <= (long)patch || end > 0x11000 || fseek(f, 0, SEEK_SET)) fail("bad fixture image size");
    size_t size = (size_t)end;
    image = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!image || fread(image, 1, size, f) != size) fail("cannot load image");
    fclose(f);

    f = open_read(argv[3], "r");
    unsigned orig;
    for (unsigned i = 0; i < ARRAYSIZE(gates); ++i) {
        if (fscanf_s(f, "%x\tsvc\t%u\t%x\n", &gates[i].ret, &gates[i].svc, &orig) != 3 ||
            gates[i].ret < patch || gates[i].ret >= size || gates[i].svc != 0x42 + i || orig < ENTRY || orig >= PAGE)
            fail("bad SVC gate table");
    }
    if (fgetc(f) != EOF) fail("unexpected additional gate");
    fclose(f);
    set_keys(key, idx);
    DWORD old;
    if (!VirtualProtect(image, PAGE, PAGE_EXECUTE_READ, &old) ||
        !VirtualProtect(image + PAGE, patch - PAGE, PAGE_READONLY, &old) ||
        !VirtualProtect(image + patch, size - patch, PAGE_EXECUTE_READ, &old) ||
        !FlushInstructionCache(GetCurrentProcess(), image, size)) fail("cannot seal code and key page");

    HANDLE worker = CreateThread(NULL, 0, run_guest, (void *)(uintptr_t)0x3344, 0, NULL);
    if (!worker) fail("CreateThread failed");
    run_guest((void *)(uintptr_t)0x1122);
    if (WaitForSingleObject(worker, 10000) != WAIT_OBJECT_0) fail("guest worker did not finish");
    CloseHandle(worker);
    if (!VirtualFree(image, 0, MEM_RELEASE)) fail("VirtualFree failed");
    puts("M1 native ARM64: 2 SVC sites resolved on 2 threads; GPRs, SIMD, NZCV, FP state and TLS verified");
    return 0;
}
