// One short ARM64 process-wide trace. PSS collects control contexts without
// this process suspending threads, attaching a debugger, or cloning memory.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define _NO_CVCONST_H
#include <dbghelp.h>
#include <processsnapshot.h>
#include <tlhelp32.h>
#include <limits.h>
#include <locale.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#if !defined(_M_ARM64)
#error Build m2_cpu_trace as native ARM64 to read ARM64 thread contexts.
#endif

enum { DEFAULT_SAMPLES = 12, DEFAULT_INTERVAL_MS = 100, MAX_SAMPLES = 32, MAX_INTERVAL_MS = 250 };

struct codeview {
    DWORD signature;
    GUID guid;
    DWORD age;
};

struct sample {
    DWORD tid;
    BOOL terminated;
    ULONGLONG created, captured, user, kernel, pc;
};

struct snapshot {
    size_t first, count;
    ULONGLONG elapsed_ms, capture_ms;
    DWORD error;
    const char *where;
};

struct trace {
    struct snapshot snapshots[MAX_SAMPLES];
    struct sample *samples;
    size_t count, capacity;
    unsigned attempted, complete;
};

struct modules {
    MODULEENTRY32W *items;
    size_t count, capacity;
};

static ULONGLONG filetime_value(FILETIME t) {
    return ((ULONGLONG)t.dwHighDateTime << 32) | t.dwLowDateTime;
}

static BOOL parse_number(const wchar_t *text, ULONGLONG limit, ULONGLONG *out) {
    ULONGLONG value = 0;
    if (!*text) return FALSE;
    for (; *text; ++text) {
        if (*text < L'0' || *text > L'9') return FALSE;
        const unsigned digit = (unsigned)(*text - L'0');
        if (value > (limit - digit) / 10) return FALSE;
        value = value * 10 + digit;
    }
    *out = value;
    return TRUE;
}

static void *reserve(void *data, size_t *capacity, size_t need, size_t width) {
    if (need <= *capacity) return data;
    if (need > SIZE_MAX / width) return NULL;
    size_t n = *capacity ? *capacity : 16;
    while (n < need) {
        if (n > SIZE_MAX / width / 2) {
            n = need;
            break;
        }
        n *= 2;
    }
    void *next = realloc(data, n * width);
    if (next) *capacity = n;
    return next;
}

static DWORD read_modules(DWORD pid, struct modules *modules) {
    HANDLE snapshot = INVALID_HANDLE_VALUE;
    DWORD error = ERROR_BAD_LENGTH;
    // Windows requests a retry if the loader changed its list during capture.
    for (unsigned i = 0; i < 3 && error == ERROR_BAD_LENGTH; ++i) {
        snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
        if (snapshot != INVALID_HANDLE_VALUE) break;
        error = GetLastError();
    }
    if (snapshot == INVALID_HANDLE_VALUE) return error;
    MODULEENTRY32W entry = {.dwSize = sizeof entry};
    BOOL more = Module32FirstW(snapshot, &entry);
    while (more) {
        MODULEENTRY32W *next = reserve(modules->items, &modules->capacity, modules->count + 1, sizeof entry);
        if (!next) {
            CloseHandle(snapshot);
            return ERROR_OUTOFMEMORY;
        }
        modules->items = next;
        modules->items[modules->count++] = entry;
        more = Module32NextW(snapshot, &entry);
    }
    error = GetLastError();
    CloseHandle(snapshot);
    return error == ERROR_NO_MORE_FILES && modules->count ? ERROR_SUCCESS : error;
}

static ULONGLONG module_base(const MODULEENTRY32W *module) {
    return (ULONGLONG)(ULONG_PTR)module->modBaseAddr;
}

static const MODULEENTRY32W *module_at(const struct modules *modules, ULONGLONG pc) {
    for (size_t i = 0; i < modules->count; ++i) {
        const MODULEENTRY32W *m = &modules->items[i];
        if (pc >= module_base(m) && pc - module_base(m) < m->modBaseSize) return m;
    }
    return NULL;
}

static const MODULEENTRY32W *main_module(const struct modules *modules, const wchar_t *path) {
    for (size_t i = 0; i < modules->count; ++i)
        if (CompareStringOrdinal(modules->items[i].szExePath, -1, path, -1, TRUE) == CSTR_EQUAL)
            return &modules->items[i];
    return NULL;
}

