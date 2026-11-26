#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>

/* Observation breakpoints restore the original instruction before continuing.
 * Exceptions are preserved unless the exact mismatch INT3 is opted in. */
typedef struct { DWORD64 base, size; char path[2048]; } MODULE;
typedef struct { DWORD64 rva, address; BYTE original; int armed; char module[256]; } BREAKPOINT;
enum {
    TEXTURE_HOOK_REGISTER_RVA = 0x247ad64,
    TEXTURE_HOOK_REGISTERED_RVA = 0x247ad69,
    XMEM_HOOK_DISPATCH_CALL_RVA = 0x4deb4eb,
    TEXTURE_HOOK_CATEGORY = 0x80,
    XMEM_HOOK_LIST_RVA = 0x6843370
};
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
static int skip_xmem_placeholder_splits;
static int continue_observed_descriptor_heap_mismatch;
static int continued_observed_descriptor_heap_mismatch;
static WCHAR window_capture_path[32768];

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
static void arm_breakpoint(BREAKPOINT *b, const MODULE *m) {
    b->address = m->base + b->rva;
    if (b->rva < m->size && read_at(b->address, &b->original, 1) &&
        b->original != 0xcc && write_byte(b->address, 0xcc)) {
        b->armed = 1;
        log_line("BREAK_ARMED module=%s rva=0x%llx original=%02x", filename(m->path), b->rva, b->original);
    } else log_line("BREAK_ARM_FAILED rva=0x%llx error=%lu", b->rva, GetLastError());
}
static int is_texture_dispatch_probe(const BREAKPOINT *b, const MODULE *m) {
    return b->rva == XMEM_HOOK_DISPATCH_CALL_RVA &&
        !_stricmp(filename(m->path), "game_scarlett_master.exe");
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
        if (!is_texture_dispatch_probe(b, m)) arm_breakpoint(b, m);
    }
}
static void add_thread(DWORD tid) {
    if (thread_count < 512) threads[thread_count++] = tid;
}
static void remove_thread(DWORD tid) {
    for (unsigned i = 0; i < thread_count; ++i)
        if (threads[i] == tid) { threads[i] = threads[--thread_count]; return; }
}
static void dump_region(DWORD64 address, const char *label, SIZE_T size) {
    if (!size || size > 0x1000 || address > UINT64_MAX - size) {
        log_line("MEMORY_DUMP_INVALID label=%s address=0x%llx size=%llu", label, address, (unsigned long long)size);
        return;
    }
    for (SIZE_T offset = 0; offset < size; offset += 16) {
        SIZE_T chunk = size - offset < 16 ? size - offset : 16;
        unsigned char row[16] = {0};
        if (!read_at(address + offset, row, chunk)) {
            log_line("MEMORY_DUMP_READ_FAILED label=%s address=0x%llx", label, address + offset);
            continue;
        }
        char hex[16 * 3 + 1];
        for (SIZE_T i = 0; i < chunk; ++i) snprintf(hex + i * 3, sizeof(hex) - i * 3, "%02x%s", row[i], i + 1 == chunk ? "" : " ");
        log_line("MEMORY_DUMP label=%s +0x%02llx %s", label, (unsigned long long)offset, hex);
    }
}
/* Observe the D3D12 initialization predicate and its caller without changing
 * either result. The breakpoints at 0x2478e27 and 0x19f4f54 are before TEST;
 * 0x2478e29 and 0x19f4f56 are after TEST and expose the resulting ZF. */
static void dump_startup_check(DWORD tid, const char *stage) {
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!thread) { log_line("STARTUP_CHECK_THREAD_OPEN_FAILED stage=%s tid=%lu error=%lu", stage, tid, GetLastError()); return; }
    CONTEXT c = {0}; c.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &c)) {
        log_line("STARTUP_CHECK_CONTEXT_FAILED stage=%s tid=%lu error=%lu", stage, tid, GetLastError());
        CloseHandle(thread); return;
    }
    CloseHandle(thread);
    log_line("STARTUP_CHECK stage=%s tid=%lu rip=0x%llx rax=0x%llx al=0x%02lx eflags=0x%08lx zf=%u",
             stage, tid, c.Rip, c.Rax, (DWORD)(c.Rax & 0xff), c.EFlags,
             (unsigned)((c.EFlags >> 6) & 1));
}
static void dump_descriptor_heap_trace(DWORD tid, DWORD64 rva) {
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!thread) {
        log_line("DESCRIPTOR_HANDLE_TRACE_THREAD_OPEN_FAILED rva=0x%llx tid=%lu error=%lu",
                 rva, tid, GetLastError());
        return;
    }
    CONTEXT c = {0}; c.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &c)) {
        log_line("DESCRIPTOR_HANDLE_TRACE_CONTEXT_FAILED rva=0x%llx tid=%lu error=%lu",
                 rva, tid, GetLastError());
        CloseHandle(thread); return;
    }
    CloseHandle(thread);
    log_line("DESCRIPTOR_HANDLE_TRACE rva=0x%llx tid=%lu rip=0x%llx rax=0x%llx rbx=0x%llx rcx=0x%llx rdx=0x%llx r8=0x%llx r9=0x%llx r14=0x%llx r15=0x%llx eflags=0x%08lx",
             rva, tid, c.Rip, c.Rax, c.Rbx, c.Rcx, c.Rdx, c.R8, c.R9, c.R14, c.R15, c.EFlags);
}

/* Read the startup texture arena's allocator state. The breakpoint handler
 * restores and executes the original instruction at each site. */
