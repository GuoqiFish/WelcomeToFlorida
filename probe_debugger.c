#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>

/* One-shot observation breakpoints are restored before executing the original
 * instruction. Unexpected exceptions are recorded, never skipped. */
typedef struct { DWORD64 base, size; char path[2048]; } MODULE;
typedef struct { DWORD64 rva, address; BYTE original; int armed; char module[256]; } BREAKPOINT;
static MODULE modules[1024];
static unsigned module_count;
static BREAKPOINT breaks[64];
static unsigned break_count;
static DWORD threads[512];
static unsigned thread_count;
static FILE *output;
static HANDLE process;
static DWORD pid;
static ULONGLONG started;
static int symbols_ready;

static void log_line(const char *fmt, ...) {
    va_list ap;
    fprintf(output, "[%8llu ms] ", GetTickCount64() - started);
    va_start(ap, fmt); vfprintf(output, fmt, ap); va_end(ap);
    fputc('\n', output); fflush(output);
}
static int read_at(DWORD64 address, void *buffer, SIZE_T size) {
    SIZE_T got = 0;
    return ReadProcessMemory(process, (LPCVOID)address, buffer, size, &got) && got == size;
}
static int write_byte(DWORD64 address, BYTE value) {
    DWORD old = 0, unused; SIZE_T wrote = 0;
    if (!VirtualProtectEx(process, (LPVOID)address, 1, PAGE_EXECUTE_READWRITE, &old)) return 0;
    int ok = WriteProcessMemory(process, (LPVOID)address, &value, 1, &wrote) && wrote == 1;
    FlushInstructionCache(process, (LPCVOID)address, 1);
    VirtualProtectEx(process, (LPVOID)address, 1, old, &unused);
    return ok;
}
static MODULE *find_module(DWORD64 address) {
    for (unsigned i = 0; i < module_count; ++i)
        if (address >= modules[i].base && address - modules[i].base < modules[i].size) return &modules[i];
    return NULL;
}
static const char *filename(const char *path) {
    const char *name = strrchr(path, '\\'); return name ? name + 1 : path;
}
static void describe(DWORD64 address, char *result, size_t capacity) {
    MODULE *m = find_module(address);
    if (m) snprintf(result, capacity, "%s+0x%llx", filename(m->path), address - m->base);
    else snprintf(result, capacity, "0x%llx", address);
}
static void add_module(DWORD64 base, HANDLE file) {
    if (module_count >= 1024) return;
    MODULE *m = &modules[module_count++]; m->base = base;
    if (file) GetFinalPathNameByHandleA(file, m->path, sizeof(m->path), FILE_NAME_NORMALIZED);
    IMAGE_DOS_HEADER dos; IMAGE_NT_HEADERS64 nt;
    if (read_at(base, &dos, sizeof(dos)) && dos.e_magic == IMAGE_DOS_SIGNATURE &&
        read_at(base + dos.e_lfanew, &nt, sizeof(nt)) && nt.Signature == IMAGE_NT_SIGNATURE)
        m->size = nt.OptionalHeader.SizeOfImage;
    log_line("MODULE base=0x%llx size=0x%llx path=%s", base, m->size, m->path);
    if (symbols_ready) SymLoadModuleEx(process, file, m->path, NULL, base, (DWORD)m->size, NULL, 0);
    for (unsigned i = 0; i < break_count; ++i) {
        BREAKPOINT *b = &breaks[i];
        if (b->address || (b->module[0] ? _stricmp(b->module, filename(m->path)) != 0 : module_count != 1)) continue;
        b->address = base + b->rva;
        if (read_at(b->address, &b->original, 1) && b->original != 0xcc && write_byte(b->address, 0xcc)) {
            b->armed = 1; log_line("BREAK_ARMED module=%s rva=0x%llx original=%02x", filename(m->path), b->rva, b->original);
        } else log_line("BREAK_ARM_FAILED rva=0x%llx error=%lu", b->rva, GetLastError());
    }
}
static void add_thread(DWORD tid) {
    if (thread_count < 512) threads[thread_count++] = tid;
}
static void remove_thread(DWORD tid) {
    for (unsigned i = 0; i < thread_count; ++i)
        if (threads[i] == tid) { threads[i] = threads[--thread_count]; return; }
}
static void dump_context(DWORD tid, const CONTEXT *c) {
    char where[2200]; describe(c->Rip, where, sizeof(where));
    log_line("REG tid=%lu rip=%016llx (%s) rsp=%016llx rbp=%016llx eax=%08lx eflags=%08lx",
             tid, c->Rip, where, c->Rsp, c->Rbp, (DWORD)c->Rax, c->EFlags);
    log_line("REG rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx rsi=%016llx rdi=%016llx",
             c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi);
    log_line("REG r8=%016llx r9=%016llx r10=%016llx r11=%016llx r12=%016llx r13=%016llx r14=%016llx r15=%016llx",
             c->R8, c->R9, c->R10, c->R11, c->R12, c->R13, c->R14, c->R15);
    DWORD64 pointers[] = {c->Rcx, c->Rdx, c->R8, c->R9, c->Rsi, c->Rdi};
    const char *names[] = {"rcx", "rdx", "r8", "r9", "rsi", "rdi"};
    for (unsigned p = 0; p < 6; ++p) {
        unsigned char bytes[512] = {0}; SIZE_T got = 0;
        if (!ReadProcessMemory(process, (void *)pointers[p], bytes, sizeof(bytes)-2, &got) || got < 8) continue;
        char ascii[257] = {0}, utf8[1024] = {0};
        for (unsigned j = 0; j < 256 && bytes[j]; ++j) ascii[j] = bytes[j] >= 32 && bytes[j] < 127 ? bytes[j] : '.';
        if (ascii[0]) log_line("POINTER %s ascii=%s", names[p], ascii);
        if (bytes[0] >= 32 && bytes[0] < 127 && bytes[1] == 0) {
            WideCharToMultiByte(CP_UTF8, 0, (WCHAR *)bytes, -1, utf8, sizeof(utf8), NULL, NULL);
            log_line("POINTER %s utf16=%s", names[p], utf8);
        }
        log_line("POINTER %s hex=%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x",
                 names[p], bytes[0],bytes[1],bytes[2],bytes[3],bytes[4],bytes[5],bytes[6],bytes[7],bytes[8],bytes[9],bytes[10],bytes[11],bytes[12],bytes[13],bytes[14],bytes[15]);
    }
    DWORD64 words[16];
    if (read_at(c->Rsp, words, sizeof(words))) {
        for (unsigned i = 0; i < 16; i += 4)
            log_line("STACKMEM +%02x: %016llx %016llx %016llx %016llx", i * 8, words[i], words[i+1], words[i+2], words[i+3]);
    }
    char message[2048] = {0};
    if (FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, (DWORD)c->Rax, 0, message, sizeof(message), NULL))
        log_line("EAX_SYSTEM_MESSAGE %s", message);
}
static void dump_stack(DWORD tid, HANDLE thread, CONTEXT context) {
    STACKFRAME64 frame = {0};
    frame.AddrPC.Offset = context.Rip; frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp; frame.AddrStack.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
    char where[2200]; describe(context.Rip, where, sizeof(where));
    log_line("FRAME tid=%lu #0 %s", tid, where);
    DWORD64 last = 0;
    for (unsigned i = 1; i < 32; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, NULL,
                        SymFunctionTableAccess64, SymGetModuleBase64, NULL) || !frame.AddrPC.Offset) break;
        if (frame.AddrPC.Offset == last) break;
        last = frame.AddrPC.Offset; describe(last, where, sizeof(where));
        char storage[sizeof(SYMBOL_INFO) + 1024] = {0};
        SYMBOL_INFO *symbol = (SYMBOL_INFO *)storage;
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO); symbol->MaxNameLen = 1023;
        DWORD64 displacement = 0;
        if (SymFromAddr(process, last, &displacement, symbol))
            log_line("FRAME tid=%lu #%u %s (%s+0x%llx)", tid, i, where, symbol->Name, displacement);
        else log_line("FRAME tid=%lu #%u %s", tid, i, where);
    }
}
static void inspect_thread(DWORD tid, int suspend) {
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!thread) { log_line("THREAD_OPEN_FAILED tid=%lu error=%lu", tid, GetLastError()); return; }
    if (suspend && SuspendThread(thread) == (DWORD)-1) { CloseHandle(thread); return; }
    CONTEXT c = {0}; c.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(thread, &c)) { dump_context(tid, &c); dump_stack(tid, thread, c); }
    else log_line("CONTEXT_FAILED tid=%lu error=%lu", tid, GetLastError());
    if (suspend) ResumeThread(thread);
    CloseHandle(thread);
}
static BOOL CALLBACK window_callback(HWND hwnd, LPARAM unused) {
    (void)unused; DWORD owner; GetWindowThreadProcessId(hwnd, &owner);
    if (owner == pid) {
        char title[512] = {0}, cls[256] = {0}; RECT rect = {0};
        GetWindowTextA(hwnd, title, sizeof(title)); GetClassNameA(hwnd, cls, sizeof(cls)); GetClientRect(hwnd, &rect);
        log_line("WINDOW hwnd=%p visible=%d client=%ldx%ld class=%s title=%s", hwnd, IsWindowVisible(hwnd), rect.right, rect.bottom, cls, title);
    }
    return TRUE;
}
static void debug_string(const OUTPUT_DEBUG_STRING_INFO *s) {
    DWORD length = s->nDebugStringLength; if (length > 8190) length = 8190;
    if (s->fUnicode) {
        WCHAR buffer[8192] = {0}; char utf8[32768] = {0};
        if (read_at((DWORD64)s->lpDebugStringData, buffer, length * sizeof(WCHAR))) {
            buffer[length] = 0; WideCharToMultiByte(CP_UTF8, 0, buffer, -1, utf8, sizeof(utf8), NULL, NULL);
            log_line("DEBUGSTRING %s", utf8);
        }
    } else {
        char buffer[8192] = {0};
        if (read_at((DWORD64)s->lpDebugStringData, buffer, length)) log_line("DEBUGSTRING %s", buffer);
    }
}
int wmain(int argc, wchar_t **argv) {
    if (argc < 4) { fwprintf(stderr, L"Usage: probe_debugger.exe EXE LOG TIMEOUT_SECONDS [BREAK_RVA ...]\n"); return 2; }
    output = _wfopen(argv[2], L"wb"); if (!output) return 2;
    started = GetTickCount64();
    unsigned seconds = wcstoul(argv[3], NULL, 0);
    if (!seconds || seconds > 300) { fclose(output); return 2; }
    for (int i = 4; i < argc && break_count < 64; ++i) {
        BREAKPOINT *b = &breaks[break_count++];
        const WCHAR *separator = wcschr(argv[i], L'!');
        if (separator) {
            WideCharToMultiByte(CP_UTF8, 0, argv[i], (int)(separator-argv[i]), b->module, sizeof(b->module)-1, NULL, NULL);
            b->rva = _wcstoui64(separator+1, NULL, 0);
        } else b->rva = _wcstoui64(argv[i], NULL, 0);
    }
    WCHAR command[32768], directory[32768];
    _snwprintf(command, 32768, L"\"%s\"", argv[1]); wcsncpy(directory, argv[1], 32767);
    WCHAR *slash = wcsrchr(directory, L'\\'); if (!slash) { fclose(output); return 2; } *slash = 0;
    STARTUPINFOEXW startup = {0}; startup.StartupInfo.cb = sizeof(startup); PROCESS_INFORMATION pi = {0};
    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_size);
    startup.lpAttributeList = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, attribute_size);
    DWORD desktop_policy = PROCESS_CREATION_DESKTOP_APP_BREAKAWAY_ENABLE_PROCESS_TREE;
    if (!startup.lpAttributeList || !InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attribute_size) ||
        !UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_DESKTOP_APP_POLICY,
                                  &desktop_policy, sizeof(desktop_policy), NULL, NULL)) {
        log_line("DESKTOP_POLICY_FAILED error=%lu", GetLastError()); fclose(output); return 2;
    }
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) { CloseHandle(job); job = NULL; }
    }
    BOOL created = CreateProcessW(argv[1], command, NULL, NULL, FALSE, DEBUG_ONLY_THIS_PROCESS | EXTENDED_STARTUPINFO_PRESENT,
                                  NULL, directory, &startup.StartupInfo, &pi);
    DWORD create_error = GetLastError();
    DeleteProcThreadAttributeList(startup.lpAttributeList); HeapFree(GetProcessHeap(), 0, startup.lpAttributeList);
    if (!created) {
        log_line("CREATE_FAILED error=%lu", create_error); fclose(output); if (job) CloseHandle(job); return 2;
    }
    process = pi.hProcess; pid = pi.dwProcessId;
    if (job && !AssignProcessToJobObject(job, process)) log_line("JOB_ASSIGN_FAILED error=%lu", GetLastError());
    log_line("START pid=%lu timeout=%u", pid, seconds);
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
    symbols_ready = SymInitialize(process, "", FALSE);
    int done = 0, stopping = 0, initial_breakpoint = 0, result = 0; ULONGLONG stop_time = 0;
    while (!done) {
        if (!stopping && GetTickCount64() - started >= (ULONGLONG)seconds * 1000) {
            log_line("TIMEOUT: process alive; this does not imply successful initialization");
            EnumWindows(window_callback, 0);
            for (unsigned i = 0; i < thread_count; ++i) inspect_thread(threads[i], 1);
            TerminateProcess(process, 0xe0000001); stopping = 1; stop_time = GetTickCount64(); result = 10;
        }
        DEBUG_EVENT event = {0};
        if (!WaitForDebugEvent(&event, 100)) {
            if (GetLastError() != ERROR_SEM_TIMEOUT) { log_line("DEBUG_WAIT_FAILED error=%lu", GetLastError()); result = 2; break; }
            if (stopping && GetTickCount64() - stop_time > 5000) break;
            continue;
        }
        DWORD disposition = DBG_CONTINUE;
        switch (event.dwDebugEventCode) {
        case CREATE_PROCESS_DEBUG_EVENT: {
            DWORD64 base = (DWORD64)event.u.CreateProcessInfo.lpBaseOfImage;
            add_thread(event.dwThreadId); add_module(base, event.u.CreateProcessInfo.hFile);
            if (event.u.CreateProcessInfo.hFile) CloseHandle(event.u.CreateProcessInfo.hFile);
            if (event.u.CreateProcessInfo.hThread != pi.hThread) CloseHandle(event.u.CreateProcessInfo.hThread);
            CloseHandle(pi.hThread); pi.hThread = NULL; break;
        }
        case CREATE_THREAD_DEBUG_EVENT: add_thread(event.dwThreadId); CloseHandle(event.u.CreateThread.hThread); break;
        case EXIT_THREAD_DEBUG_EVENT: remove_thread(event.dwThreadId); break;
        case LOAD_DLL_DEBUG_EVENT:
            add_module((DWORD64)event.u.LoadDll.lpBaseOfDll, event.u.LoadDll.hFile);
            if (event.u.LoadDll.hFile) CloseHandle(event.u.LoadDll.hFile); break;
        case UNLOAD_DLL_DEBUG_EVENT:
            for (unsigned i = 0; i < module_count; ++i) if (modules[i].base == (DWORD64)event.u.UnloadDll.lpBaseOfDll) {
                if (symbols_ready) SymUnloadModule64(process, modules[i].base);
                modules[i] = modules[--module_count]; break;
            }
            break;
        case OUTPUT_DEBUG_STRING_EVENT: debug_string(&event.u.DebugString); break;
        case EXCEPTION_DEBUG_EVENT: {
            const EXCEPTION_DEBUG_INFO *e = &event.u.Exception;
            DWORD code = e->ExceptionRecord.ExceptionCode;
            DWORD64 address = (DWORD64)e->ExceptionRecord.ExceptionAddress;
            int handled = 0;
            if (code == EXCEPTION_BREAKPOINT) {
                for (unsigned i = 0; i < break_count; ++i) {
                    BREAKPOINT *b = &breaks[i];
                    if (b->armed && address == b->address) {
                        log_line("TRACE_BREAK rva=0x%llx tid=%lu", b->rva, event.dwThreadId);
                        inspect_thread(event.dwThreadId, 0);
                        HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, event.dwThreadId);
                        CONTEXT c = {0}; c.ContextFlags = CONTEXT_CONTROL;
                        if (!thread || !GetThreadContext(thread, &c) || !write_byte(b->address, b->original)) {
                            log_line("BREAK_RESTORE_FAILED error=%lu", GetLastError());
                            if (thread) CloseHandle(thread);
                            TerminateProcess(process, 0xe0000002); stopping = 1; stop_time = GetTickCount64(); result = 2;
                        } else {
                            c.Rip = b->address;
                            if (!SetThreadContext(thread, &c)) { TerminateProcess(process, 0xe0000002); stopping = 1; stop_time = GetTickCount64(); result = 2; }
                            CloseHandle(thread); b->armed = 0;
                        }
                        handled = 1; break;
                    }
                }
                MODULE *m = find_module(address);
                if (!handled && !initial_breakpoint && m && !_stricmp(filename(m->path), "ntdll.dll")) {
                    initial_breakpoint = 1; handled = 1; log_line("INITIAL_DEBUGGER_BREAK");
                }
            }
            if (!handled) {
                char where[2200]; describe(address, where, sizeof(where));
                log_line("EXCEPTION code=0x%08lx first_chance=%lu tid=%lu address=%s flags=0x%lx", code, e->dwFirstChance, event.dwThreadId, where, e->ExceptionRecord.ExceptionFlags);
                for (DWORD i = 0; i < e->ExceptionRecord.NumberParameters; ++i)
                    log_line("EXCEPTION_INFO [%lu]=0x%llx", i, (DWORD64)e->ExceptionRecord.ExceptionInformation[i]);
                if (code == 0x40080201 && e->ExceptionRecord.NumberParameters >= 3) {
                    WCHAR message[4096] = {0}; char utf8[16384] = {0};
                    SIZE_T count = e->ExceptionRecord.ExceptionInformation[1]; if (count > 4095) count = 4095;
                    if (read_at(e->ExceptionRecord.ExceptionInformation[2], message, count * sizeof(WCHAR))) {
                        WideCharToMultiByte(CP_UTF8, 0, message, -1, utf8, sizeof(utf8), NULL, NULL);
                        log_line("ORIGINATE_ERROR_MESSAGE %s", utf8);
                    }
                }
                inspect_thread(event.dwThreadId, 0); disposition = DBG_EXCEPTION_NOT_HANDLED;
                if (!e->dwFirstChance || code == EXCEPTION_BREAKPOINT || code == EXCEPTION_ACCESS_VIOLATION ||
                    code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_PRIV_INSTRUCTION ||
                    code == EXCEPTION_STACK_OVERFLOW || code == 0xc0000409 || code == 0xc0000374 ||
                    code == 0xc0000135 || code == 0xc0000139) {
                    EnumWindows(window_callback, 0); log_line("STOP_ON_EXCEPTION: no assertion or failure has been bypassed");
                    TerminateProcess(process, 0xe0000003); stopping = 1; stop_time = GetTickCount64(); result = 11;
                }
            }
            break;
        }
        case EXIT_PROCESS_DEBUG_EVENT: log_line("EXIT code=0x%08lx", event.u.ExitProcess.dwExitCode); done = 1; break;
        default: break;
        }
        ContinueDebugEvent(event.dwProcessId, event.dwThreadId, disposition);
    }
    if (!done && WaitForSingleObject(process, 0) == WAIT_TIMEOUT) TerminateProcess(process, 0xe0000004);
    WaitForSingleObject(process, 5000);
    log_line("END result=%d process_signaled=%d", result, WaitForSingleObject(process, 0) == WAIT_OBJECT_0);
    if (symbols_ready) SymCleanup(process);
    if (job) CloseHandle(job); if (pi.hThread) CloseHandle(pi.hThread);
    CloseHandle(process); fclose(output); printf("Debugger result=%d pid=%lu\n", result, pid);
    return result;
}