static BOOL read_image(HANDLE process, const MODULEENTRY32W *module, size_t offset, void *out, size_t size) {
    if (offset > module->modBaseSize || size > module->modBaseSize - offset) {
        SetLastError(ERROR_BAD_EXE_FORMAT);
        return FALSE;
    }
    return ReadProcessMemory(process, (const void *)(ULONG_PTR)(module_base(module) + offset), out, size, NULL);
}

// Read the debug identity from the *running* image, not an EXE that may have
// been replaced on disk since process creation. RSDS carries the GUID and age.
static DWORD read_codeview(HANDLE process, const MODULEENTRY32W *module, struct codeview *cv) {
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt;
    if (!read_image(process, module, 0, &dos, sizeof dos)) return GetLastError();
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) return ERROR_BAD_EXE_FORMAT;
    if (!read_image(process, module, (size_t)dos.e_lfanew, &nt, sizeof nt)) return GetLastError();
    if (nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_ARM64 ||
        nt.FileHeader.SizeOfOptionalHeader < sizeof nt.OptionalHeader ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DEBUG) return ERROR_BAD_EXE_FORMAT;
    const IMAGE_DATA_DIRECTORY dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (!dir.VirtualAddress || !dir.Size) return ERROR_NOT_FOUND;
    if (dir.VirtualAddress > module->modBaseSize || dir.Size > module->modBaseSize - dir.VirtualAddress ||
        dir.Size % sizeof(IMAGE_DEBUG_DIRECTORY)) return ERROR_BAD_EXE_FORMAT;
    for (size_t off = 0; off < dir.Size; off += sizeof(IMAGE_DEBUG_DIRECTORY)) {
        IMAGE_DEBUG_DIRECTORY debug;
        if (!read_image(process, module, dir.VirtualAddress + off, &debug, sizeof debug)) return GetLastError();
        if (debug.Type != IMAGE_DEBUG_TYPE_CODEVIEW || debug.SizeOfData < sizeof *cv || !debug.AddressOfRawData)
            continue;
        if (!read_image(process, module, debug.AddressOfRawData, cv, sizeof *cv)) return GetLastError();
        if (cv->signature == 0x53445352U) return ERROR_SUCCESS; // "RSDS"
    }
    return ERROR_NOT_FOUND;
}

static void print_guid(const GUID *g) {
    printf("%08lx-%04x-%04x-%02x%02x-", g->Data1, (unsigned)g->Data2, (unsigned)g->Data3,
           (unsigned)g->Data4[0], (unsigned)g->Data4[1]);
    for (unsigned i = 2; i < 8; ++i) printf("%02x", (unsigned)g->Data4[i]);
}

static BOOL symbol_error(const char *reason, DWORD error) {
    printf("HOST_SYMBOLS status=error reason=%s win32=%lu\n", reason, error);
    return FALSE;
}