static void dump_texture_arena(DWORD tid, DWORD64 rva, DWORD64 image_base) {
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!thread) { log_line("TEXTURE_ARENA_THREAD_OPEN_FAILED tid=%lu error=%lu", tid, GetLastError()); return; }
    CONTEXT c = {0}; c.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &c)) {
        log_line("TEXTURE_ARENA_CONTEXT_FAILED tid=%lu error=%lu", tid, GetLastError());
        CloseHandle(thread); return;
    }
    CloseHandle(thread);
    DWORD64 base = 0, fields[2] = {0};
    DWORD64 context = 0;
    const char *stage = "unknown";
    switch (rva) {
    case TEXTURE_HOOK_REGISTER_RVA: stage = "register"; context = c.R8; break;
    case 0x247aeb6: stage = "texture-return"; context = c.Rsp + 0x70; break;
    case 0x247af25: stage = "compare"; context = c.Rsp + 0x70; break;
    case 0x56a2a60: stage = "callback"; context = c.R8; break;
    case 0x56a2a8b: stage = "matching-callback"; context = c.R8; break;
    case 0x56a2abe: stage = "backing-return"; context = c.Rdi; break;
    }
    int base_ok = read_at(image_base + 0x75e6ff0, &base, sizeof(base));
    int context_ok = context != 0 && read_at(context, fields, sizeof(fields));
    log_line("TEXTURE_ARENA stage=%s tid=%lu rva=0x%llx context=0x%llx context_ok=%d cursor=0x%llx capacity=0x%llx base_ok=%d base=0x%llx rax=0x%llx rcx=0x%llx rdx=0x%llx",
        stage, tid, rva, context, context_ok, fields[0], fields[1], base_ok, base, c.Rax, c.Rcx, c.Rdx);
}

/* Trace game-owned XMem hook registration and dispatch without invoking the
 * callback from the compatibility layer. */
static void dump_xmem_hook_probe(DWORD tid, DWORD64 rva, DWORD64 image_base) {
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!thread) {
        log_line("XMEM_HOOK_THREAD_OPEN_FAILED rva=0x%llx tid=%lu error=%lu",
            rva, tid, GetLastError());
        return;
    }
    CONTEXT c = {0}; c.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &c)) {
        log_line("XMEM_HOOK_CONTEXT_FAILED rva=0x%llx tid=%lu error=%lu",
            rva, tid, GetLastError());
        CloseHandle(thread);
        return;
    }
    CloseHandle(thread);

    if (rva == TEXTURE_HOOK_REGISTER_RVA) {
        DWORD64 arena[2] = {0}, exported_hooks[4] = {0};
        int arena_ok = c.R8 && read_at(c.R8, arena, sizeof(arena));
        int hooks_ok = read_at(image_base + 0x6844380, exported_hooks, sizeof(exported_hooks));
        log_line("XMEM_HOOK_REGISTER tid=%lu category=0x%x owner=0x%llx callback=0x%llx context=0x%llx "
            "context_ok=%d cursor=0x%llx capacity=0x%llx export_ok=%d exports=%llx,%llx,%llx,%llx",
            tid, TEXTURE_HOOK_CATEGORY, c.Rcx, c.Rdx, c.R8, arena_ok, arena[0], arena[1], hooks_ok,
            exported_hooks[0], exported_hooks[1], exported_hooks[2], exported_hooks[3]);
        return;
    }

    int registered = rva == TEXTURE_HOOK_REGISTERED_RVA;
    DWORD64 attribute_index = registered ? TEXTURE_HOOK_CATEGORY : (c.Rdx >> 16) & 0xff;
    DWORD64 head = 0;
    int head_ok = read_at(image_base + XMEM_HOOK_LIST_RVA + attribute_index * sizeof(DWORD64),
        &head, sizeof(head));
    if (registered) {
        log_line("XMEM_HOOK_REGISTERED tid=%lu category=0x%llx head_ok=%d head=0x%llx context=0x%llx",
            tid, attribute_index, head_ok, head, c.Rsp + 0x70);
    } else {
        log_line("XMEM_HOOK_DISPATCH tid=%lu size=0x%llx attributes=0x%llx category=0x%llx "
            "context=0x%llx stack_copy=0x%llx head_ok=%d head=0x%llx",
            tid, c.Rcx, c.Rdx, attribute_index, c.R8, c.R9, head_ok, head);
    }
    for (unsigned i = 0; head && i < 8; ++i) {
        DWORD64 record[4] = {0};
        if (!read_at(head, record, sizeof(record))) {
            log_line("XMEM_HOOK_RECORD unreadable index=%u address=0x%llx", i, head);
            break;
        }
        log_line("XMEM_HOOK_RECORD index=%u address=0x%llx callback=0x%llx context=0x%llx "
            "owner=0x%llx next=0x%llx",
            i, head, record[0], record[1], record[2], record[3]);
        head = record[3];
    }
}

static void dump_virtual_memory_configuration(DWORD tid, DWORD64 rva) {
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!thread) { log_line("VIRTUAL_MEMORY_CONFIG_THREAD_OPEN_FAILED tid=%lu error=%lu", tid, GetLastError()); return; }
    CONTEXT c = {0}; c.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &c)) {
        log_line("VIRTUAL_MEMORY_CONFIG_CONTEXT_FAILED tid=%lu error=%lu", tid, GetLastError());
        CloseHandle(thread); return;
    }
    CloseHandle(thread);
    DWORD64 address = rva == 0x1e1cfe0 ? c.Rcx : c.Rdi;
    DWORD64 fields[4] = {0};
    int valid = address && read_at(address, fields, sizeof(fields));
    log_line("VIRTUAL_MEMORY_CONFIG stage=%s tid=%lu rva=0x%llx address=0x%llx readable=%d fields=%016llx,%016llx,%016llx,%016llx hr=0x%08lx",
        rva == 0x1e1cfe0 ? "call" : "result", tid, rva, address, valid,
        fields[0], fields[1], fields[2], fields[3], (DWORD)c.Rax);
}
/* Capture the full raw Xbox-only device call immediately before its original
 * indirect call. The breakpoint is observational: the null target still runs
 * and its original access violation is preserved. */