static BOOL load_host_symbols(HANDLE process, const MODULEENTRY32W *host, const struct codeview *cv,
                              BOOL *initialized) {
    wchar_t pdb[MAX_PATH], directory[MAX_PATH];
    const wchar_t *slash = wcsrchr(host->szExePath, L'\\');
    if (!slash) return symbol_error("host_path_invalid", ERROR_BAD_PATHNAME);
    const wchar_t *extension = wcsrchr(slash + 1, L'.');
    const size_t len = extension ? (size_t)(extension - host->szExePath) : wcslen(host->szExePath);
    if (len + 5 > ARRAYSIZE(pdb)) return symbol_error("pdb_path_too_long", ERROR_FILENAME_EXCED_RANGE);
    const size_t dir_len = (size_t)(slash - host->szExePath) + 1;
    wmemcpy(directory, host->szExePath, dir_len);
    directory[dir_len] = L'\0';
    wmemcpy(pdb, host->szExePath, len);
    wmemcpy(pdb + len, L".pdb", 5);

    // Only the adjacent file may be opened for symbolization. Keep it pinned
    // against replacement while indexing it and loading DbgHelp's own handle.
    HANDLE file = CreateFileW(pdb, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        printf("HOST_SYMBOLS status=error reason=open_adjacent_pdb win32=%lu pdb=\"%ls\"\n", error, pdb);
        return FALSE;
    }
    SymSetOptions(SYMOPT_EXACT_SYMBOLS | SYMOPT_UNDNAME | SYMOPT_IGNORE_CVREC | SYMOPT_IGNORE_NT_SYMPATH |
                  SYMOPT_NO_IMAGE_SEARCH | SYMOPT_NO_PROMPTS | SYMOPT_FAIL_CRITICAL_ERRORS |
                  SYMOPT_DISABLE_SYMSRV_AUTODETECT);
    SYMSRV_INDEX_INFOW index = {.sizeofstruct = sizeof index};
    BOOL matched = FALSE;
    if (!SymSrvGetFileIndexInfoW(pdb, &index, 0)) {
        symbol_error("pdb_index_failed", GetLastError());
    } else if (memcmp(&cv->guid, &index.guid, sizeof cv->guid) || cv->age != index.age) {
        printf("HOST_SYMBOLS status=error reason=pdb_mismatch pdb=\"%ls\" expected_guid=", pdb);
        print_guid(&cv->guid);
        printf(" expected_age=%lu actual_guid=", cv->age);
        print_guid(&index.guid);
        printf(" actual_age=%lu\n", index.age);
    } else if (!SymInitializeW(process, directory, FALSE)) {
        symbol_error("SymInitializeW", GetLastError());
    } else {
        *initialized = TRUE;
        const DWORD64 base = module_base(host);
        IMAGEHLP_MODULEW64 info = {.SizeOfStruct = sizeof info};
        if (SymLoadModuleExW(process, NULL, pdb, NULL, base, host->modBaseSize, NULL, 0) != base)
            symbol_error("SymLoadModuleExW", GetLastError());
        else if (!SymGetModuleInfoW64(process, base, &info))
            symbol_error("SymGetModuleInfoW64", GetLastError());
        else if (info.SymType != SymPdb || info.PdbUnmatched || info.DbgUnmatched ||
                 memcmp(&cv->guid, &info.PdbSig70, sizeof cv->guid) || cv->age != info.PdbAge ||
                 CompareStringOrdinal(info.LoadedPdbName, -1, pdb, -1, TRUE) != CSTR_EQUAL) {
            printf("HOST_SYMBOLS status=error reason=loaded_pdb_not_verified sym_type=%u pdb_unmatched=%d"
                   " dbg_unmatched=%d loaded_pdb=\"%ls\" loaded_guid=",
                   (unsigned)info.SymType, info.PdbUnmatched, info.DbgUnmatched, info.LoadedPdbName);
            print_guid(&info.PdbSig70);
            printf(" loaded_age=%lu\n", info.PdbAge);
        } else {
            printf("HOST_SYMBOLS status=matched image=\"%ls\" pdb=\"%ls\" base=0x%llx size=%lu guid=",
                   host->szExePath, pdb, base, host->modBaseSize);
            print_guid(&cv->guid);
            printf(" age=%lu\n", cv->age);
            matched = TRUE;
        }
    }
    CloseHandle(file);
    return matched;
}

static int sample_order(const void *left, const void *right) {
    const struct sample *a = left, *b = right;
    if (a->tid != b->tid) return a->tid > b->tid ? 1 : -1;
    return (a->created > b->created) - (a->created < b->created);
}

static void capture(HANDLE process, DWORD pid, struct trace *trace, struct snapshot *snapshot) {
    HPSS pss = NULL;
    HPSSWALK marker = NULL;
    PSS_THREAD_INFORMATION info;
    DWORD error;
    const char *where = "PssCaptureSnapshot";
    const ULONGLONG start = GetTickCount64();
    snapshot->first = trace->count;
    error = PssCaptureSnapshot(process, PSS_CAPTURE_THREADS | PSS_CAPTURE_THREAD_CONTEXT, CONTEXT_CONTROL, &pss);
    snapshot->capture_ms = GetTickCount64() - start;
    if (error) goto done;
    where = "PssQuerySnapshot";
    error = PssQuerySnapshot(pss, PSS_QUERY_THREAD_INFORMATION, &info, sizeof info);
    if (error) goto done;
    if (!info.ThreadsCaptured) {
        where = "no_threads_captured";
        error = ERROR_NOT_FOUND;
        goto done;
    }
    where = "sample_storage";
    struct sample *next = reserve(trace->samples, &trace->capacity, trace->count + info.ThreadsCaptured,
                                  sizeof *trace->samples);
    if (!next) {
        error = ERROR_OUTOFMEMORY;
        goto done;
    }
    trace->samples = next;
    where = "PssWalkMarkerCreate";
    error = PssWalkMarkerCreate(NULL, &marker);
    if (error) goto done;
    where = "PssWalkSnapshot";
    for (;;) {
        PSS_THREAD_ENTRY entry;
        error = PssWalkSnapshot(pss, PSS_WALK_THREADS, marker, &entry, sizeof entry);
        if (error == ERROR_NO_MORE_ITEMS) {
            error = snapshot->count == info.ThreadsCaptured ? ERROR_SUCCESS : ERROR_INVALID_DATA;
            break;
        }
        if (error) break;
        if (snapshot->count == info.ThreadsCaptured || entry.ProcessId != pid) {
            error = ERROR_INVALID_DATA;
            break;
        }
        struct sample *sample = &trace->samples[trace->count++];
        ++snapshot->count;
        *sample = (struct sample){.tid = entry.ThreadId, .terminated = (entry.Flags & PSS_THREAD_FLAGS_TERMINATED) != 0,
            .created = filetime_value(entry.CreateTime), .captured = filetime_value(entry.CaptureTime),
            .user = filetime_value(entry.UserTime), .kernel = filetime_value(entry.KernelTime)};
        if (entry.ContextRecord && entry.SizeOfContextRecord >= offsetof(CONTEXT, Pc) + sizeof entry.ContextRecord->Pc &&
            (entry.ContextRecord->ContextFlags & CONTEXT_CONTROL) == CONTEXT_CONTROL)
            sample->pc = entry.ContextRecord->Pc;
    }

done:
    if (marker) {
        const DWORD freed = PssWalkMarkerFree(marker);
        if (!error && freed) {
            where = "PssWalkMarkerFree";
            error = freed;
        }
    }
    if (pss) {
        const DWORD freed = PssFreeSnapshot(GetCurrentProcess(), pss);
        if (!error && freed) {
            where = "PssFreeSnapshot";
            error = freed;
        }
    }
    if (snapshot->count > 1)
        qsort(trace->samples + snapshot->first, snapshot->count, sizeof *trace->samples, sample_order);
    snapshot->error = error;
    snapshot->where = where;
}

struct totals {
    size_t active, no_cpu, unknown, context_missing, counter_regressions, host_pc, host_functions;
};

static void print_location(HANDLE process, const struct modules *modules, const MODULEENTRY32W *host,
                           BOOL matched, const struct sample *sample, struct totals *totals) {
    if (!sample->pc) {
        if (!sample->terminated) ++totals->context_missing;
        printf(" pc=- module=- rva=- function=- symbols=%s", sample->terminated ? "thread_exited" : "no_context");
        return;
    }
    printf(" pc=0x%llx", sample->pc);
    const MODULEENTRY32W *module = module_at(modules, sample->pc);
    if (!module) {
        printf(" module=- rva=- function=- symbols=outside_initial_module_map");
        return;
    }
    printf(" module=\"%ls\" rva=0x%llx", module->szModule, sample->pc - module_base(module));
    if (module != host) {
        printf(" function=- symbols=no_local_symbols_for_module");
        return;
    }
    ++totals->host_pc;
    if (!matched) {
        printf(" function=- symbols=host_pdb_unavailable");
        return;
    }

    struct { SYMBOL_INFO symbol; char tail[MAX_SYM_NAME - 1]; } buffer;
    SYMBOL_INFO *symbol = &buffer.symbol;
    symbol->SizeOfStruct = sizeof *symbol;
    symbol->MaxNameLen = MAX_SYM_NAME;
    DWORD64 offset;
    if (SymFromAddr(process, sample->pc, &offset, symbol) && symbol->ModBase == module_base(host)) {
        if ((symbol->Tag == SymTagFunction || (symbol->Flags & SYMFLAG_FUNCTION)) &&
            (symbol->Size ? offset < symbol->Size : !offset)) {
            printf(" function=\"%s\" function_offset=0x%llx symbols=pdb", symbol->Name, offset);
            ++totals->host_functions;
            return;
        }
        printf(" function=- symbols=pdb_no_function_range nearest_symbol=\"%s\" nearest_offset=0x%llx",
               symbol->Name, offset);
        return;
    }
    printf(" function=- symbols=no_pdb_symbol");
}