static void dump_xbox_device_slot428_call(DWORD tid, const char *site) {
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!thread) { log_line("SLOT428_THREAD_OPEN_FAILED site=%s tid=%lu error=%lu", site, tid, GetLastError()); return; }
    CONTEXT c = {0}; c.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &c)) {
        log_line("SLOT428_CONTEXT_FAILED site=%s tid=%lu error=%lu", site, tid, GetLastError());
        CloseHandle(thread); return;
    }
    CloseHandle(thread);

    DWORD64 stack_args[3] = {0};
    int stack_ok = c.Rsp <= UINT64_MAX - 0x38 &&
        read_at(c.Rsp + 0x20, stack_args, sizeof(stack_args));
    log_line("SLOT428_CALL site=%s tid=%lu device=0x%llx resource_address=0x%llx config=0x%llx arg4=0x%llx stack_ok=%d arg5=0x%llx iid=0x%llx out=0x%llx",
        site, tid, c.Rcx, c.Rdx, c.R8, c.R9, stack_ok,
        stack_args[0], stack_args[1], stack_args[2]);
    char label[96];
    _snprintf(label, sizeof(label), "slot428-%s-config", site);
    dump_region(c.R8, label, 0x40);
    if (stack_ok) {
        _snprintf(label, sizeof(label), "slot428-%s-iid", site);
        dump_region(stack_args[1], label, 0x10);
        _snprintf(label, sizeof(label), "slot428-%s-out-before", site);
        dump_region(stack_args[2], label, sizeof(DWORD64));
    }
}
static void dump_xbox_device_slot70_call(DWORD tid) {
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!thread) { log_line("SLOT70_CONTEXT_OPEN_FAILED tid=%lu error=%lu", tid, GetLastError()); return; }
    CONTEXT c = {0}; c.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &c)) {
        log_line("SLOT70_CONTEXT_FAILED tid=%lu error=%lu", tid, GetLastError());
        CloseHandle(thread); return;
    }
    CloseHandle(thread);
    DWORD64 target = 0;
    int target_ok = c.Rax && c.Rax <= UINT64_MAX - 0x230 &&
        read_at(c.Rax + 0x230, &target, sizeof(target));
    log_line("SLOT70_FORWARD_CALL tid=%lu hook=0x%llx vtable=0x%llx target_ok=%d target=0x%llx arg1=0x%llx arg2=0x%llx arg3=0x%llx",
        tid, c.Rcx, c.Rax, target_ok, target, c.Rdx, c.R8, c.R9);
    dump_region(c.Rdx, "slot70-arg1", 0x20);
    dump_region(c.R8, "slot70-arg2", 0x20);
    dump_region(c.R9, "slot70-arg3", 0x20);
}
static void dump_d3d12_core_interface_arg(DWORD tid) {
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!thread) { log_line("D3D12_CORE_ARG_CONTEXT_OPEN_FAILED tid=%lu error=%lu", tid, GetLastError()); return; }
    CONTEXT c = {0}; c.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &c)) {
        log_line("D3D12_CORE_ARG_CONTEXT_FAILED tid=%lu error=%lu", tid, GetLastError());
        CloseHandle(thread); return;
    }
    CloseHandle(thread);
    DWORD64 vtable = 0, slot8 = 0;
    int vtable_ok = c.R15 && read_at(c.R15, &vtable, sizeof(vtable));
    int slot8_ok = vtable_ok && vtable <= UINT64_MAX - 0x40 &&
        read_at(vtable + 0x40, &slot8, sizeof(slot8));
    log_line("D3D12_CORE_INTERFACE_ARG tid=%lu r15=0x%llx object_vtable_ok=%d object_vtable=0x%llx slot8_ok=%d slot8=0x%llx",
        tid, c.R15, vtable_ok, vtable, slot8_ok, slot8);
    dump_region(c.R15, "d3d12-core-interface-object", 0x20);
}
static void dump_resource_allocation_info_call(DWORD tid) {
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!thread) { log_line("RESOURCE_ALLOCATION_INFO_CONTEXT_OPEN_FAILED tid=%lu error=%lu", tid, GetLastError()); return; }
    CONTEXT c = {0}; c.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &c)) {
        log_line("RESOURCE_ALLOCATION_INFO_CONTEXT_FAILED tid=%lu error=%lu", tid, GetLastError());
        CloseHandle(thread); return;
    }
    CloseHandle(thread);
    DWORD64 stack_arguments[1] = {0};
    int stack_ok = c.Rsp <= UINT64_MAX - 0x28 &&
        read_at(c.Rsp + 0x20, stack_arguments, sizeof(stack_arguments));
    log_line("RESOURCE_ALLOCATION_INFO_CALL tid=%lu rdx=0x%llx visible_mask=0x%llx num_descs=0x%llx stack_ok=%d resource_desc=0x%llx return_buffer=0x%llx",
        tid, c.Rdx, c.R8, c.R9, stack_ok, stack_ok ? stack_arguments[0] : 0, c.Rcx);
    if (stack_ok) dump_region(stack_arguments[0], "resource-allocation-resource-desc", 0x38);
}
static void dump_xbox_device_slot3f8_call(DWORD tid) {
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!thread) { log_line("SLOT3F8_CONTEXT_OPEN_FAILED tid=%lu error=%lu", tid, GetLastError()); return; }
    CONTEXT c = {0}; c.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &c)) {
        log_line("SLOT3F8_CONTEXT_FAILED tid=%lu error=%lu", tid, GetLastError());
        CloseHandle(thread); return;
    }
    CloseHandle(thread);
    DWORD64 args[7] = {0};
    int stack_ok = c.Rsp <= UINT64_MAX - 0x58 && read_at(c.Rsp + 0x20, args, sizeof(args));
    log_line("SLOT3F8_CALL tid=%lu this=0x%llx heap_properties=0x%llx heap_flags=0x%llx "
        "resource_desc=0x%llx initial_layout=0x%llx clear_value=0x%llx protected_session=0x%llx "
        "castable_count=0x%llx castable_formats=0x%llx riid=0x%llx resource_out=0x%llx stack_ok=%d",
        tid, c.Rcx, c.Rdx, c.R8, c.R9,
        stack_ok ? args[0] : 0, stack_ok ? args[1] : 0,
        stack_ok ? args[2] : 0, stack_ok ? args[3] : 0,
        stack_ok ? args[4] : 0, stack_ok ? args[5] : 0,
        stack_ok ? args[6] : 0, stack_ok);
    dump_region(c.Rdx, "slot3f8-heap-properties", 0x14);
    dump_region(c.R9, "slot3f8-resource-desc1", 0x40);
    if (stack_ok && args[1]) dump_region(args[1], "slot3f8-clear-value", 0x14);
    if (stack_ok && args[6]) dump_region(args[6], "slot3f8-resource-output-before", sizeof(DWORD64));
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
/* RVA 0x223716c reads a 32-bit field at [RSI+0x0c], then passes it to a
 * nonzero predicate before the title's own INT3 at 0x2237178. Capture only
 * those bytes and a small raw context window. */