static struct totals print_samples(HANDLE process, const struct trace *trace, const struct modules *modules,
                                   const MODULEENTRY32W *host, BOOL matched) {
    struct totals totals = {0};
    for (unsigned i = 0; i < trace->attempted; ++i) {
        const struct snapshot *snapshot = &trace->snapshots[i];
        const struct snapshot *previous = i ? &trace->snapshots[i - 1] : NULL;
        size_t last = 0;
        for (size_t j = 0; j < snapshot->count; ++j) {
            const struct sample *sample = &trace->samples[snapshot->first + j];
            const struct sample *before = NULL;
            if (previous && sample->created) {
                while (last < previous->count) {
                    const struct sample *candidate = &trace->samples[previous->first + last];
                    const int order = sample_order(candidate, sample);
                    if (!order) before = candidate;
                    if (order >= 0) break;
                    ++last;
                }
            }
            printf("SAMPLE index=%u tid=%lu created_100ns=%llu captured_100ns=%llu thread=%s user_100ns=%llu kernel_100ns=%llu",
                   i + 1, sample->tid, sample->created, sample->captured, sample->terminated ? "exited" : "live",
                   sample->user, sample->kernel);
            if (!before || sample->user < before->user || sample->kernel < before->kernel) {
                if (before) ++totals.counter_regressions;
                ++totals.unknown;
                printf(" activity=unknown reason=%s user_delta_100ns=- kernel_delta_100ns=-",
                       before ? "cpu_counter_regressed" : "no_previous_observation");
            } else {
                const ULONGLONG user = sample->user - before->user, kernel = sample->kernel - before->kernel;
                if (user || kernel) ++totals.active;
                else ++totals.no_cpu;
                // The CPU interval is not time attributed to this sampled PC.
                // Zero means no measured CPU time, not proof of a blocked wait.
                printf(" activity=%s user_delta_100ns=%llu kernel_delta_100ns=%llu",
                       user || kernel ? "active" : "no_cpu_observed", user, kernel);
            }
            print_location(process, modules, host, matched, sample, &totals);
            putchar('\n');
        }
    }
    return totals;
}

static int early_error(const char *reason, DWORD error) {
    printf("TRACE_RESULT status=error reason=%s win32=%lu\n", reason, error);
    return 2;
}