static void dump_assertion_context(DWORD tid, const char *stage) {
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!thread) { log_line("VECTOR_DUMP_THREAD_OPEN_FAILED stage=%s tid=%lu error=%lu", stage, tid, GetLastError()); return; }
    CONTEXT c = {0}; c.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &c)) {
        log_line("VECTOR_DUMP_CONTEXT_FAILED stage=%s tid=%lu error=%lu", stage, tid, GetLastError());
        CloseHandle(thread); return;
    }
    CloseHandle(thread);

    DWORD64 context = c.Rsi;
    if (!context || context > UINT64_MAX - 0x20) {
        log_line("ASSERT_CONTEXT stage=%s rsi=0x%llx context=invalid", stage, context);
        return;
    }
    DWORD field = 0;
    if (read_at(context + 0x0c, &field, sizeof(field)))
        log_line("ASSERT_CONTEXT stage=%s tid=%lu rsi=0x%llx field_c=0x%08lx",
                 stage, tid, context, field);
    else
        log_line("ASSERT_CONTEXT stage=%s tid=%lu rsi=0x%llx field_c=read-failed",
                 stage, tid, context);
    dump_region(context, "assertion-object", 0x20);
}

/* RVA 0x56eb30 scans a lookup object using [object+0xc04] as its loop
 * bound, examines [object+0xc08]/[object+0xc0c], and advances its record
 * cursor by 0x100. Keep this as raw, disassembly-guided observation; the
 * fields' higher-level meaning is not established. */
static void dump_lookup_inputs(DWORD tid) {
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!thread) { log_line("LOOKUP_INPUT_THREAD_OPEN_FAILED tid=%lu error=%lu", tid, GetLastError()); return; }
    CONTEXT c = {0}; c.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &c)) {
        log_line("LOOKUP_INPUT_CONTEXT_FAILED tid=%lu error=%lu", tid, GetLastError());
        CloseHandle(thread); return;
    }
    CloseHandle(thread);
    log_line("LOOKUP_INPUT tid=%lu object=0x%llx filename=0x%llx root=0x%llx",
             tid, c.Rcx, c.Rdx, c.R8);
    if (!c.Rcx || c.Rcx > UINT64_MAX - 0xc40) {
        log_line("LOOKUP_DATA_INVALID object=0x%llx", c.Rcx); return;
    }
    DWORD loop_bound = 0, initial_index = 0;
    BYTE mode = 0;
    if (!read_at(c.Rcx + 0xc04, &loop_bound, sizeof(loop_bound)) ||
        !read_at(c.Rcx + 0xc08, &initial_index, sizeof(initial_index)) ||
        !read_at(c.Rcx + 0xc0c, &mode, sizeof(mode))) {
        log_line("LOOKUP_HEADER_READ_FAILED object=0x%llx", c.Rcx); return;
    }
    log_line("LOOKUP_HEADER object=0x%llx loop_bound=%lu initial_index=%lu mode=%u record_stride=0x100",
             c.Rcx, loop_bound, initial_index, mode);
    dump_region(c.Rcx + 0xc00, "lookup-header", 0x40);
    if (loop_bound > 0x1000 || loop_bound > (UINT64_MAX - c.Rcx) / 0x100) {
        log_line("LOOKUP_SCAN_DUMP_LIMIT loop_bound=%lu", loop_bound); return;
    }
    DWORD limit = loop_bound < 8 ? loop_bound : 8;
    for (DWORD i = 0; i < limit; ++i) {
        DWORD64 record = c.Rcx + (DWORD64)i * 0x100;
        char label[64];
        _snprintf(label, sizeof(label), "lookup-record-%lu", i);
        label[sizeof(label)-1] = 0;
        dump_region(record, label, 0x40);
    }
    if (loop_bound > limit) log_line("LOOKUP_SCAN_TRUNCATED dumped=%lu loop_bound=%lu", limit, loop_bound);
}

static void dump_predicate_return(DWORD tid) {
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!thread) { log_line("PREDICATE_RETURN_THREAD_OPEN_FAILED tid=%lu error=%lu", tid, GetLastError()); return; }
    CONTEXT c = {0}; c.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &c)) {
        log_line("PREDICATE_RETURN_CONTEXT_FAILED tid=%lu error=%lu", tid, GetLastError());
        CloseHandle(thread); return;
    }
    CloseHandle(thread);
    log_line("PREDICATE_RETURN tid=%lu rax=0x%llx al=0x%02lx eflags=0x%08lx", tid, c.Rax, (DWORD)(c.Rax & 0xff), c.EFlags);
}
static void dump_lookup_candidate(DWORD tid, const char *stage) {
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!thread) { log_line("LOOKUP_CANDIDATE_THREAD_OPEN_FAILED stage=%s tid=%lu error=%lu", stage, tid, GetLastError()); return; }
    CONTEXT c = {0}; c.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &c)) {
        log_line("LOOKUP_CANDIDATE_CONTEXT_FAILED stage=%s tid=%lu error=%lu", stage, tid, GetLastError());
        CloseHandle(thread); return;
    }
    CloseHandle(thread);
    log_line("LOOKUP_CANDIDATE stage=%s tid=%lu rax=0x%llx rcx=0x%llx rdx=0x%llx rbx=0x%llx",
             stage, tid, c.Rax, c.Rcx, c.Rdx, c.Rbx);
    if (c.Rbx) dump_region(c.Rbx, "lookup-scratch", 0x100);
    if (c.Rax && !strcmp(stage, "candidate-object")) {
        DWORD64 vtable = 0, validate = 0;
        if (read_at(c.Rax, &vtable, sizeof(vtable)) && vtable &&
            read_at(vtable + 0x168, &validate, sizeof(validate))) {
            log_line("LOOKUP_VTABLE object=0x%llx vtable=0x%llx validate_168=0x%llx",
                     c.Rax, vtable, validate);
            if (validate) dump_region(validate, "lookup-validator-code", 0x40);
        } else {
            log_line("LOOKUP_VTABLE_READ_FAILED object=0x%llx", c.Rax);
        }
    }
}
/* The observed graphics routine invokes Xbox-only command-list slots before
 * its final Close-shaped +0x48 call. Record each slot without invoking it. */
static void dump_graphics_extension_call(DWORD tid, DWORD64 slot_offset) {
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!thread) { log_line("GRAPHICS_EXTENSION_THREAD_OPEN_FAILED slot=0x%llx tid=%lu error=%lu", slot_offset, tid, GetLastError()); return; }
    CONTEXT c = {0}; c.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &c)) {
        log_line("GRAPHICS_EXTENSION_CONTEXT_FAILED slot=0x%llx tid=%lu error=%lu", slot_offset, tid, GetLastError());
        CloseHandle(thread); return;
    }
    CloseHandle(thread);

    DWORD64 object_vtable = 0, target = 0, stack_arg = 0, stack_object = 0;
    read_at(c.Rsp + 0x60, &stack_object, sizeof(stack_object));
    if (c.Rcx) read_at(c.Rcx, &object_vtable, sizeof(object_vtable));
    if (object_vtable) read_at(object_vtable + slot_offset, &target, sizeof(target));
    read_at(c.Rsp + 0x20, &stack_arg, sizeof(stack_arg));
    char target_name[2200] = {0};
    if (target) describe(target, target_name, sizeof(target_name));
    log_line("GRAPHICS_EXTENSION_CALL tid=%lu slot=0x%llx this=0x%llx object_vtable=0x%llx target=0x%llx (%s) rdx=0x%llx r8=0x%llx r9=0x%llx stack_arg20=0x%llx stack_rsp60=0x%llx",
             tid, slot_offset, c.Rcx, object_vtable, target, target_name,
             c.Rdx, c.R8, c.R9, stack_arg, stack_object);
    char label[80];
    _snprintf(label, sizeof(label), "graphics-slot-%llx-code", slot_offset);
    label[sizeof(label)-1] = 0;
    if (target) dump_region(target, label, 0x20);
}
static void dump_graphics_extension_return(DWORD tid, const char *stage) {
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!thread) { log_line("GRAPHICS_EXTENSION_RETURN_THREAD_OPEN_FAILED stage=%s tid=%lu error=%lu", stage, tid, GetLastError()); return; }
    CONTEXT c = {0}; c.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &c)) {
        log_line("GRAPHICS_EXTENSION_RETURN_CONTEXT_FAILED stage=%s tid=%lu error=%lu", stage, tid, GetLastError());
        CloseHandle(thread); return;
    }
    CloseHandle(thread);
    log_line("GRAPHICS_EXTENSION_RETURN stage=%s tid=%lu rax=0x%llx eax=0x%08lx eflags=0x%08lx",
             stage, tid, c.Rax, (DWORD)c.Rax, c.EFlags);
}
/* RVA 0x2478bcd is immediately before the list's Close-shaped slot +0x48.
 * Also report neighboring Xbox extension targets without invoking them. */
static void dump_graphics_close_call(DWORD tid) {
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!thread) { log_line("GRAPHICS_CLOSE_THREAD_OPEN_FAILED tid=%lu error=%lu", tid, GetLastError()); return; }
    CONTEXT c = {0}; c.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &c)) {
        log_line("GRAPHICS_CLOSE_CONTEXT_FAILED tid=%lu error=%lu", tid, GetLastError());
        CloseHandle(thread); return;
    }
    CloseHandle(thread);
    DWORD64 object_vtable = 0, extension_418 = 0, extension_420 = 0, close_target = 0;
    read_at(c.Rcx, &object_vtable, sizeof(object_vtable));
    if (object_vtable) {
        read_at(object_vtable + 0x418, &extension_418, sizeof(extension_418));
        read_at(object_vtable + 0x420, &extension_420, sizeof(extension_420));
        read_at(object_vtable + 0x48, &close_target, sizeof(close_target));
    }
    char close_name[2200] = {0}, slot418_name[2200] = {0}, slot420_name[2200] = {0};
    if (close_target) describe(close_target, close_name, sizeof(close_name));
    if (extension_418) describe(extension_418, slot418_name, sizeof(slot418_name));
    if (extension_420) describe(extension_420, slot420_name, sizeof(slot420_name));
    log_line("GRAPHICS_CLOSE_CALL tid=%lu this=0x%llx vtable=0x%llx slot48=0x%llx (%s) slot418=0x%llx (%s) slot420=0x%llx (%s)",
             tid, c.Rcx, object_vtable, close_target, close_name,
             extension_418, slot418_name, extension_420, slot420_name);
}
static void dump_graphics_close_return(DWORD tid) {
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!thread) { log_line("GRAPHICS_CLOSE_RETURN_THREAD_OPEN_FAILED tid=%lu error=%lu", tid, GetLastError()); return; }
    CONTEXT c = {0}; c.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &c)) {
        log_line("GRAPHICS_CLOSE_RETURN_CONTEXT_FAILED tid=%lu error=%lu", tid, GetLastError());
        CloseHandle(thread); return;
    }
    CloseHandle(thread);
    log_line("GRAPHICS_CLOSE_RETURN tid=%lu eax=0x%08lx eflags=0x%08lx", tid, (DWORD)c.Rax, c.EFlags);
}
/* At RVA 0x1fb1f50 the helper has returned. Capture the result and the path
 * before the validator takes its success or suffix-check branch. */