int wmain(int argc, wchar_t **argv) {
    setlocale(LC_CTYPE, ".UTF-8");
    ULONGLONG pid = 0, expected_created = 0, samples = DEFAULT_SAMPLES, interval = DEFAULT_INTERVAL_MS;
    int arg = 2;
    if (argc < 2 || !parse_number(argv[1], MAXDWORD, &pid) || !pid) goto usage;
    if (arg < argc && !wcscmp(argv[arg], L"--created-100ns")) {
        if (++arg >= argc || !parse_number(argv[arg++], ULLONG_MAX, &expected_created) || !expected_created)
            goto usage;
    }
    if (arg < argc && (!parse_number(argv[arg++], MAX_SAMPLES, &samples) || samples < 2)) goto usage;
    if (arg < argc && (!parse_number(argv[arg++], MAX_INTERVAL_MS, &interval) || interval < 10)) goto usage;
    if (arg != argc) goto usage;

    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, FALSE, (DWORD)pid);
    if (!process) return early_error("OpenProcess", GetLastError());
    int result;
    FILETIME created, exited, kernel, user;
    USHORT machine, native;
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        result = early_error("GetProcessTimes", GetLastError());
        goto close;
    }
    const ULONGLONG actual_created = filetime_value(created);
    if (expected_created && expected_created != actual_created) {
        printf("TRACE_RESULT status=error reason=target_identity_mismatch pid=%llu expected_created_100ns=%llu actual_created_100ns=%llu\n",
               pid, expected_created, actual_created);
        result = 2;
        goto close;
    }
    if (!IsWow64Process2(process, &machine, &native)) {
        result = early_error("IsWow64Process2", GetLastError());
        goto close;
    }
    if (machine != IMAGE_FILE_MACHINE_UNKNOWN || native != IMAGE_FILE_MACHINE_ARM64) {
        printf("TRACE_RESULT status=error reason=target_not_native_arm64 machine=0x%x native=0x%x\n",
               (unsigned)machine, (unsigned)native);
        result = 2;
        goto close;
    }
    const DWORD alive = WaitForSingleObject(process, 0);
    if (alive != WAIT_TIMEOUT) {
        result = alive == WAIT_OBJECT_0 ? early_error("target_exited", ERROR_SUCCESS) :
                                         early_error("WaitForSingleObject", GetLastError());
        goto close;
    }
    printf("TRACE_BEGIN version=1 pid=%llu created_100ns=%llu requested=%llu interval_ms=%llu"
           " method=pss_thread_context pc_kind=user_instruction_pointer module_map=initial cpu_unit=100ns\n",
           pid, actual_created, samples, interval);
    fflush(stdout);

    wchar_t image[MAX_PATH];
    DWORD image_chars = ARRAYSIZE(image);
    struct modules modules = {0};
    const MODULEENTRY32W *host = NULL;
    struct codeview cv;
    DWORD cv_error = ERROR_NOT_FOUND;
    DWORD module_error = ERROR_SUCCESS;
    if (!QueryFullProcessImageNameW(process, 0, image, &image_chars)) {
        module_error = GetLastError();
        printf("TRACE_EVENT status=error reason=QueryFullProcessImageNameW win32=%lu\n", module_error);
    } else {
        module_error = read_modules((DWORD)pid, &modules);
        if (module_error) printf("TRACE_EVENT status=error reason=module_snapshot win32=%lu\n", module_error);
        if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
            host = main_module(&modules, image);
            if (host) cv_error = read_codeview(process, host, &cv);
        }
    }
    struct trace trace = {0};
    const ULONGLONG start = GetTickCount64();
    for (unsigned i = 0; i < (unsigned)samples; ++i) {
        const DWORD waited = WaitForSingleObject(process, i ? (DWORD)interval : 0);
        if (waited != WAIT_TIMEOUT) {
            printf("TRACE_EVENT status=error reason=%s win32=%lu before_snapshot=%u\n",
                   waited == WAIT_OBJECT_0 ? "target_exited" : "WaitForSingleObject",
                   waited == WAIT_OBJECT_0 ? ERROR_SUCCESS : GetLastError(), i + 1);
            break;
        }
        struct snapshot *snapshot = &trace.snapshots[trace.attempted++];
        snapshot->elapsed_ms = GetTickCount64() - start;
        capture(process, (DWORD)pid, &trace, snapshot);
        printf("SNAPSHOT index=%u status=%s elapsed_ms=%llu capture_ms=%llu threads=%zu",
               i + 1, snapshot->error ? "error" : "ok", snapshot->elapsed_ms, snapshot->capture_ms, snapshot->count);
        if (snapshot->error) printf(" reason=%s win32=%lu", snapshot->where, snapshot->error);
        else ++trace.complete;
        putchar('\n');
        fflush(stdout);
    }

    BOOL initialized = FALSE, matched = FALSE;
    if (!host) symbol_error("running_image_not_in_module_map", ERROR_NOT_FOUND);
    else if (cv_error) symbol_error("running_image_codeview_unavailable", cv_error);
    else matched = load_host_symbols(process, host, &cv, &initialized);
    const struct totals totals = print_samples(process, &trace, &modules, host, matched);
    const BOOL cleaned_up = !initialized || SymCleanup(process);
    if (!cleaned_up)
        printf("TRACE_EVENT status=error reason=SymCleanup win32=%lu\n", GetLastError());
    const char *evidence = !matched ? "symbols_unavailable" : !totals.host_pc ? "not_sampled" :
                           !totals.host_functions ? "unresolved" : "symbolized";
    const BOOL partial = !matched || module_error || !cleaned_up || trace.complete != samples ||
                         totals.context_missing || totals.counter_regressions ||
                         (totals.host_pc && !totals.host_functions);
    result = !trace.count ? 2 : partial ? 1 : 0;
    printf("TRACE_RESULT status=%s requested=%llu complete_snapshots=%u samples=%zu active_intervals=%zu"
           " no_cpu_intervals=%zu unknown_intervals=%zu context_missing=%zu counter_regressions=%zu"
           " host_pc_samples=%zu host_function_samples=%zu host_evidence=%s\n",
           result == 0 ? "ok" : result == 1 ? "partial" : "error", samples, trace.complete, trace.count,
           totals.active, totals.no_cpu, totals.unknown, totals.context_missing, totals.counter_regressions,
           totals.host_pc, totals.host_functions, evidence);
    free(trace.samples);
    free(modules.items);
close:
    CloseHandle(process);
    return result;

usage:
    fputs("Usage: m2_cpu_trace.exe <pid> [--created-100ns <decimal FILETIME>] [samples (2-32)] [interval_ms (10-250)]\n",
          stderr);
    return early_error("usage", ERROR_INVALID_PARAMETER);
}