static void dump_validator_helper_result(DWORD tid) {
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!thread) { log_line("VALIDATOR_HELPER_RESULT_THREAD_OPEN_FAILED tid=%lu error=%lu", tid, GetLastError()); return; }
    CONTEXT c = {0}; c.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &c)) {
        log_line("VALIDATOR_HELPER_RESULT_CONTEXT_FAILED tid=%lu error=%lu", tid, GetLastError());
        CloseHandle(thread); return;
    }
    CloseHandle(thread);
    char path[160] = {0}; SIZE_T got = 0;
    if (c.Rsi) ReadProcessMemory(process, (LPCVOID)c.Rsi, path, sizeof(path)-1, &got);
    log_line("VALIDATOR_HELPER_RESULT tid=%lu rax=0x%llx rsi=0x%llx path=%s",
             tid, c.Rax, c.Rsi, path);
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
static void capture_window_client(HWND hwnd) {
    RECT rect = {0};
    int width = 0, height = 0;
    HDC source_dc = NULL, memory_dc = NULL;
    HWND source_window = NULL;
    POINT source_origin = {0};
    HBITMAP bitmap = NULL, previous_bitmap = NULL;
    void *pixels = NULL;
    HANDLE file = INVALID_HANDLE_VALUE;
    BITMAPINFO info = {0};
    BITMAPFILEHEADER file_header = {0};
    BITMAPINFOHEADER info_header = {0};
    DWORD pixel_bytes = 0, written = 0;
    DWORD failure_error = ERROR_SUCCESS, desktop_error = ERROR_SUCCESS;
    const char *failure_stage = "GetClientRect";
    const char *capture_source = "desktop";
    BOOL copied = FALSE, saved = FALSE;

    if (!window_capture_path[0]) return;
    if (!GetClientRect(hwnd, &rect)) {
        failure_error = GetLastError();
        goto cleanup;
    }
    width = rect.right - rect.left;
    height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192 ||
        (uint64_t)width * (uint64_t)height > (UINT32_MAX - 54u) / 4u) {
        log_line("WINDOW_CAPTURE_INVALID_SIZE width=%d height=%d", width, height);
        return;
    }
    pixel_bytes = (DWORD)((uint64_t)width * (uint64_t)height * 4u);
    failure_stage = "GetDC_DESKTOP";
    source_dc = GetDC(NULL);
    if (!source_dc) {
        failure_error = GetLastError();
        goto cleanup;
    }
    failure_stage = "ClientToScreen";
    if (!ClientToScreen(hwnd, &source_origin)) {
        failure_error = GetLastError();
        goto cleanup;
    }
    failure_stage = "CreateCompatibleDC";
    memory_dc = CreateCompatibleDC(source_dc);
    if (!memory_dc) {
        failure_error = GetLastError();
        goto cleanup;
    }

    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    failure_stage = "CreateDIBSection";
    bitmap = CreateDIBSection(source_dc, &info, DIB_RGB_COLORS, &pixels, NULL, 0);
    if (!bitmap || !pixels) {
        failure_error = GetLastError();
        goto cleanup;
    }
    failure_stage = "SelectObject";
    previous_bitmap = (HBITMAP)SelectObject(memory_dc, bitmap);
    if (!previous_bitmap || previous_bitmap == HGDI_ERROR) {
        previous_bitmap = NULL;
        failure_error = GetLastError();
        goto cleanup;
    }
    failure_stage = "BitBlt_DESKTOP";
    copied = BitBlt(memory_dc, 0, 0, width, height, source_dc,
                    source_origin.x, source_origin.y, SRCCOPY | CAPTUREBLT);
    if (!copied) {
        desktop_error = GetLastError();
        ReleaseDC(NULL, source_dc);
        source_dc = NULL;
        capture_source = "window";
        source_window = hwnd;
        failure_stage = "GetDC_WINDOW";
        source_dc = GetDC(hwnd);
        if (!source_dc) {
            failure_error = GetLastError();
            goto cleanup;
        }
        failure_stage = "BitBlt_WINDOW";
        copied = BitBlt(memory_dc, 0, 0, width, height, source_dc,
                        0, 0, SRCCOPY);
        if (!copied) {
            failure_error = GetLastError();
            goto cleanup;
        }
    }

    failure_stage = "CreateFile";
    file = CreateFileW(window_capture_path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        failure_error = GetLastError();
        goto cleanup;
    }
    file_header.bfType = 0x4d42;
    file_header.bfOffBits = (DWORD)(sizeof(file_header) + sizeof(info_header));
    file_header.bfSize = file_header.bfOffBits + pixel_bytes;
    info_header.biSize = sizeof(info_header);
    info_header.biWidth = width;
    info_header.biHeight = -height;
    info_header.biPlanes = 1;
    info_header.biBitCount = 32;
    info_header.biCompression = BI_RGB;
    info_header.biSizeImage = pixel_bytes;
    failure_stage = "WriteFileHeader";
    if (!WriteFile(file, &file_header, sizeof(file_header), &written, NULL) ||
        written != sizeof(file_header)) {
        failure_error = GetLastError();
        if (!failure_error) failure_error = ERROR_WRITE_FAULT;
        goto cleanup;
    }
    failure_stage = "WriteFileInfo";
    if (!WriteFile(file, &info_header, sizeof(info_header), &written, NULL) ||
        written != sizeof(info_header)) {
        failure_error = GetLastError();
        if (!failure_error) failure_error = ERROR_WRITE_FAULT;
        goto cleanup;
    }
    failure_stage = "WriteFilePixels";
    if (!WriteFile(file, pixels, pixel_bytes, &written, NULL) || written != pixel_bytes) {
        failure_error = GetLastError();
        if (!failure_error) failure_error = ERROR_WRITE_FAULT;
        goto cleanup;
    }
    saved = TRUE;

cleanup:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (previous_bitmap && memory_dc) SelectObject(memory_dc, previous_bitmap);
    if (bitmap) DeleteObject(bitmap);
    if (memory_dc) DeleteDC(memory_dc);
    if (source_dc) ReleaseDC(source_window, source_dc);
    if (saved)
        log_line("WINDOW_CAPTURE_SAVED source=%s path=%S hwnd=%p width=%d height=%d origin=%ld,%ld bytes=%lu",
                 capture_source, window_capture_path, hwnd, width, height,
                 source_origin.x, source_origin.y, pixel_bytes + 54u);
    else
        log_line("WINDOW_CAPTURE_FAILED source=%s path=%S hwnd=%p stage=%s origin=%ld,%ld copied=%d error=%lu desktop_error=%lu",
                 capture_source, window_capture_path, hwnd, failure_stage,
                 source_origin.x, source_origin.y, copied, failure_error, desktop_error);
}
static BOOL CALLBACK window_callback(HWND hwnd, LPARAM unused) {
    (void)unused; DWORD owner; GetWindowThreadProcessId(hwnd, &owner);
    if (owner == pid) {
        char title[512] = {0}, cls[256] = {0}; RECT rect = {0};
        GetWindowTextA(hwnd, title, sizeof(title)); GetClassNameA(hwnd, cls, sizeof(cls)); GetClientRect(hwnd, &rect);
        log_line("WINDOW hwnd=%p visible=%d client=%ldx%ld class=%s title=%s", hwnd, IsWindowVisible(hwnd), rect.right, rect.bottom, cls, title);
        if (IsWindowVisible(hwnd) && !_stricmp(cls, "sgaWindowClass")) capture_window_client(hwnd);
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
    _snwprintf(window_capture_path, sizeof(window_capture_path) / sizeof(window_capture_path[0]),
               L"%s.window.bmp", argv[2]);
    window_capture_path[(sizeof(window_capture_path) / sizeof(window_capture_path[0])) - 1] = 0;
    output = _wfopen(argv[2], L"wb"); if (!output) return 2;
    started = GetTickCount64();
    {
        char value[2] = {0, 0};
        skip_xmem_placeholder_splits =
            GetEnvironmentVariableA("COMPAT_SKIP_XMEM_PLACEHOLDER_SPLITS", value,
                (DWORD)sizeof(value)) == 1 && value[0] == '1';
        continue_observed_descriptor_heap_mismatch =
            GetEnvironmentVariableA("COMPAT_CONTINUE_OBSERVED_DESCRIPTOR_HEAP_MISMATCH", value,
                (DWORD)sizeof(value)) == 1 && value[0] == '1';
    }
    log_line("DIAGNOSTIC_CONTINUE_OBSERVED_DESCRIPTOR_HEAP_MISMATCH enabled=%d",
             continue_observed_descriptor_heap_mismatch);
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
            int continued_mismatch_this_exception = 0;
            if (code == EXCEPTION_BREAKPOINT) {
                for (unsigned i = 0; i < break_count; ++i) {
                    BREAKPOINT *b = &breaks[i];
                    if (b->armed && address == b->address) {
                        log_line("TRACE_BREAK rva=0x%llx tid=%lu", b->rva, event.dwThreadId);
                        if (b->rva == 0x1e1cfe0 || b->rva == 0x1e1cfe5)
                            dump_virtual_memory_configuration(event.dwThreadId, b->rva);
                        if (b->rva == TEXTURE_HOOK_REGISTER_RVA || b->rva == TEXTURE_HOOK_REGISTERED_RVA ||
                            b->rva == XMEM_HOOK_DISPATCH_CALL_RVA) {
                            MODULE *owner = find_module(b->address);
                            if (owner && !_stricmp(filename(owner->path), "game_scarlett_master.exe")) {
                                dump_xmem_hook_probe(event.dwThreadId, b->rva, owner->base);
                                if (b->rva == TEXTURE_HOOK_REGISTERED_RVA) {
                                    for (unsigned j = 0; j < break_count; ++j) {
                                        BREAKPOINT *dispatch = &breaks[j];
                                        if (!dispatch->address && is_texture_dispatch_probe(dispatch, owner) &&
                                            (!dispatch->module[0] || !_stricmp(dispatch->module, filename(owner->path))))
                                            arm_breakpoint(dispatch, owner);
                                    }
                                }
                            }
                        }
                        if (b->rva == TEXTURE_HOOK_REGISTER_RVA || b->rva == 0x247aeb6 ||
                            b->rva == 0x247af25 || b->rva == 0x56a2a60 ||
                            b->rva == 0x56a2a8b || b->rva == 0x56a2abe) {
                            MODULE *owner = find_module(b->address);
                            if (owner && !_stricmp(filename(owner->path), "game_scarlett_master.exe"))
                                dump_texture_arena(event.dwThreadId, b->rva, owner->base);
                        }
                        if (b->rva == 0x1e8684c) dump_xbox_device_slot428_call(event.dwThreadId, "startup");
                        if (b->rva == 0x193dff8) dump_xbox_device_slot428_call(event.dwThreadId, "resource-setup-a");
                        if (b->rva == 0x193e2d4) dump_xbox_device_slot428_call(event.dwThreadId, "resource-setup-b");
                        if (b->rva == 0x1cf3355 || b->rva == 0x1cf335b || b->rva == 0x1cf336a ||
                            b->rva == 0x1cf338f || b->rva == 0x1cf3395)
                            dump_descriptor_heap_trace(event.dwThreadId, b->rva);
                        if (b->rva == 0x4ecc81b) dump_xbox_device_slot70_call(event.dwThreadId);
                        if (b->rva == 0x344182) dump_resource_allocation_info_call(event.dwThreadId);
                        if (b->rva == 0x193e1b4) dump_xbox_device_slot3f8_call(event.dwThreadId);
                        if (b->rva == 0xc3fa2 && b->module[0] &&
                            !_stricmp(b->module, "D3D12Core.dll"))
                            dump_d3d12_core_interface_arg(event.dwThreadId);
                        inspect_thread(event.dwThreadId, 0);
                        if (b->rva == 0x1fb1f50) dump_validator_helper_result(event.dwThreadId);
                        if (b->rva == 0x2478b96) dump_graphics_extension_call(event.dwThreadId, 0x420);
                        if (b->rva == 0x2478ba4) dump_graphics_extension_return(event.dwThreadId, "slot-420");
                        if (b->rva == 0x2478bb7) dump_graphics_extension_call(event.dwThreadId, 0x418);
                        if (b->rva == 0x2478bc5) dump_graphics_extension_return(event.dwThreadId, "slot-418");
                        if (b->rva == 0x2478bcd) dump_graphics_close_call(event.dwThreadId);
                        if (b->rva == 0x2478bd0) dump_graphics_close_return(event.dwThreadId);
                        if (b->rva == 0x2478e22) dump_startup_check(event.dwThreadId, "device-278-call");
                        if (b->rva == 0x2478e27) dump_startup_check(event.dwThreadId, "device-278-result-pre-test");
                        if (b->rva == 0x2478e29) dump_startup_check(event.dwThreadId, "device-278-result-post-test");
                        if (b->rva == 0x2478e2f) dump_startup_check(event.dwThreadId, "device-278-success-path");
                        if (b->rva == 0x24787a7) dump_startup_check(event.dwThreadId, "shared-false-exit");
                        if (b->rva == 0x19f4f54) dump_startup_check(event.dwThreadId, "caller-result-pre-test");
                        if (b->rva == 0x19f4f56) dump_startup_check(event.dwThreadId, "caller-result-post-test");
                        if (b->rva == 0x2236e6e) dump_lookup_inputs(event.dwThreadId);
                        if (b->rva == 0x2236e73 || b->rva == 0x56eb7e ||
                            b->rva == 0x643cb8 || b->rva == 0x56ebc6 ||
                            b->rva == 0x56ebf0 || b->rva == 0x56ec0a)
                            dump_predicate_return(event.dwThreadId);
                        if (b->rva == 0x56eba8) dump_lookup_candidate(event.dwThreadId, "candidate-object");
                        if (b->rva == 0x56ebbd) dump_lookup_candidate(event.dwThreadId, "candidate-validation-result");
                        if (b->rva == 0x223716c || b->rva == 0x2237178)
                            dump_assertion_context(event.dwThreadId, "pre-assert");
                        HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, event.dwThreadId);
                        CONTEXT c = {0}; c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                        if (!thread || !GetThreadContext(thread, &c) || !write_byte(b->address, b->original)) {
                            log_line("BREAK_RESTORE_FAILED error=%lu", GetLastError());
                            if (thread) CloseHandle(thread);
                            TerminateProcess(process, 0xe0000002); stopping = 1; stop_time = GetTickCount64(); result = 2;
                        } else {
                            c.Rip = b->address;
                            if (skip_xmem_placeholder_splits &&
                                (b->rva == 0x1e864cd || b->rva == 0x1e864fa ||
                                 b->rva == 0x1e86531 || b->rva == 0x1e8655e)) {
                                /* These four sites are the six-byte indirect
                                 * VirtualFree calls used only to split the
                                 * observed placeholder reservation. Unified
                                 * mode leaves the reservation intact. */
                                c.Rip = b->address + 6;
                                c.Rax = 1;
                                log_line("XMEM_PLACEHOLDER_SPLIT_SKIPPED rva=0x%llx tid=%lu rip=0x%llx",
                                         b->rva, event.dwThreadId, c.Rip);
                            }
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
                MODULE *exception_module = find_module(address);
                if (exception_module && !_stricmp(filename(exception_module->path), "game_scarlett_master.exe") &&
                    address - exception_module->base == 0x2237178)
                    dump_assertion_context(event.dwThreadId, "assertion-int3");
                if (code == 0x40080201 && e->ExceptionRecord.NumberParameters >= 3) {
                    WCHAR message[4096] = {0}; char utf8[16384] = {0};
                    SIZE_T count = e->ExceptionRecord.ExceptionInformation[1]; if (count > 4095) count = 4095;
                    if (read_at(e->ExceptionRecord.ExceptionInformation[2], message, count * sizeof(WCHAR))) {
                        WideCharToMultiByte(CP_UTF8, 0, message, -1, utf8, sizeof(utf8), NULL, NULL);
                        log_line("ORIGINATE_ERROR_MESSAGE %s", utf8);
                    }
                }
                inspect_thread(event.dwThreadId, 0); disposition = DBG_EXCEPTION_NOT_HANDLED;
                if (continue_observed_descriptor_heap_mismatch &&
                    !continued_observed_descriptor_heap_mismatch && e->dwFirstChance &&
                    code == EXCEPTION_BREAKPOINT && exception_module &&
                    !_stricmp(filename(exception_module->path), "game_scarlett_master.exe") &&
                    address - exception_module->base == 0x1cf336c) {
                    BYTE opcode = 0;
                    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, event.dwThreadId);
                    CONTEXT c = {0}; c.ContextFlags = CONTEXT_CONTROL;
                    if (!read_at(address, &opcode, sizeof(opcode)) || opcode != 0xcc) {
                        log_line("DESCRIPTOR_MISMATCH_CONTINUE_REFUSED reason=opcode rva=0x1cf336c opcode=0x%02x",
                                 opcode);
                    } else if (!thread) {
                        log_line("DESCRIPTOR_MISMATCH_CONTINUE_REFUSED reason=open-thread error=%lu", GetLastError());
                    } else if (!GetThreadContext(thread, &c)) {
                        log_line("DESCRIPTOR_MISMATCH_CONTINUE_REFUSED reason=get-context error=%lu", GetLastError());
                    } else if (c.Rip != address + 1) {
                        log_line("DESCRIPTOR_MISMATCH_CONTINUE_REFUSED reason=rip rip=0x%llx expected=0x%llx",
                                 c.Rip, address + 1);
                    } else {
                        c.Rip = address + 1;
                        if (SetThreadContext(thread, &c)) {
                            continued_observed_descriptor_heap_mismatch = 1;
                            continued_mismatch_this_exception = 1;
                            disposition = DBG_CONTINUE;
                            log_line("OBSERVED_DESCRIPTOR_HEAP_MISMATCH_INT3_CONTINUED rva=0x1cf336c tid=%lu rip=0x%llx opcode=0xcc",
                                     event.dwThreadId, c.Rip);
                        } else {
                            log_line("DESCRIPTOR_MISMATCH_CONTINUE_REFUSED reason=set-context error=%lu", GetLastError());
                        }
                    }
                    if (thread) CloseHandle(thread);
                }
                if (!continued_mismatch_this_exception &&
                    (!e->dwFirstChance || code == EXCEPTION_BREAKPOINT || code == EXCEPTION_ACCESS_VIOLATION ||
                    code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_PRIV_INSTRUCTION ||
                    code == EXCEPTION_STACK_OVERFLOW || code == 0xc0000409 || code == 0xc0000374 ||
                    code == 0xc0000135 || code == 0xc0000139)) {
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
