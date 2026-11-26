#define COBJMACROS
#include <windows.h>
#define D3D12SerializeVersionedRootSignature D3D12SerializeVersionedRootSignatureSdkDeclaration
#include <d3d12.h>
#undef D3D12SerializeVersionedRootSignature
#include <stdint.h>
#include <stddef.h>
#include <intrin.h>
#include <string.h>
#include <stdio.h>

typedef PVOID (WINAPI *PFN_VIRTUAL_ALLOC2)(HANDLE process, PVOID base_address,
    SIZE_T size, ULONG allocation_type, ULONG page_protection,
    PMEM_EXTENDED_PARAMETER extended_parameters, ULONG parameter_count);

static PVOID compat_virtual_alloc2(PVOID address, SIZE_T size,
    ULONG allocation_type, ULONG protection)
{
    HMODULE module = GetModuleHandleW(L"kernel32.dll");
    PFN_VIRTUAL_ALLOC2 virtual_alloc2 = NULL;
    if (module)
        virtual_alloc2 = (PFN_VIRTUAL_ALLOC2)GetProcAddress(module, "VirtualAlloc2");
    if (!virtual_alloc2) {
        module = GetModuleHandleW(L"kernelbase.dll");
        if (module)
            virtual_alloc2 = (PFN_VIRTUAL_ALLOC2)GetProcAddress(module, "VirtualAlloc2");
    }
    if (!virtual_alloc2) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return NULL;
    }
    /* GetProcAddress may leave a stale error even though the allocation
     * succeeds. Clear it immediately before the API whose result is logged. */
    SetLastError(ERROR_SUCCESS);
    return virtual_alloc2(GetCurrentProcess(), address, size, allocation_type,
        protection, NULL, 0);
}

/* Diagnostic mode state for keeping the observed 185 MiB range as one
 * ordinary reservation while committing the five exact startup segments. */
static void *g_xmem_unified_base;
static SIZE_T g_xmem_unified_size;

/* The observed startup texture hook accounts for 64 KiB-aligned slices.
 * This CPU allocation gives that hook a real address range; it does not
 * share physical memory with the committed Windows texture resources. */
#define COMPAT_TEXTURE_ARENA_ATTRIBUTES 0x00808000u
#define COMPAT_TEXTURE_ARENA_TEXTURE_BYTES 0x01fe0000u
static SRWLOCK g_xmem_texture_arena_lock = SRWLOCK_INIT;
static void *g_xmem_texture_arena_base;
static SIZE_T g_xmem_texture_arena_size;
static __declspec(thread) BOOL g_xmem_texture_arena_dispatching;

/*
 * This is a deliberately small loader probe, not an implementation of the
 * Xbox runtime.  It supplies the missing export names so the executable can
 * reveal the next initialization boundary.  Functions that cannot be mapped
 * safely to Windows return E_NOTIMPL; memory helpers use ordinary process
 * heap/virtual memory where that is semantically reasonable.
 */

static void *compat_alloc(size_t size)
{
    return HeapAlloc(GetProcessHeap(), 0, size ? size : 1);
}

static void compat_free(void *ptr)
{
    if (ptr) HeapFree(GetProcessHeap(), 0, ptr);
}

static BOOL compat_xmem_free_texture_arena(void *ptr, unsigned flags)
{
    if (!ptr || flags != COMPAT_TEXTURE_ARENA_ATTRIBUTES) return FALSE;
    AcquireSRWLockExclusive(&g_xmem_texture_arena_lock);
    uintptr_t address = (uintptr_t)ptr;
    uintptr_t base = (uintptr_t)g_xmem_texture_arena_base;
    BOOL in_arena = base && address >= base && address - base < g_xmem_texture_arena_size;
    if (in_arena && address == base && VirtualFree(ptr, 0, MEM_RELEASE)) {
        g_xmem_texture_arena_base = NULL;
        g_xmem_texture_arena_size = 0;
    }
    ReleaseSRWLockExclusive(&g_xmem_texture_arena_lock);
    return in_arena;
}

__declspec(dllexport) void *WINAPI XMemAlloc(size_t size, unsigned flags)
{
    (void)flags;
    return compat_alloc(size);
}

__declspec(dllexport) void *WINAPI XMemAllocDefault(size_t size, unsigned flags)
{
    if (g_xmem_texture_arena_dispatching && flags == COMPAT_TEXTURE_ARENA_ATTRIBUTES) {
        /* Without the registered hook, the dispatcher would pass the slice
         * size here directly. Refuse that request rather than log a match. */
        if (size != 0x4000000u && size != 0x5200000u) return NULL;
        AcquireSRWLockExclusive(&g_xmem_texture_arena_lock);
        void *base = NULL;
        if (!g_xmem_texture_arena_base) {
            base = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (base) {
                g_xmem_texture_arena_base = base;
                g_xmem_texture_arena_size = size;
            }
        }
        ReleaseSRWLockExclusive(&g_xmem_texture_arena_lock);
        char message[200];
        snprintf(message, sizeof(message),
            "[xmem] texture arena backing bytes=0x%llx attributes=0x%x base=%p\n",
            (unsigned long long)size, flags, base);
        OutputDebugStringA(message);
        return base;
    }
    return compat_alloc(size);
}

__declspec(dllexport) void WINAPI XMemFree(void *ptr, unsigned flags)
{
    if (!compat_xmem_free_texture_arena(ptr, flags)) compat_free(ptr);
}

__declspec(dllexport) void WINAPI XMemFreeDefault(void *ptr, unsigned flags)
{
    if (!compat_xmem_free_texture_arena(ptr, flags)) compat_free(ptr);
}

/* This optional interface is called in xmem.dll so the dispatch and the
 * default backing allocator share state. The EXE's exported dispatcher
 * takes care of selecting the current thread's registered callback. */
__declspec(dllexport) BOOL WINAPI CompatXMemTextureArenaSlice(SIZE_T size, SIZE_T alignment,
    void **allocated, SIZE_T *offset)
{
    typedef void *(WINAPI *hook_allocate_fn)(SIZE_T, ULONG_PTR);
    if (!allocated || !offset) return FALSE;
    *allocated = NULL;
    *offset = 0;
    if (size != COMPAT_TEXTURE_ARENA_TEXTURE_BYTES || alignment != 0x10000u ||
        g_xmem_texture_arena_dispatching) return FALSE;

    HMODULE exe = GetModuleHandleW(NULL);
    FARPROC record = exe ? GetProcAddress(exe, "__XMemAllocationHooks__") : NULL;
    MEMORY_BASIC_INFORMATION memory = {0};
    if (!record || !VirtualQuery((LPCVOID)record, &memory, sizeof(memory)) ||
        memory.AllocationBase != exe || memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        memory.RegionSize < sizeof(hook_allocate_fn) ||
        (uintptr_t)record - (uintptr_t)memory.BaseAddress > memory.RegionSize - sizeof(hook_allocate_fn))
        return FALSE;
    hook_allocate_fn allocate = NULL;
    memcpy(&allocate, (const void *)record, sizeof(allocate));
    if (!allocate || !VirtualQuery((LPCVOID)allocate, &memory, sizeof(memory)) ||
        memory.AllocationBase != exe || memory.State != MEM_COMMIT ||
        (memory.Protect & PAGE_GUARD) ||
        !(memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
        return FALSE;

    /* Category 0x80 and mode 0x8000 are confirmed by the dispatcher and its
     * callback. The full attribute word from Xbox D3D12 is still unknown. */
    g_xmem_texture_arena_dispatching = TRUE;
    void *slice = allocate(size, COMPAT_TEXTURE_ARENA_ATTRIBUTES);
    g_xmem_texture_arena_dispatching = FALSE;
    AcquireSRWLockShared(&g_xmem_texture_arena_lock);
    uintptr_t address = (uintptr_t)slice;
    uintptr_t base = (uintptr_t)g_xmem_texture_arena_base;
    BOOL valid = base && address >= base && size <= g_xmem_texture_arena_size &&
        address - base <= g_xmem_texture_arena_size - size && !(address & (alignment - 1));
    if (valid) {
        *allocated = slice;
        *offset = address - base;
    }
    ReleaseSRWLockShared(&g_xmem_texture_arena_lock);
    return valid;
}

__declspec(dllexport) void *WINAPI XMemVirtualAlloc(void *address, size_t size,
    unsigned allocation_flags, unsigned attributes, unsigned protection_flags)
{
    const unsigned xbox_64k_pages = 0x20400000u;
    const unsigned xbox_page_cluster = 0x20000000u;
    unsigned windows_allocation_flags = MEM_COMMIT | MEM_RESERVE | MEM_RESET |
        MEM_RESET_UNDO | MEM_TOP_DOWN | MEM_WRITE_WATCH | MEM_LARGE_PAGES | MEM_PHYSICAL;
    const unsigned windows_protection_flags = 0xffu | PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE;
    unsigned pc_allocation_flags = allocation_flags;
    unsigned pc_protection_flags = protection_flags;
    DWORD error = ERROR_SUCCESS;
    void *result = NULL;
    char message[512];
    char *cursor = message;
    const char *labels[] = { " address=", " size=", " allocation=", " attributes=",
        " protection=", " pcAllocation=", " pcProtection=", " result=", " error=" };
    uint64_t values[9];
    const char *prefix = "[xmem] XMemVirtualAlloc";
    char allow_va_reservation[2] = { 0, 0 };
    BOOL exact_placeholder_reserve = FALSE;
    BOOL exact_placeholder_replace = FALSE;
    BOOL exact_unified_reserve = FALSE;
    BOOL exact_unified_commit = FALSE;
    BOOL allow_observed_va_reservation =
        GetEnvironmentVariableA("COMPAT_XMEM_EXACT_VA_RESERVATION", allow_va_reservation,
            (DWORD)sizeof(allow_va_reservation)) == 1 && allow_va_reservation[0] == '1';
    char unified_value[2] = { 0, 0 };
    BOOL allow_unified_reservation =
        GetEnvironmentVariableA("COMPAT_XMEM_UNIFIED_RESERVATION", unified_value,
            (DWORD)sizeof(unified_value)) == 1 && unified_value[0] == '1';

    /* Microsoft GDK's FSR3 sample passes XMem attributes fourth and page
     * protection fifth, reserves first, then maps physical pages separately.
     * The first two observed attribute forms only support a reservation probe.
     * This exact third request is accepted as ordinary CPU memory so startup
     * can be traced further; it does not provide Xbox physical pages, graphics
     * coherence, or a valid GPU mapping.
     */
    if (attributes == 0x181u &&
        allocation_flags == (xbox_64k_pages | MEM_RESERVE | MEM_COMMIT) &&
        protection_flags == PAGE_READWRITE && address == NULL && size == 0x01400000u) {
        /* Observed 20 MiB mappable heap with Xbox 64 KiB page granularity. */
        pc_allocation_flags &= ~xbox_64k_pages;
    } else if (attributes == 0x183u &&
        allocation_flags == (xbox_page_cluster | MEM_RESERVE | MEM_COMMIT) &&
        protection_flags == (PAGE_READWRITE | PAGE_GRAPHICS_READWRITE | PAGE_GRAPHICS_COHERENT) &&
        address == NULL && (size == 0x49400000u || size == 0x23300000u || size == 0x25400000u)) {
        /* This Xbox page/cluster flag shares the numeric value of Windows
         * MEM_LARGE_PAGES. Use regular committed memory for this exact
         * observed request, retaining only its CPU PAGE_READWRITE permission.
         */
        pc_allocation_flags &= ~xbox_page_cluster;
        pc_protection_flags = PAGE_READWRITE;
    } else if (attributes == 0x283u &&
        allocation_flags == (xbox_page_cluster | MEM_RESERVE | MEM_COMMIT) &&
        protection_flags == (PAGE_READWRITE | PAGE_GRAPHICS_READWRITE | PAGE_WRITECOMBINE) &&
        address == NULL && size == 0x89700000u) {
        /* Second observed graphics heap shape. Preserve the Windows CPU
         * write-combine bit while dropping the Xbox graphics protection bit.
         */
        pc_allocation_flags &= ~xbox_page_cluster;
        pc_protection_flags = PAGE_READWRITE | PAGE_WRITECOMBINE;
    } else if (attributes == 0x283u &&
        allocation_flags == (xbox_64k_pages | MEM_COMMIT) &&
        protection_flags == (PAGE_READWRITE | PAGE_GRAPHICS_READWRITE |
            PAGE_GRAPHICS_NOCACHE | PAGE_WRITECOMBINE) &&
        address == NULL && size == 0x10000u) {
        /* Observed 64 KiB CPU-visible page with Xbox graphics/NOCACHE flags.
         * Xbox accepts this address-less commit form as an allocation; Windows
         * needs an address reservation, so back this exact request with a
         * 64 KiB-aligned reserve+commit and retain CPU write-combine behavior.
         * This is ordinary host memory, not GPU-local or Xbox physical memory.
         */
        pc_allocation_flags &= ~xbox_64k_pages;
        pc_allocation_flags |= MEM_RESERVE;
        pc_protection_flags = PAGE_READWRITE | PAGE_WRITECOMBINE;
    } else if ((allow_observed_va_reservation || allow_unified_reservation) && address == NULL &&
        size == 0x0b900000u && allocation_flags == 0x00042000u &&
        attributes == 0x7u && protection_flags == PAGE_NOACCESS) {
        /* Unified mode is paired with debugger skips at the four split sites,
         * so the whole address range remains one Windows reservation. */
        if (allow_unified_reservation) {
            pc_allocation_flags = MEM_RESERVE;
            exact_unified_reserve = TRUE;
        } else {
            pc_allocation_flags = MEM_RESERVE | MEM_RESERVE_PLACEHOLDER;
            windows_allocation_flags |= MEM_RESERVE_PLACEHOLDER;
            exact_placeholder_reserve = TRUE;
        }
        pc_protection_flags = PAGE_NOACCESS;
    } else if ((allow_observed_va_reservation || allow_unified_reservation) && address != NULL &&
        allocation_flags == 0x20406000u && attributes == 0x27u &&
        ((size == 0x00a00000u && protection_flags == 0x1004u) ||
         (size == 0x00100000u && protection_flags == 0x1404u) ||
         (size == 0x01b00000u && protection_flags == 0x8404u) ||
         (size == 0x02f00000u && protection_flags == 0x2404u) ||
         (size == 0x06400000u && protection_flags == 0x2004u))) {
        /* Replace or commit only the five exact ranges observed in startup.
         * Unified mode commits inside the single reservation. */
        if (allow_unified_reservation) {
            const uintptr_t target = (uintptr_t)address;
            const uintptr_t base = (uintptr_t)g_xmem_unified_base;
            if (!g_xmem_unified_base || g_xmem_unified_size != 0x0b900000u ||
                target < base || target - base > g_xmem_unified_size ||
                size > g_xmem_unified_size - (target - base)) {
                error = ERROR_INVALID_ADDRESS;
            } else {
                pc_allocation_flags = MEM_COMMIT;
                exact_unified_commit = TRUE;
            }
        } else {
            pc_allocation_flags = MEM_RESERVE | MEM_REPLACE_PLACEHOLDER;
            windows_allocation_flags |= MEM_REPLACE_PLACEHOLDER;
            exact_placeholder_replace = TRUE;
        }
        pc_protection_flags = protection_flags & ~(0x1000u | 0x2000u | 0x8000u);
    } else if (attributes != 0x21u && attributes != 0x3u) {
        error = ERROR_CALL_NOT_IMPLEMENTED;
    } else if ((allocation_flags & xbox_64k_pages) == xbox_64k_pages) {
        pc_allocation_flags &= ~xbox_64k_pages;
        if ((size & 0xffffu) || ((uintptr_t)address & 0xffffu))
            error = ERROR_INVALID_PARAMETER;
    }
    /* winnt.h defines PAGE_GRAPHICS_NOACCESS as 0x800.  In the observed
     * 0x801 request, PAGE_NOACCESS is the separate CPU protection bit.
     */
    if (protection_flags == 0x801u)
        pc_protection_flags = PAGE_NOACCESS;
    if ((pc_allocation_flags & ~windows_allocation_flags) ||
        (pc_protection_flags & ~windows_protection_flags))
        error = ERROR_CALL_NOT_IMPLEMENTED;
    if (error == ERROR_SUCCESS) {
        SetLastError(ERROR_SUCCESS);
        if (exact_unified_reserve) {
            result = VirtualAlloc(address, size, pc_allocation_flags,
                pc_protection_flags);
            if (result) {
                g_xmem_unified_base = result;
                g_xmem_unified_size = size;
            }
        } else if (exact_unified_commit) {
            result = VirtualAlloc(address, size, pc_allocation_flags,
                pc_protection_flags);
        } else if (exact_placeholder_reserve) {
            SetLastError(ERROR_SUCCESS);
            result = compat_virtual_alloc2(address, size, pc_allocation_flags,
                pc_protection_flags);
        } else if (exact_placeholder_replace) {
            MEMORY_BASIC_INFORMATION mbi;
            SIZE_T queried = VirtualQuery(address, &mbi, sizeof(mbi));
            if (queried != sizeof(mbi) || mbi.BaseAddress != address ||
                mbi.RegionSize != size || mbi.State != MEM_RESERVE) {
                error = ERROR_INVALID_ADDRESS;
            } else {
                SetLastError(ERROR_SUCCESS);
                result = compat_virtual_alloc2(address, size,
                    pc_allocation_flags, pc_protection_flags);
            }
        } else {
            result = VirtualAlloc(address, size, pc_allocation_flags,
                pc_protection_flags);
        }
        if (error == ERROR_SUCCESS)
            error = GetLastError();
    }

    values[0] = (uintptr_t)address;
    values[1] = size;
    values[2] = allocation_flags;
    values[3] = attributes;
    values[4] = protection_flags;
    values[5] = pc_allocation_flags;
    values[6] = pc_protection_flags;
    values[7] = (uintptr_t)result;
    values[8] = error;
    while (*prefix) *cursor++ = *prefix++;
    for (unsigned i = 0; i != 9; ++i) {
        const char *label = labels[i];
        while (*label) *cursor++ = *label++;
        *cursor++ = '0';
        *cursor++ = 'x';
        for (int shift = 60; shift >= 0; shift -= 4)
            *cursor++ = "0123456789abcdef"[(values[i] >> shift) & 0xfu];
    }
    *cursor++ = '\n';
    *cursor = '\0';
    OutputDebugStringA(message);
    SetLastError(error);
    return result;
}

__declspec(dllexport) size_t WINAPI XMemVirtualQuery(const void *address, void *buffer, size_t length)
{
    return VirtualQuery(address, (PMEMORY_BASIC_INFORMATION)buffer, length);
}

__declspec(dllexport) BOOL WINAPI XMemMapPhysicalPagesScatter(void *address, size_t count, void *pages, unsigned flags)
{
    (void)address;
    (void)count;
    (void)pages;
    (void)flags;
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

__declspec(dllexport) void WINAPI XMemTransferMemory(void *destination, const void *source, size_t size, unsigned flags)
{
    (void)flags;
    if (destination && source && size) memcpy(destination, source, size);
}

/* The Xbox API represents physical pages as an array of ULONG_PTR values.
 * On Windows, each value below is the address of one reserved backing-page
 * slot. The game's virtual reservation remains separate; MapPhysicalPages
 * copies page contents into and out of that reservation. This models CPU
 * visibility and page lifetime, not Xbox GPU aliases or coherency.
 */
#define XMEM_COMPAT_64K_PAGE_FLAG 0x200u
#define XMEM_COMPAT_2MB_CLUSTER_FLAG 0x080u
#define XMEM_COMPAT_MAX_PAGES 0x100000u

typedef struct XmemPhysicalBlock {
    struct XmemPhysicalBlock *next;
    unsigned char *base;
    SIZE_T page_size;
    SIZE_T page_count;
    SIZE_T live_count;
    unsigned char *live;
    void **mapped_at;
} XmemPhysicalBlock;

static SRWLOCK g_xmem_physical_lock = SRWLOCK_INIT;
static XmemPhysicalBlock *g_xmem_physical_blocks;

static XmemPhysicalBlock *xmem_find_page_locked(ULONG_PTR page, SIZE_T *index_out)
{
    XmemPhysicalBlock *block;
    for (block = g_xmem_physical_blocks; block; block = block->next) {
        ULONG_PTR base = (ULONG_PTR)block->base;
        ULONG_PTR end = base + block->page_size * block->page_count;
        if (page >= base && page < end && ((page - base) % block->page_size) == 0) {
            SIZE_T index = (SIZE_T)((page - base) / block->page_size);
            if (block->live[index]) {
                if (index_out) *index_out = index;
                return block;
            }
        }
    }
    return NULL;
}

static BOOL xmem_commit_backing_page(XmemPhysicalBlock *block, SIZE_T index)
{
    void *page = block->base + index * block->page_size;
    return VirtualAlloc(page, block->page_size, MEM_COMMIT, PAGE_READWRITE) == page;
}

__declspec(dllexport) BOOL WINAPI XMemAllocatePhysicalPages(ULONG_PTR flags,
    ULONG_PTR *inout_page_count, ULONG_PTR *page_array)
{
    SIZE_T page_size, page_count, bytes;
    XmemPhysicalBlock *block;
    char message[320];
    if (!inout_page_count) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    page_count = (SIZE_T)*inout_page_count;
    if (!page_count) return TRUE;
    if (!page_array || page_count > XMEM_COMPAT_MAX_PAGES) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (flags == (XMEM_COMPAT_64K_PAGE_FLAG | 0x080u)) {
        page_size = 0x10000u;
    } else if (flags == XMEM_COMPAT_2MB_CLUSTER_FLAG) {
        page_size = 0x200000u;
    } else {
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    if (page_count > ((SIZE_T)-1) / page_size) {
        SetLastError(ERROR_ARITHMETIC_OVERFLOW);
        return FALSE;
    }
    bytes = page_count * page_size;
    block = (XmemPhysicalBlock *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*block));
    if (!block) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    block->live = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, page_count);
    block->mapped_at = (void **)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
        page_count * sizeof(*block->mapped_at));
    block->base = (unsigned char *)VirtualAlloc(NULL, bytes, MEM_RESERVE, PAGE_NOACCESS);
    if (!block->live || !block->mapped_at || !block->base) {
        DWORD error = GetLastError();
        if (block->base) VirtualFree(block->base, 0, MEM_RELEASE);
        if (block->mapped_at) HeapFree(GetProcessHeap(), 0, block->mapped_at);
        if (block->live) HeapFree(GetProcessHeap(), 0, block->live);
        HeapFree(GetProcessHeap(), 0, block);
        SetLastError(error ? error : ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    block->page_size = page_size;
    block->page_count = page_count;
    block->live_count = page_count;
    memset(block->live, 1, page_count);

    AcquireSRWLockExclusive(&g_xmem_physical_lock);
    block->next = g_xmem_physical_blocks;
    g_xmem_physical_blocks = block;
    for (SIZE_T i = 0; i < page_count; ++i)
        page_array[i] = (ULONG_PTR)(block->base + i * page_size);
    ReleaseSRWLockExclusive(&g_xmem_physical_lock);
    snprintf(message, sizeof(message),
        "[xmem] XMemAllocatePhysicalPages flags=0x%llx page_size=0x%llx count=%llu bytes=0x%llx array=%p backing=0x%p first=0x%llx last=0x%llx\n",
        (unsigned long long)flags, (unsigned long long)page_size,
        (unsigned long long)page_count, (unsigned long long)bytes, page_array,
        block->base, (unsigned long long)page_array[0],
        (unsigned long long)page_array[page_count - 1]);
    OutputDebugStringA(message);
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

__declspec(dllexport) BOOL WINAPI XMemFreePhysicalPages(ULONG_PTR page_count,
    ULONG_PTR *page_array)
{
    SIZE_T count = (SIZE_T)page_count;
    if (!count) return TRUE;
    if (!page_array || count > XMEM_COMPAT_MAX_PAGES) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    AcquireSRWLockExclusive(&g_xmem_physical_lock);
    for (SIZE_T i = 0; i < count; ++i) {
        SIZE_T index;
        XmemPhysicalBlock *block = xmem_find_page_locked(page_array[i], &index);
        if (!block || block->mapped_at[index]) {
            ReleaseSRWLockExclusive(&g_xmem_physical_lock);
            SetLastError(ERROR_INVALID_ADDRESS);
            return FALSE;
        }
        for (SIZE_T j = 0; j < i; ++j) {
            if (page_array[j] == page_array[i]) {
                ReleaseSRWLockExclusive(&g_xmem_physical_lock);
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
        }
    }
    for (SIZE_T i = 0; i < count; ++i) {
        SIZE_T index;
        XmemPhysicalBlock *block = xmem_find_page_locked(page_array[i], &index);
        block->live[index] = 0;
        --block->live_count;
        if (!block->live_count) {
            XmemPhysicalBlock **link = &g_xmem_physical_blocks;
            while (*link && *link != block) link = &(*link)->next;
            if (*link) *link = block->next;
            VirtualFree(block->base, 0, MEM_RELEASE);
            HeapFree(GetProcessHeap(), 0, block->mapped_at);
            HeapFree(GetProcessHeap(), 0, block->live);
            HeapFree(GetProcessHeap(), 0, block);
        }
    }
    ReleaseSRWLockExclusive(&g_xmem_physical_lock);
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

__declspec(dllexport) void *WINAPI XMemMapPhysicalPages(void *address,
    ULONG_PTR page_count, ULONG_PTR *page_array)
{
    SIZE_T count = (SIZE_T)page_count;
    SIZE_T page_size = 0;
    XmemPhysicalBlock *first_block = NULL;
    SIZE_T first_index = 0;
    BOOL contiguous = TRUE;
    void *backing_base = NULL;
    ULONG_PTR first_page = 0, last_page = 0;
    if (!address || !count || count > XMEM_COMPAT_MAX_PAGES ||
        ((ULONG_PTR)address & 0xffffu)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    AcquireSRWLockExclusive(&g_xmem_physical_lock);
    if (page_array) {
        for (SIZE_T i = 0; i < count; ++i) {
            SIZE_T index;
            XmemPhysicalBlock *block = xmem_find_page_locked(page_array[i], &index);
            if (!block || (page_size && block->page_size != page_size)) {
                ReleaseSRWLockExclusive(&g_xmem_physical_lock);
                SetLastError(ERROR_INVALID_ADDRESS);
                return NULL;
            }
            if (!i) {
                first_block = block;
                first_index = index;
            } else if (block != first_block || index != first_index + i) {
                contiguous = FALSE;
            }
            if (!i) first_page = page_array[i];
            last_page = page_array[i];
            page_size = block->page_size;
        }
        if (!page_size || ((ULONG_PTR)address % page_size)) {
            ReleaseSRWLockExclusive(&g_xmem_physical_lock);
            SetLastError(ERROR_INVALID_PARAMETER);
            return NULL;
        }
        /* Save and detach pages currently occupying the requested address. */
        for (SIZE_T i = 0; i < count; ++i) {
            void *target = (unsigned char *)address + i * page_size;
            XmemPhysicalBlock *block;
            for (block = g_xmem_physical_blocks; block; block = block->next) {
                for (SIZE_T j = 0; j < block->page_count; ++j) {
                    if (block->live[j] && block->mapped_at[j] == target) {
                        if (xmem_commit_backing_page(block, j))
                            memcpy(block->base + j * block->page_size, target, page_size);
                        block->mapped_at[j] = NULL;
                        VirtualFree(target, page_size, MEM_DECOMMIT);
                        break;
                    }
                }
            }
        }
        /* A physical page can occupy only one virtual location in this model. */
        for (SIZE_T i = 0; i < count; ++i) {
            SIZE_T index;
            XmemPhysicalBlock *block = xmem_find_page_locked(page_array[i], &index);
            if (block->mapped_at[index]) {
                void *old = block->mapped_at[index];
                if (xmem_commit_backing_page(block, index))
                    memcpy(block->base + index * page_size, old, page_size);
                block->mapped_at[index] = NULL;
                VirtualFree(old, page_size, MEM_DECOMMIT);
            }
        }
        for (SIZE_T i = 0; i < count; ++i) {
            SIZE_T index;
            XmemPhysicalBlock *block = xmem_find_page_locked(page_array[i], &index);
            void *target = (unsigned char *)address + i * page_size;
            if (!xmem_commit_backing_page(block, index) ||
                !VirtualAlloc(target, page_size, MEM_COMMIT, PAGE_READWRITE)) {
                ReleaseSRWLockExclusive(&g_xmem_physical_lock);
                SetLastError(ERROR_NOT_ENOUGH_MEMORY);
                return NULL;
            }
            memcpy(target, block->base + index * page_size, page_size);
            block->mapped_at[index] = target;
        }
        backing_base = first_block ? first_block->base : NULL;
    } else {
        /* A null page array unmaps the current physical pages in this range. */
        for (XmemPhysicalBlock *block = g_xmem_physical_blocks; block && !page_size; block = block->next) {
            for (SIZE_T j = 0; j < block->page_count; ++j) {
                if (block->live[j] && block->mapped_at[j] == address) {
                    page_size = block->page_size;
                    break;
                }
            }
        }
        if (!page_size) page_size = 0x10000u;
        for (SIZE_T i = 0; i < count; ++i) {
            void *target = (unsigned char *)address + i * page_size;
            for (XmemPhysicalBlock *block = g_xmem_physical_blocks; block; block = block->next) {
                for (SIZE_T j = 0; j < block->page_count; ++j) {
                    if (block->live[j] && block->mapped_at[j] == target) {
                        if (xmem_commit_backing_page(block, j))
                            memcpy(block->base + j * block->page_size, target, block->page_size);
                        block->mapped_at[j] = NULL;
                        break;
                    }
                }
            }
            VirtualFree(target, page_size, MEM_DECOMMIT);
        }
    }
    ReleaseSRWLockExclusive(&g_xmem_physical_lock);
    if (page_array) {
        char message[384];
        snprintf(message, sizeof(message),
            "[xmem] XMemMapPhysicalPages kind=map target=0x%p count=%llu page_size=0x%llx array=%p backing=0x%p first_index=%llu last_index=%llu contiguous=%u first_page=0x%llx last_page=0x%llx\n",
            address, (unsigned long long)count, (unsigned long long)page_size,
            page_array, backing_base,
            (unsigned long long)first_index,
            (unsigned long long)(first_index + count - 1), contiguous,
            (unsigned long long)first_page,
            (unsigned long long)last_page);
        OutputDebugStringA(message);
    } else {
        char message[192];
        snprintf(message, sizeof(message),
            "[xmem] XMemMapPhysicalPages kind=unmap target=0x%p count=%llu page_size=0x%llx\n",
            address, (unsigned long long)count, (unsigned long long)page_size);
        OutputDebugStringA(message);
    }
    SetLastError(ERROR_SUCCESS);
    return address;
}

__declspec(dllexport) void *WINAPI XMemGetWorkingSetStatistics(void *statistics, unsigned flags)
{
    (void)statistics;
    (void)flags;
    return NULL;
}

__declspec(dllexport) BOOL WINAPI XMemIsToolsMemAvailable(void)
{
    return FALSE;
}

__declspec(dllexport) HRESULT WINAPI D3D12ConfigureVirtualMemoryX(void *configuration)
{
    (void)configuration;
    return S_OK;
}

typedef HRESULT (WINAPI *compat_d3d12_serialize_root_signature_fn)(const void *description, void **blob, void **error_blob);

static BOOL compat_d3d12_bvh_translation_requested(void)
{
    char enabled[2];
    return GetEnvironmentVariableA("COMPAT_D3D12_XBOX_BVH_PIPELINES", enabled,
        sizeof(enabled)) == 1 && enabled[0] == '1';
}

__declspec(dllexport) HRESULT WINAPI D3D12SerializeVersionedRootSignature(const void *description, void **blob, void **error_blob)
{
    HMODULE module = LoadLibraryW(L"d3d12.dll");
    compat_d3d12_serialize_root_signature_fn function = module
        ? (compat_d3d12_serialize_root_signature_fn)GetProcAddress(module, "D3D12SerializeVersionedRootSignature")
        : NULL;
    if (!function) {
        if (blob) *blob = NULL;
        if (error_blob) *error_blob = NULL;
        return E_NOTIMPL;
    }
    HMODULE bvh = GetModuleHandleW(L"XboxBvhBuilder.dll");
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc = description;
    if (!bvh || (uintptr_t)_ReturnAddress() != (uintptr_t)bvh + 0xc335 ||
        !desc || !blob || error_blob || desc->Version != D3D_ROOT_SIGNATURE_VERSION_1_1 ||
        (UINT)desc->Desc_1_1.Flags != 0x03000000u || !compat_d3d12_bvh_translation_requested())
        return function(description, blob, error_blob);

    /* XboxBvhBuilder's helper adds these two Xbox flags itself. Desktop
     * D3D12 rejects them; leave the parameters and the caller's copy intact. */
    void *original_error = NULL, *original_blob = NULL;
    HRESULT original = function(description, &original_blob, &original_error);
    HRESULT result = original;
    *blob = original_blob;
    if (original == E_INVALIDARG && !original_blob) {
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC translated = *desc;
        translated.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        result = function(&translated, blob, NULL);
    }
    char message[345];
    ID3DBlob *detail = original_error;
    const char *detail_data = detail ? ID3D10Blob_GetBufferPointer(detail) : "";
    SIZE_T detail_size = detail ? ID3D10Blob_GetBufferSize(detail) : 0;
    snprintf(message, sizeof(message),
        "[graphics] XboxBvhBuilder root signature flags=0x03000000 original_hr=0x%08lx "
        "final_hr=0x%08lx parameters=%u samplers=%u diagnostic=%.*s\n",
        (unsigned long)original, (unsigned long)result, desc->Desc_1_1.NumParameters,
        desc->Desc_1_1.NumStaticSamplers, (int)(detail_size > 160 ? 160 : detail_size), detail_data);
    OutputDebugStringA(message);
    if (detail) ID3D10Blob_Release(detail);
    return result;
}

/* D3D12XboxCreateDevice returns an Xbox-facing COM object.  Its ordinary
 * device methods have the same argument order as Windows, but large
 * structure-return methods use the Xbox order (return buffer, this) while
 * Windows COM uses (this, return buffer).  A cloned vtable with tiny x64
 * forwarding thunks lets us adapt those calls without changing the game. */
#define COMPAT_D3D12_DEVICE11_VTABLE_SLOTS 134
#define COMPAT_D3D12_THUNK_STRIDE 32
#define COMPAT_D3D12_SLOT_GET_RESOURCE_ALLOCATION_INFO 25
#define COMPAT_D3D12_SLOT_GET_CUSTOM_HEAP_PROPERTIES 26
#define COMPAT_D3D12_SLOT_GET_RESOURCE_ALLOCATION_INFO1 56
#define COMPAT_D3D12_SLOT_GET_RESOURCE_ALLOCATION_INFO2 68
#define COMPAT_D3D12_XBOX_SLOT_CREATE_COMMITTED_RESOURCE3 (0x3f8 / sizeof(void *))
#define COMPAT_D3D12_DEVICE11_SLOT_CREATE_COMMITTED_RESOURCE3 76

typedef struct CompatD3D12DeviceProxy {
    void **vtable;
    void *native_device;
    volatile LONG references;
} CompatD3D12DeviceProxy;

static HRESULT STDMETHODCALLTYPE compat_d3d12_bvh_compute_pipeline_slot1f0(
    CompatD3D12DeviceProxy *proxy, const void *desc, uintptr_t extra0, uintptr_t extra1,
    const GUID *iid, void **pipeline)
{
    HMODULE bvh = GetModuleHandleW(L"XboxBvhBuilder.dll");
    if (!bvh || (uintptr_t)_ReturnAddress() != (uintptr_t)bvh + 0xc4e4) {
        /* Other callers of this slot keep the Windows Device5 ABI. */
        return ID3D12Device5_CreateStateObject((ID3D12Device5 *)proxy->native_device,
            desc, (const GUID *)extra0, (void **)extra1);
    }
    static const GUID iid_pipeline_state = {
        0x765a30f3, 0xf624, 0x4c6f, {0xa8,0x28,0xac,0xe9,0x48,0x62,0x24,0x45}
    };
    if (extra0 || extra1 || !desc || !iid || !pipeline || !IsEqualGUID(iid, &iid_pipeline_state))
        return E_NOTIMPL;
    const D3D12_COMPUTE_PIPELINE_STATE_DESC *compute = desc;
    HRESULT result = ID3D12Device_CreateComputePipelineState((ID3D12Device *)proxy->native_device,
        compute, iid, pipeline);
    char message[220];
    snprintf(message, sizeof(message),
        "[graphics] XboxBvhBuilder compute pipeline slot +0x1f0->+0x58 shader_bytes=0x%llx hr=0x%08lx\n",
        (unsigned long long)compute->CS.BytecodeLength, (unsigned long)result);
    OutputDebugStringA(message);
    return result;
}

typedef struct CompatD3D12CommandQueueDesc {
    int32_t type;
    int32_t priority;
    uint32_t flags;
    uint32_t node_mask;
} CompatD3D12CommandQueueDesc;

typedef struct CompatD3D12HeapDesc {
    uint64_t size_in_bytes;
    int32_t type;
    int32_t cpu_page_property;
    int32_t memory_pool_preference;
    uint32_t creation_node_mask;
    uint32_t visible_node_mask;
    uint32_t flags;
    uint64_t alignment;
} CompatD3D12HeapDesc;

static SRWLOCK g_d3d12_device_proxy_lock = SRWLOCK_INIT;
static void **g_d3d12_device11_vtable;
static unsigned char *g_d3d12_device11_thunks;

static void compat_write_u32(unsigned char **cursor, uint32_t value)
{
    (*cursor)[0] = (unsigned char)value;
    (*cursor)[1] = (unsigned char)(value >> 8);
    (*cursor)[2] = (unsigned char)(value >> 16);
    (*cursor)[3] = (unsigned char)(value >> 24);
    *cursor += 4;
}

#define COMPAT_D3D12_DESCRIPTOR_HEAP_VTABLE_SLOTS 11
#define COMPAT_D3D12_DESCRIPTOR_HEAP_THUNK_STRIDE 32

typedef struct CompatD3D12DescriptorHeapProxy {
    void **vtable;
    void *native_heap;
    CompatD3D12DeviceProxy *device_proxy;
    volatile LONG references;
} CompatD3D12DescriptorHeapProxy;

static SRWLOCK g_d3d12_descriptor_heap_proxy_lock = SRWLOCK_INIT;
static void **g_d3d12_descriptor_heap_proxy_vtable;
static unsigned char *g_d3d12_descriptor_heap_proxy_thunks;

static HRESULT STDMETHODCALLTYPE compat_d3d12_proxy_query_interface(
    CompatD3D12DeviceProxy *proxy, const GUID *iid, void **object);
static ULONG STDMETHODCALLTYPE compat_d3d12_proxy_add_ref(
    CompatD3D12DeviceProxy *proxy);
static ULONG STDMETHODCALLTYPE compat_d3d12_proxy_release(
    CompatD3D12DeviceProxy *proxy);
static HRESULT STDMETHODCALLTYPE compat_d3d12_descriptor_heap_get_device(
    CompatD3D12DescriptorHeapProxy *proxy, const GUID *iid, void **device);
static HRESULT STDMETHODCALLTYPE compat_d3d12_descriptor_heap_query_interface(
    CompatD3D12DescriptorHeapProxy *proxy, const GUID *iid, void **object);
static ULONG STDMETHODCALLTYPE compat_d3d12_descriptor_heap_add_ref(
    CompatD3D12DescriptorHeapProxy *proxy);
static ULONG STDMETHODCALLTYPE compat_d3d12_descriptor_heap_release(
    CompatD3D12DescriptorHeapProxy *proxy);
static D3D12_DESCRIPTOR_HEAP_DESC *STDMETHODCALLTYPE
compat_d3d12_descriptor_heap_get_desc(
    D3D12_DESCRIPTOR_HEAP_DESC *description,
    CompatD3D12DescriptorHeapProxy *proxy);
static SIZE_T STDMETHODCALLTYPE compat_d3d12_descriptor_heap_get_cpu_handle(
    CompatD3D12DescriptorHeapProxy *proxy);
static SIZE_T STDMETHODCALLTYPE compat_d3d12_descriptor_heap_get_gpu_handle(
    CompatD3D12DescriptorHeapProxy *proxy);
static HRESULT STDMETHODCALLTYPE compat_d3d12_proxy_create_descriptor_heap(
    CompatD3D12DeviceProxy *proxy,
    const D3D12_DESCRIPTOR_HEAP_DESC *description,
    const GUID *iid, void **heap_out);

static BOOL compat_d3d12_descriptor_heap_proxy_init(void *native_heap)
{
    SYSTEM_INFO system_info;
    SIZE_T code_size;
    DWORD old_protection;
    unsigned char *code;
    void **vtable;
    void **native_vtable;

    AcquireSRWLockExclusive(&g_d3d12_descriptor_heap_proxy_lock);
    if (g_d3d12_descriptor_heap_proxy_vtable) {
        ReleaseSRWLockExclusive(&g_d3d12_descriptor_heap_proxy_lock);
        return TRUE;
    }
    if (!native_heap) {
        ReleaseSRWLockExclusive(&g_d3d12_descriptor_heap_proxy_lock);
        return FALSE;
    }

    GetSystemInfo(&system_info);
    code_size = (COMPAT_D3D12_DESCRIPTOR_HEAP_VTABLE_SLOTS *
        COMPAT_D3D12_DESCRIPTOR_HEAP_THUNK_STRIDE + system_info.dwPageSize - 1) &
        ~((SIZE_T)system_info.dwPageSize - 1);
    code = (unsigned char *)VirtualAlloc(NULL, code_size,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    vtable = (void **)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
        COMPAT_D3D12_DESCRIPTOR_HEAP_VTABLE_SLOTS * sizeof(*vtable));
    if (!code || !vtable) {
        if (code) VirtualFree(code, 0, MEM_RELEASE);
        if (vtable) HeapFree(GetProcessHeap(), 0, vtable);
        ReleaseSRWLockExclusive(&g_d3d12_descriptor_heap_proxy_lock);
        return FALSE;
    }

    native_vtable = *(void ***)native_heap;
    memcpy(vtable, native_vtable,
        COMPAT_D3D12_DESCRIPTOR_HEAP_VTABLE_SLOTS * sizeof(*vtable));
    for (unsigned slot = 3; slot < 8; ++slot) {
        unsigned char *cursor = code + slot * COMPAT_D3D12_DESCRIPTOR_HEAP_THUNK_STRIDE;
        *cursor++ = 0x48; *cursor++ = 0x8b; *cursor++ = 0x49; *cursor++ = 0x08;
        *cursor++ = 0x48; *cursor++ = 0x8b; *cursor++ = 0x01;
        *cursor++ = 0xff; *cursor++ = 0xa0;
        compat_write_u32(&cursor, slot * (uint32_t)sizeof(void *));
        vtable[slot] = code + slot * COMPAT_D3D12_DESCRIPTOR_HEAP_THUNK_STRIDE;
    }
    vtable[0] = (void *)compat_d3d12_descriptor_heap_query_interface;
    vtable[1] = (void *)compat_d3d12_descriptor_heap_add_ref;
    vtable[2] = (void *)compat_d3d12_descriptor_heap_release;
    vtable[7] = (void *)compat_d3d12_descriptor_heap_get_device;
    vtable[8] = (void *)compat_d3d12_descriptor_heap_get_desc;
    vtable[9] = (void *)compat_d3d12_descriptor_heap_get_cpu_handle;
    vtable[10] = (void *)compat_d3d12_descriptor_heap_get_gpu_handle;
    if (!VirtualProtect(code, code_size, PAGE_EXECUTE_READ, &old_protection)) {
        VirtualFree(code, 0, MEM_RELEASE);
        HeapFree(GetProcessHeap(), 0, vtable);
        ReleaseSRWLockExclusive(&g_d3d12_descriptor_heap_proxy_lock);
        return FALSE;
    }
    FlushInstructionCache(GetCurrentProcess(), code, code_size);
    g_d3d12_descriptor_heap_proxy_thunks = code;
    g_d3d12_descriptor_heap_proxy_vtable = vtable;
    ReleaseSRWLockExclusive(&g_d3d12_descriptor_heap_proxy_lock);
    return TRUE;
}

static CompatD3D12DescriptorHeapProxy *compat_d3d12_descriptor_heap_proxy_create(
    void *native_heap, CompatD3D12DeviceProxy *device_proxy)
{
    CompatD3D12DescriptorHeapProxy *proxy;
    if (!native_heap || !device_proxy ||
        !compat_d3d12_descriptor_heap_proxy_init(native_heap))
        return NULL;
    proxy = (CompatD3D12DescriptorHeapProxy *)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*proxy));
    if (!proxy) return NULL;
    proxy->vtable = g_d3d12_descriptor_heap_proxy_vtable;
    proxy->native_heap = native_heap;
    proxy->device_proxy = device_proxy;
    compat_d3d12_proxy_add_ref(device_proxy);
    proxy->references = 1;
    return proxy;
}

static HRESULT STDMETHODCALLTYPE compat_d3d12_descriptor_heap_get_device(
    CompatD3D12DescriptorHeapProxy *proxy, const GUID *iid, void **device)
{
    typedef HRESULT (STDMETHODCALLTYPE *get_device_fn)(void *, const GUID *, void **);
    typedef ULONG (STDMETHODCALLTYPE *release_fn)(void *);
    void *native_device = NULL;
    HRESULT result;
    if (!proxy || !iid || !device) return E_POINTER;
    *device = NULL;
    result = ((get_device_fn)(*(void ***)proxy->native_heap)[7])(
        proxy->native_heap, iid, &native_device);
    if (FAILED(result)) return result;
    if (native_device)
        ((release_fn)(*(void ***)native_device)[2])(native_device);
    return compat_d3d12_proxy_query_interface(proxy->device_proxy, iid, device);
}

static HRESULT STDMETHODCALLTYPE compat_d3d12_descriptor_heap_query_interface(
    CompatD3D12DescriptorHeapProxy *proxy, const GUID *iid, void **object)
{
    typedef HRESULT (STDMETHODCALLTYPE *query_interface_fn)(void *, const GUID *, void **);
    typedef ULONG (STDMETHODCALLTYPE *release_fn)(void *);
    void *queried = NULL;
    HRESULT result;
    if (!proxy || !iid || !object) return E_POINTER;
    *object = NULL;
    result = ((query_interface_fn)(*(void ***)proxy->native_heap)[0])(
        proxy->native_heap, iid, &queried);
    if (FAILED(result) || !queried) return FAILED(result) ? result : E_NOINTERFACE;
    ((release_fn)(*(void ***)queried)[2])(queried);
    InterlockedIncrement(&proxy->references);
    *object = proxy;
    return S_OK;
}

static ULONG STDMETHODCALLTYPE compat_d3d12_descriptor_heap_add_ref(
    CompatD3D12DescriptorHeapProxy *proxy)
{
    return proxy ? (ULONG)InterlockedIncrement(&proxy->references) : 0;
}

static ULONG STDMETHODCALLTYPE compat_d3d12_descriptor_heap_release(
    CompatD3D12DescriptorHeapProxy *proxy)
{
    LONG references;
    if (!proxy) return 0;
    references = InterlockedDecrement(&proxy->references);
    if (!references) {
        typedef ULONG (STDMETHODCALLTYPE *release_fn)(void *);
        ((release_fn)(*(void ***)proxy->native_heap)[2])(proxy->native_heap);
        compat_d3d12_proxy_release(proxy->device_proxy);
        HeapFree(GetProcessHeap(), 0, proxy);
    }
    return references < 0 ? 0 : (ULONG)references;
}

static D3D12_DESCRIPTOR_HEAP_DESC *STDMETHODCALLTYPE
compat_d3d12_descriptor_heap_get_desc(
    D3D12_DESCRIPTOR_HEAP_DESC *description,
    CompatD3D12DescriptorHeapProxy *proxy)
{
    typedef D3D12_DESCRIPTOR_HEAP_DESC *(STDMETHODCALLTYPE *get_desc_fn)(
        void *, D3D12_DESCRIPTOR_HEAP_DESC *);
    if (!description || !proxy) return NULL;
    return ((get_desc_fn)(*(void ***)proxy->native_heap)[8])(
        proxy->native_heap, description);
}

static SIZE_T STDMETHODCALLTYPE compat_d3d12_descriptor_heap_get_cpu_handle(
    CompatD3D12DescriptorHeapProxy *proxy)
{
    typedef D3D12_CPU_DESCRIPTOR_HANDLE *(STDMETHODCALLTYPE *get_handle_fn)(
        void *, D3D12_CPU_DESCRIPTOR_HANDLE *);
    D3D12_CPU_DESCRIPTOR_HANDLE handle = {0};
    if (!proxy) return 0;
    ((get_handle_fn)(*(void ***)proxy->native_heap)[9])(
        proxy->native_heap, &handle);
    return handle.ptr;
}

static SIZE_T STDMETHODCALLTYPE compat_d3d12_descriptor_heap_get_gpu_handle(
    CompatD3D12DescriptorHeapProxy *proxy)
{
    typedef D3D12_GPU_DESCRIPTOR_HANDLE *(STDMETHODCALLTYPE *get_handle_fn)(
        void *, D3D12_GPU_DESCRIPTOR_HANDLE *);
    D3D12_GPU_DESCRIPTOR_HANDLE handle = {0};
    if (!proxy) return 0;
    ((get_handle_fn)(*(void ***)proxy->native_heap)[10])(
        proxy->native_heap, &handle);
    return handle.ptr;
}

static HRESULT STDMETHODCALLTYPE compat_d3d12_proxy_create_descriptor_heap(
    CompatD3D12DeviceProxy *proxy,
    const D3D12_DESCRIPTOR_HEAP_DESC *description,
    const GUID *iid, void **heap_out)
{
    static const GUID iid_d3d12_descriptor_heap = {
        0x8efb471d, 0x616c, 0x4f49, {0x90,0xf7,0x12,0x7b,0xb7,0x63,0xfa,0x51}
    };
    typedef HRESULT (STDMETHODCALLTYPE *create_descriptor_heap_fn)(
        void *, const D3D12_DESCRIPTOR_HEAP_DESC *, const GUID *, void **);
    typedef ULONG (STDMETHODCALLTYPE *release_fn)(void *);
    void *native_heap = NULL;
    CompatD3D12DescriptorHeapProxy *heap_proxy;
    D3D12_DESCRIPTOR_HEAP_DESC retry_description;
    HRESULT result;
    HRESULT initial_result;
    char message[256];
    char clamp_value[8];
    DWORD clamp_length;
    UINT native_count;
    BOOL retry_attempted = FALSE;

    if (!proxy || !description || !iid || !heap_out) return E_POINTER;
    *heap_out = NULL;
    native_count = description->NumDescriptors;
    result = ((create_descriptor_heap_fn)(*(void ***)proxy->native_device)[14])(
        proxy->native_device, description, iid, &native_heap);
    initial_result = result;
    clamp_length = GetEnvironmentVariableA(
        "COMPAT_D3D12_CLAMP_OBSERVED_DESCRIPTOR_HEAP",
        clamp_value, sizeof(clamp_value));
    if (result == E_INVALIDARG && clamp_length == 1 && clamp_value[0] == '1' &&
        IsEqualGUID(iid, &iid_d3d12_descriptor_heap) &&
        description->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
        description->NumDescriptors == 1245184 &&
        description->Flags == D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE &&
        description->NodeMask == 1) {
        if (native_heap) {
            ((release_fn)(*(void ***)native_heap)[2])(native_heap);
            native_heap = NULL;
        }
        retry_description = *description;
        retry_description.NumDescriptors = 1000000;
        native_count = retry_description.NumDescriptors;
        retry_attempted = TRUE;
        result = ((create_descriptor_heap_fn)(*(void ***)proxy->native_device)[14])(
            proxy->native_device, &retry_description, iid, &native_heap);
    }
    if (FAILED(result)) goto done;
    if (!native_heap) {
        result = E_UNEXPECTED;
        goto done;
    }
    if (!IsEqualGUID(iid, &iid_d3d12_descriptor_heap)) {
        *heap_out = native_heap;
        goto done;
    }
    heap_proxy = compat_d3d12_descriptor_heap_proxy_create(native_heap, proxy);
    if (!heap_proxy) {
        ((release_fn)(*(void ***)native_heap)[2])(native_heap);
        native_heap = NULL;
        result = E_OUTOFMEMORY;
        goto done;
    }
    *heap_out = heap_proxy;
done:
    if (FAILED(result) && native_heap) {
        ((release_fn)(*(void ***)native_heap)[2])(native_heap);
        native_heap = NULL;
    }
    snprintf(message, sizeof(message),
        "[graphics] CreateDescriptorHeap type=%ld requested=%lu nativeCount=%lu flags=0x%lx node=0x%lx iid=%p initial=0x%08lx result=0x%08lx retried=%u native=%p proxy=%p\n",
        (long)description->Type, (unsigned long)description->NumDescriptors,
        (unsigned long)native_count,
        (unsigned long)description->Flags, (unsigned long)description->NodeMask,
        (const void *)iid, (unsigned long)initial_result, (unsigned long)result,
        (unsigned)retry_attempted, native_heap, *heap_out);
    OutputDebugStringA(message);
    return result;
}

static HRESULT STDMETHODCALLTYPE compat_d3d12_proxy_query_interface(
    CompatD3D12DeviceProxy *proxy, const GUID *iid, void **object)
{
    static const GUID iid_iunknown = {
        0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}
    };
    static const GUID device_iids[] = {
        {0x189819f1, 0x1db6, 0x4b57, {0xbe,0x54,0x18,0x21,0x33,0x9b,0x85,0xf7}},
        {0x77acce80, 0x638e, 0x4e65, {0x88,0x95,0xc1,0xf2,0x33,0x86,0x86,0x3e}},
        {0x30baa41e, 0xb15b, 0x475c, {0xa0,0xbb,0x1a,0xf5,0xc5,0xb6,0x43,0x28}},
        {0x81dadc15, 0x2bad, 0x4392, {0x93,0xc5,0x10,0x13,0x45,0xc4,0xaa,0x98}},
        {0xe865df17, 0xa9ee, 0x46f9, {0xa4,0x63,0x30,0x98,0x31,0x5a,0xa2,0xe5}},
        {0x8b4f173b, 0x2fea, 0x4b80, {0x8f,0x58,0x43,0x07,0x19,0x1a,0xb9,0x5d}},
        {0xc70b221b, 0x40e4, 0x4a17, {0x89,0xaf,0x02,0x5a,0x07,0x27,0xa6,0xdc}},
        {0x5c014b53, 0x68a1, 0x4b9b, {0x8b,0xd1,0xdd,0x60,0x46,0xb9,0x35,0x8b}},
        {0x9218e6bb, 0xf944, 0x4f7e, {0xa7,0x5c,0xb1,0xb2,0xc7,0xb7,0x01,0xf3}},
        {0x4c80e962, 0xf032, 0x4f60, {0xbc,0x9e,0xeb,0xc2,0xcf,0xa1,0xd8,0x3c}},
        {0x517f8718, 0xaa66, 0x49f9, {0xb0,0x2b,0xa7,0xab,0x89,0xc0,0x60,0x31}},
        {0x5405c344, 0xd457, 0x444e, {0xb4,0xdd,0x23,0x66,0xe4,0x5a,0xee,0x39}}
    };
    typedef HRESULT (STDMETHODCALLTYPE *query_interface_fn)(void *, const GUID *, void **);
    typedef ULONG (STDMETHODCALLTYPE *release_fn)(void *);
    void **native_vtable;
    void *queried = NULL;
    BOOL supported = FALSE;
    HRESULT result;

    if (!proxy || !iid || !object) return E_POINTER;
    *object = NULL;
    if (!IsEqualGUID(iid, &iid_iunknown)) {
        for (size_t i = 0; i < sizeof(device_iids) / sizeof(device_iids[0]); ++i) {
            if (IsEqualGUID(iid, &device_iids[i])) {
                supported = TRUE;
                break;
            }
        }
        if (!supported) return E_NOINTERFACE;
    }

    native_vtable = *(void ***)proxy->native_device;
    result = ((query_interface_fn)native_vtable[0])(proxy->native_device, iid, &queried);
    if (FAILED(result)) return result;
    if (!queried) return E_NOINTERFACE;
    ((release_fn)(*(void ***)queried)[2])(queried);
    InterlockedIncrement(&proxy->references);
    *object = proxy;
    return S_OK;
}

static ULONG STDMETHODCALLTYPE compat_d3d12_proxy_add_ref(CompatD3D12DeviceProxy *proxy)
{
    return (ULONG)InterlockedIncrement(&proxy->references);
}

static ULONG STDMETHODCALLTYPE compat_d3d12_proxy_release(CompatD3D12DeviceProxy *proxy)
{
    LONG references = InterlockedDecrement(&proxy->references);
    if (!references) {
        typedef ULONG (STDMETHODCALLTYPE *release_fn)(void *);
        void *native_device = proxy->native_device;
        ((release_fn)(*(void ***)native_device)[2])(native_device);
        HeapFree(GetProcessHeap(), 0, proxy);
    }
    return references < 0 ? 0 : (ULONG)references;
}

static HRESULT compat_d3d12_proxy_create_command_queue_for_slot(
    CompatD3D12DeviceProxy *proxy, const CompatD3D12CommandQueueDesc *description,
    const GUID *iid, void **queue, unsigned xbox_slot)
{
    typedef HRESULT (STDMETHODCALLTYPE *create_command_queue_fn)(
        void *, const CompatD3D12CommandQueueDesc *, const GUID *, void **);
    char message[512];
    CompatD3D12CommandQueueDesc translated;
    void **native_vtable;
    HRESULT result;

    if (!proxy || !description || !iid || !queue) return E_POINTER;
    *queue = NULL;
    translated = *description;
    /* The observed Xbox priority is the ordinal high-priority value (2); the
     * Windows D3D12 ABI uses the explicit priority constant 100. */
    if (translated.priority == 2) translated.priority = 100;
    /* Xbox queue flag 0x2 requests dynamic queue priority, but the host
     * Windows D3D12 runtime rejects it with E_INVALIDARG. For the Xbox-only
     * alias at slot +0x180, degrade to a normal queue so startup can proceed;
     * this intentionally does not emulate dynamic-priority scheduling. */
    if (xbox_slot == 48 && (translated.flags & 0x2u))
        translated.flags &= ~0x2u;
    native_vtable = *(void ***)proxy->native_device;
    result = ((create_command_queue_fn)native_vtable[8])(
        proxy->native_device, &translated, iid, queue);
    snprintf(message, sizeof(message),
        "[graphics] CreateCommandQueue XboxSlot=0x%lx Xbox={type=%ld priority=%ld flags=0x%lx node=0x%lx} Windows={type=%ld priority=%ld flags=0x%lx node=0x%lx} result=0x%08lx queue=%p\n",
        (unsigned long)(xbox_slot * sizeof(void *)),
        (long)description->type, (long)description->priority,
        (unsigned long)description->flags, (unsigned long)description->node_mask,
        (long)translated.type, (long)translated.priority,
        (unsigned long)translated.flags, (unsigned long)translated.node_mask,
        (unsigned long)result, *queue);
    OutputDebugStringA(message);
    return result;
}

static HRESULT STDMETHODCALLTYPE compat_d3d12_proxy_create_command_queue_slot8(
    CompatD3D12DeviceProxy *proxy, const CompatD3D12CommandQueueDesc *description,
    const GUID *iid, void **queue)
{
    return compat_d3d12_proxy_create_command_queue_for_slot(
        proxy, description, iid, queue, 8);
}

static HRESULT STDMETHODCALLTYPE compat_d3d12_proxy_create_command_queue_slot48(
    CompatD3D12DeviceProxy *proxy, const CompatD3D12CommandQueueDesc *description,
    const GUID *iid, void **queue)
{
    return compat_d3d12_proxy_create_command_queue_for_slot(
        proxy, description, iid, queue, 48);
}

typedef struct CompatXboxCommandListDesc {
    int32_t type;
    uint32_t unknown_04;
    uint32_t node_mask;
    uint32_t unknown_0c;
} CompatXboxCommandListDesc;

#define COMPAT_D3D12_COMMAND_LIST_PROXY_SLOTS 133

typedef struct CompatD3D12CommandListProxy {
    void **vtable;
    void *native_command_list;
    unsigned char *forwarding_thunks;
    volatile LONG references;
    GUID xbox_iid;
} CompatD3D12CommandListProxy;

static BOOL compat_d3d12_noop_extensions_requested(void)
{
    char value[16] = {0};
    DWORD length = GetEnvironmentVariableA(
        "COMPAT_D3D12_XBOX_COMMAND_LIST_NOOP_EXTENSIONS", value, sizeof(value));
    return length > 0 && length < sizeof(value) && value[0] != '0';
}

static BOOL compat_d3d12_noop_device_slot278_requested(void)
{
    char value[16] = {0};
    DWORD length = GetEnvironmentVariableA(
        "COMPAT_D3D12_XBOX_DEVICE_SLOT278_NOOP", value, sizeof(value));
    return length > 0 && length < sizeof(value) && value[0] != '0';
}

static BOOL compat_d3d12_noop_device_slot70_requested(void)
{
    char value[16] = {0};
    DWORD length = GetEnvironmentVariableA(
        "COMPAT_D3D12_XBOX_DEVICE_SLOT70_NOOP", value, sizeof(value));
    return length > 0 && length < sizeof(value) && value[0] != '0';
}

static BOOL compat_d3d12_translate_observed_allocation_flags_requested(void)
{
    char value[2];
    return GetEnvironmentVariableA("COMPAT_D3D12_TRANSLATE_OBSERVED_ALLOCATION_FLAGS",
        value, sizeof(value)) == 1 && value[0] == '1';
}

static BOOL compat_d3d12_observed_committed_resource3_requested(void)
{
    char value[2];
    return GetEnvironmentVariableA("COMPAT_D3D12_XBOX_SLOT3F8_COMMITTED_RESOURCE3",
        value, sizeof(value)) == 1 && value[0] == '1';
}

static void compat_d3d12_observe_texture_arena_slice(void *native,
    const D3D12_RESOURCE_DESC1 *desc, void *resource)
{
    static LONG startup_slice_count;
    char enabled[2];
    if (GetEnvironmentVariableA("COMPAT_D3D12_XBOX_TEXTURE_ARENA_HOOK", enabled,
        sizeof(enabled)) != 1 || enabled[0] != '1') return;
    /* The committed Windows textures do not drive XMem free on resize.
     * Leave later allocations alone until that lifetime is implemented. */
    if (InterlockedIncrement(&startup_slice_count) > 2) return;

    typedef BOOL (WINAPI *slice_fn)(SIZE_T, SIZE_T, void **, SIZE_T *);
    HMODULE xmem = GetModuleHandleW(L"xmem.dll");
    slice_fn reserve = xmem ? (slice_fn)GetProcAddress(xmem, "CompatXMemTextureArenaSlice") : NULL;
    D3D12_RESOURCE_DESC legacy = {
        desc->Dimension, desc->Alignment, desc->Width, desc->Height, desc->DepthOrArraySize,
        desc->MipLevels, desc->Format, desc->SampleDesc, desc->Layout, desc->Flags
    };
    D3D12_RESOURCE_ALLOCATION_INFO allocation = {0};
    ID3D12Device_GetResourceAllocationInfo((ID3D12Device *)native, &allocation, 1, 1, &legacy);
    void *slice = NULL;
    SIZE_T offset = 0;
    BOOL accepted = reserve && reserve((SIZE_T)allocation.SizeInBytes,
        (SIZE_T)allocation.Alignment, &slice, &offset);
    char message[250];
    snprintf(message, sizeof(message),
        "[graphics] texture arena slice accepted=%u texture=%p slice=%p offset=0x%llx bytes=0x%llx alignment=0x%llx\n",
        (unsigned)accepted, resource, slice, (unsigned long long)offset,
        (unsigned long long)allocation.SizeInBytes, (unsigned long long)allocation.Alignment);
    OutputDebugStringA(message);
}

/* The Xbox caller passes an explicit return buffer first; Windows COM
 * expects it after this. Only retry the observed startup allocation query. */
static D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE
compat_d3d12_device_get_allocation_info_slot25(
    D3D12_RESOURCE_ALLOCATION_INFO *info, CompatD3D12DeviceProxy *proxy,
    UINT visible_mask, UINT count, const D3D12_RESOURCE_DESC *desc)
{
    typedef D3D12_RESOURCE_ALLOCATION_INFO *(STDMETHODCALLTYPE *native_fn)(
        void *, D3D12_RESOURCE_ALLOCATION_INFO *, UINT, UINT, const D3D12_RESOURCE_DESC *);
    void *native = proxy->native_device;
    native_fn get_info = (native_fn)(*(void ***)native)[COMPAT_D3D12_SLOT_GET_RESOURCE_ALLOCATION_INFO];
    D3D12_RESOURCE_ALLOCATION_INFO *native_result =
        get_info(native, info, visible_mask, count, desc);
    D3D12_RESOURCE_ALLOCATION_INFO original = *info;
    D3D12_RESOURCE_ALLOCATION_INFO *result = native_result;
    if (native_result != info || original.SizeInBytes != UINT64_MAX ||
        !compat_d3d12_translate_observed_allocation_flags_requested() ||
        visible_mask != 1 || count != 1 || !desc ||
        desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc->Alignment != 0 ||
        desc->Width != 3840 || desc->Height != 2160 || desc->DepthOrArraySize != 1 ||
        desc->MipLevels != 1 || desc->Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
        desc->SampleDesc.Count != 1 || desc->SampleDesc.Quality != 0 ||
        desc->Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN || (UINT)desc->Flags != 0x00300005) {
        if (desc && (native_result != info || original.SizeInBytes == UINT64_MAX) &&
            compat_d3d12_translate_observed_allocation_flags_requested()) {
            char message[240];
            snprintf(message, sizeof(message),
                "[graphics] allocation-info slot +0xc8 passthrough result=%p desc=%p mask=%u count=%u "
                "size=0x%llx alignment=0x%llx flags=0x%08x\n",
                native_result, desc, visible_mask, count,
                (unsigned long long)original.SizeInBytes,
                (unsigned long long)original.Alignment, (UINT)desc->Flags);
            OutputDebugStringA(message);
        }
        return result;
    }

    D3D12_RESOURCE_DESC translated = *desc;
    D3D12_RESOURCE_ALLOCATION_INFO retry = {0};
    char message[240];
    translated.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_RESOURCE_ALLOCATION_INFO *retry_result =
        get_info(native, &retry, visible_mask, count, &translated);
    if (retry_result == &retry && retry.SizeInBytes != UINT64_MAX && retry.SizeInBytes != 0) {
        *info = retry;
        result = info;
    }
    snprintf(message, sizeof(message),
        "[graphics] allocation-info slot +0xc8 desc=%p native_result=%p retry_result=%p "
        "flags=0x%08x->0x%08x original=0x%llx/%llx final=0x%llx/%llx return=%p\n",
        desc, native_result, retry_result, (UINT)desc->Flags, (UINT)translated.Flags,
        (unsigned long long)original.SizeInBytes, (unsigned long long)original.Alignment,
        (unsigned long long)info->SizeInBytes, (unsigned long long)info->Alignment, result);
    OutputDebugStringA(message);
    return result;
}

/* Xbox device slot +0x3f8 is the CreateCommittedResource3-shaped call seen
 * at 0x193e1b4. The matching Windows Device11 method is slot 76, while
 * forwarding Xbox index 127 reads beyond the Windows vtable. */
static HRESULT STDMETHODCALLTYPE compat_d3d12_device_create_committed_resource3_slot3f8(
    CompatD3D12DeviceProxy *proxy, const D3D12_HEAP_PROPERTIES *heap_properties,
    D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC1 *desc,
    D3D12_BARRIER_LAYOUT initial_layout, const D3D12_CLEAR_VALUE *clear_value,
    ID3D12ProtectedResourceSession *protected_session, UINT castable_format_count,
    const DXGI_FORMAT *castable_formats, const GUID *resource_iid,
    void **resource_out)
{
    typedef HRESULT (STDMETHODCALLTYPE *native_fn)(void *, const D3D12_HEAP_PROPERTIES *,
        D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC1 *, D3D12_BARRIER_LAYOUT,
        const D3D12_CLEAR_VALUE *, ID3D12ProtectedResourceSession *, UINT,
        const DXGI_FORMAT *, const GUID *, void **);
    static const GUID iid_d3d12_resource = {
        0x696442be, 0xa72e, 0x4059, {0xbc,0x79,0x5b,0x5c,0x98,0x04,0x0f,0xad}
    };
    void *native = proxy->native_device;
    native_fn create = (native_fn)(*(void ***)native)[
        COMPAT_D3D12_DEVICE11_SLOT_CREATE_COMMITTED_RESOURCE3];
    void *original_output = resource_out ? *resource_out : NULL;
    HRESULT original_result = create(native, heap_properties, heap_flags, desc,
        initial_layout, clear_value, protected_session, castable_format_count,
        castable_formats, resource_iid, resource_out);

    BOOL observed_shape = heap_properties && desc && clear_value && resource_iid && resource_out &&
        heap_properties->Type == D3D12_HEAP_TYPE_DEFAULT &&
        heap_properties->CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_UNKNOWN &&
        heap_properties->MemoryPoolPreference == D3D12_MEMORY_POOL_UNKNOWN &&
        heap_properties->CreationNodeMask == 1 && heap_properties->VisibleNodeMask == 1 &&
        (UINT)heap_flags == 0x8 &&
        desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc->Alignment == 0 &&
        desc->Width == 3840 && desc->Height == 2160 && desc->DepthOrArraySize == 1 &&
        desc->MipLevels == 1 && desc->Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
        desc->SampleDesc.Count == 1 && desc->SampleDesc.Quality == 0 &&
        desc->Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN && (UINT)desc->Flags == 0x00300005 &&
        desc->SamplerFeedbackMipRegion.Width == 0 &&
        desc->SamplerFeedbackMipRegion.Height == 0 &&
        desc->SamplerFeedbackMipRegion.Depth == 0 &&
        initial_layout == D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS &&
        clear_value->Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
        clear_value->Color[0] == 0.0f && clear_value->Color[1] == 0.0f &&
        clear_value->Color[2] == 0.0f && clear_value->Color[3] == 0.0f &&
        !protected_session && castable_format_count == 0 && !castable_formats &&
        IsEqualGUID(resource_iid, &iid_d3d12_resource);

    if (SUCCEEDED(original_result) || !observed_shape ||
        !compat_d3d12_observed_committed_resource3_requested())
        return original_result;

    D3D12_RESOURCE_DESC1 translated = *desc;
    translated.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    void *retry_output = NULL;
    HRESULT retry_result = create(native, heap_properties, heap_flags, &translated,
        initial_layout, clear_value, protected_session, castable_format_count,
        castable_formats, resource_iid, &retry_output);
    char message[320];
    if (SUCCEEDED(retry_result) && retry_output) {
        compat_d3d12_observe_texture_arena_slice(native, &translated, retry_output);
        *resource_out = retry_output;
    } else {
        if (retry_output) {
            typedef ULONG (STDMETHODCALLTYPE *release_fn)(void *);
            ((release_fn)(*(void ***)retry_output)[2])(retry_output);
        }
        *resource_out = original_output;
    }
    snprintf(message, sizeof(message),
        "[graphics] Xbox device slot +0x3f8 mapped to native slot 76 desc=%p "
        "flags=0x%08x->0x%08x original_hr=0x%08lx retry_hr=0x%08lx "
        "original_out=%p retry_out=%p final_out=%p\n",
        desc, (UINT)desc->Flags, (UINT)translated.Flags,
        (unsigned long)original_result, (unsigned long)retry_result,
        original_output, retry_output, *resource_out);
    OutputDebugStringA(message);
    return SUCCEEDED(retry_result) ? retry_result : original_result;
}

static BOOL compat_d3d12_xbox_slot428_buffer_requested(void)
{
    char value[16] = {0};
    DWORD length = GetEnvironmentVariableA(
        "COMPAT_D3D12_XBOX_SLOT428_BUFFER", value, sizeof(value));
    return length > 0 && length < sizeof(value) && value[0] != '0';
}

static HRESULT STDMETHODCALLTYPE compat_d3d12_device_create_xmem_buffer_slot428(
    CompatD3D12DeviceProxy *proxy, void *resource_address,
    const void *xbox_description, uint32_t xbox_initial_state,
    const void *clear_value, const GUID *resource_iid, void **resource_out)
{
    static const GUID iid_d3d12_device3 = {
        0x81dadc15, 0x2bad, 0x4392, {0x93,0xc5,0x10,0x13,0x45,0xc4,0xaa,0x98}
    };
    static const GUID iid_d3d12_heap = {
        0x6b3b2502, 0x6e51, 0x45b3, {0x90,0xee,0x98,0x84,0x26,0x5e,0x8d,0xf3}
    };
    static const GUID iid_d3d12_resource = {
        0x696442be, 0xa72e, 0x4059, {0xbc,0x79,0x5b,0x5c,0x98,0x04,0x0f,0xad}
    };
    const SIZE_T expected_reservation_size = 0x0b900000u;
    const SIZE_T expected_resource_offset = 0x00b00000u;
    const SIZE_T expected_resource_size = 0x0ae00000u;
    unsigned char xbox_desc_bytes[64];
    D3D12_RESOURCE_DESC desc = {0};
    MEMORY_BASIC_INFORMATION mbi;
    ID3D12Device3 *device3 = NULL;
    ID3D12Heap *heap = NULL;
    ID3D12Resource *resource = NULL;
    void *reservation_base;
    uintptr_t target = 0, base = 0, end = 0;
    uintptr_t failed_cursor = 0;
    DWORD failed_state = 0, failed_type = 0;
    SIZE_T failed_region_size = 0;
    void *failed_allocation_base = NULL;
    uint32_t xbox_resource_flags = 0;
    SIZE_T bytes_read = 0, bytes_written = 0;
    HRESULT result = E_NOTIMPL;
    char message[512];
    const char *stage = "arguments";
    void *null_resource = NULL;

    if (!proxy || !resource_address || !xbox_description || !resource_iid ||
        !resource_out) return E_POINTER;
    if (!WriteProcessMemory(GetCurrentProcess(), resource_out, &null_resource,
            sizeof(void *), &bytes_written) || bytes_written != sizeof(void *))
        return E_POINTER;
    if (!compat_d3d12_xbox_slot428_buffer_requested()) return E_NOTIMPL;
    if (clear_value || !IsEqualGUID(resource_iid, &iid_d3d12_resource) ||
        xbox_initial_state != 0xffffffffu) {
        result = E_NOTIMPL;
        goto done;
    }
    if (!ReadProcessMemory(GetCurrentProcess(), xbox_description,
            xbox_desc_bytes, sizeof(xbox_desc_bytes), &bytes_read) ||
        bytes_read != sizeof(xbox_desc_bytes)) {
        result = E_POINTER;
        goto done;
    }
    memcpy(&desc, xbox_desc_bytes, sizeof(desc));
    xbox_resource_flags = desc.Flags;
    stage = "descriptor";
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        desc.Alignment != 0 || desc.Width != expected_resource_size ||
        desc.Height != 1 || desc.DepthOrArraySize != 1 || desc.MipLevels != 1 ||
        desc.Format != DXGI_FORMAT_UNKNOWN || desc.SampleDesc.Count != 1 ||
        desc.SampleDesc.Quality != 0 ||
        desc.Layout != D3D12_TEXTURE_LAYOUT_ROW_MAJOR || desc.Flags != 0x00020004u ||
        memcmp(xbox_desc_bytes + 56, "\0\0\0\0\0\0\0\0", 8) != 0) {
        result = E_NOTIMPL;
        goto done;
    }
    target = (uintptr_t)resource_address;
    stage = "query-target";
    if (!VirtualQuery(resource_address, &mbi, sizeof(mbi)) ||
        mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || !mbi.AllocationBase) {
        result = HRESULT_FROM_WIN32(ERROR_INVALID_ADDRESS);
        goto done;
    }
    reservation_base = mbi.AllocationBase;
    base = (uintptr_t)reservation_base;
    stage = "range-offset";
    if (target < base || target - base != expected_resource_offset ||
        target > UINTPTR_MAX - expected_resource_size) {
        result = HRESULT_FROM_WIN32(ERROR_INVALID_ADDRESS);
        goto done;
    }
    end = target + expected_resource_size;
    stage = "range-size";
    if (end - base != expected_reservation_size) {
        result = HRESULT_FROM_WIN32(ERROR_INVALID_ADDRESS);
        goto done;
    }
    for (uintptr_t cursor = base; cursor < end;) {
        MEMORY_BASIC_INFORMATION region;
        if (!VirtualQuery((const void *)cursor, &region, sizeof(region))) {
            stage = "range-query";
            failed_cursor = cursor;
            result = HRESULT_FROM_WIN32(GetLastError());
            goto done;
        }
        if (region.State != MEM_COMMIT || region.Type != MEM_PRIVATE ||
            region.AllocationBase != reservation_base || !region.RegionSize ||
            cursor > UINTPTR_MAX - region.RegionSize) {
            stage = "range-state";
            failed_cursor = cursor;
            failed_state = region.State;
            failed_type = region.Type;
            failed_region_size = region.RegionSize;
            failed_allocation_base = region.AllocationBase;
            result = HRESULT_FROM_WIN32(ERROR_INVALID_ADDRESS);
            goto done;
        }
        uintptr_t next = cursor + region.RegionSize;
        if (next <= cursor) {
            result = HRESULT_FROM_WIN32(ERROR_INVALID_ADDRESS);
            goto done;
        }
        cursor = next < end ? next : end;
    }

    /* The observed Xbox descriptor carries an extra 0x20000 flag. Windows
     * OpenExistingHeapFromAddress exposes the shared cross-adapter heap shape,
     * which requires ALLOW_CROSS_ADAPTER on the placed resource. */
    xbox_resource_flags = desc.Flags;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
        D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
    stage = "query-device3";
    result = ID3D12Device_QueryInterface(
        (ID3D12Device *)proxy->native_device, &iid_d3d12_device3,
        (void **)&device3);
    if (FAILED(result) || !device3) goto done;
    stage = "open-heap";
    result = ID3D12Device3_OpenExistingHeapFromAddress(device3,
        reservation_base, &iid_d3d12_heap, (void **)&heap);
    if (FAILED(result) || !heap) goto done;

    /* The only observed state is the Xbox default-state sentinel. Map it to
     * D3D12 COMMON for this narrowly recognized startup buffer. */
    stage = "create-placed-resource";
    result = ID3D12Device3_CreatePlacedResource(device3, heap,
        expected_resource_offset, &desc, D3D12_RESOURCE_STATE_COMMON, NULL,
        &iid_d3d12_resource, (void **)&resource);
    if (SUCCEEDED(result) && resource) {
        if (!WriteProcessMemory(GetCurrentProcess(), resource_out, &resource,
                sizeof(resource), &bytes_written) || bytes_written != sizeof(resource)) {
            ID3D12Resource_Release(resource);
            resource = NULL;
            result = E_POINTER;
        }
    }

done:
    if (resource && FAILED(result)) ID3D12Resource_Release(resource);
    if (heap) ID3D12Heap_Release(heap);
    if (device3) ID3D12Device3_Release(device3);
    snprintf(message, sizeof(message),
        "[graphics] XboxSlot=0x428 XMemBuffer stage=%s address=%p description=%p state=0x%08lx base=%p offset=0x%llx failAt=%p failState=0x%lx failType=0x%lx failSize=0x%llx failAllocation=%p xboxFlags=0x%08lx windowsFlags=0x%08lx result=0x%08lx resource=%p gpuVA=0x%llx\n",
        stage,
        resource_address, xbox_description, (unsigned long)xbox_initial_state,
        (void *)base, (unsigned long long)(target >= base ? target - base : 0),
        (void *)failed_cursor, (unsigned long)failed_state, (unsigned long)failed_type,
        (unsigned long long)failed_region_size, failed_allocation_base,
        (unsigned long)xbox_resource_flags,
        (unsigned long)desc.Flags, (unsigned long)result, resource,
        (unsigned long long)(resource ? ID3D12Resource_GetGPUVirtualAddress(resource) : 0));
    OutputDebugStringA(message);
    return result;
}

static HRESULT STDMETHODCALLTYPE compat_d3d12_device_noop_slot278(
    CompatD3D12DeviceProxy *proxy, uintptr_t argument1,
    uintptr_t argument2, uintptr_t argument3)
{
    char message[192];
    (void)proxy;
    snprintf(message, sizeof(message),
        "[graphics] experimental Xbox device slot +0x278 no-op args=%p,%p,%p\n",
        (void *)argument1, (void *)argument2, (void *)argument3);
    OutputDebugStringA(message);
    return S_OK;
}

/* Xbox slot +0x230 does not match the same-index Windows method. This
 * opt-in stub avoids forwarding an Xbox record into the native AddToStateObject
 * ABI while probing startup. It does not emulate the unknown Xbox operation. */
static HRESULT STDMETHODCALLTYPE compat_d3d12_device_noop_slot70(
    CompatD3D12DeviceProxy *proxy, uintptr_t argument1,
    uintptr_t argument2, uintptr_t argument3)
{
    char message[192];
    (void)proxy;
    snprintf(message, sizeof(message),
        "[graphics] experimental Xbox device slot +0x230 no-op args=%p,%p,%p\n",
        (void *)argument1, (void *)argument2, (void *)argument3);
    OutputDebugStringA(message);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE compat_d3d12_command_list_query_interface(
    CompatD3D12CommandListProxy *proxy, const GUID *iid, void **object)
{
    static const GUID iid_iunknown = {
        0x00000000, 0x0000, 0x0000, {0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}
    };
    static const GUID iid_d3d12_graphics_command_list = {
        0x5b160d0f, 0xac1b, 0x4185, {0x8b,0xa8,0xb3,0xae,0x42,0xa5,0xa4,0x55}
    };
    if (!proxy || !iid || !object) return E_POINTER;
    *object = NULL;
    if (!IsEqualGUID(iid, &iid_iunknown) &&
        !IsEqualGUID(iid, &iid_d3d12_graphics_command_list) &&
        !IsEqualGUID(iid, &proxy->xbox_iid)) return E_NOINTERFACE;
    InterlockedIncrement(&proxy->references);
    *object = proxy;
    return S_OK;
}

static ULONG STDMETHODCALLTYPE compat_d3d12_command_list_add_ref(
    CompatD3D12CommandListProxy *proxy)
{
    return proxy ? (ULONG)InterlockedIncrement(&proxy->references) : 0;
}

static ULONG STDMETHODCALLTYPE compat_d3d12_command_list_release(
    CompatD3D12CommandListProxy *proxy)
{
    LONG references;
    typedef ULONG (STDMETHODCALLTYPE *release_fn)(void *);
    if (!proxy) return 0;
    references = InterlockedDecrement(&proxy->references);
    if (!references) {
        void *native = proxy->native_command_list;
        ((release_fn)(*(void ***)native)[2])(native);
        HeapFree(GetProcessHeap(), 0, proxy->vtable);
        if (proxy->forwarding_thunks)
            VirtualFree(proxy->forwarding_thunks, 0, MEM_RELEASE);
        HeapFree(GetProcessHeap(), 0, proxy);
    }
    return references < 0 ? 0 : (ULONG)references;
}

static HRESULT STDMETHODCALLTYPE compat_d3d12_command_list_noop_extension(
    CompatD3D12CommandListProxy *proxy, uintptr_t argument1, uintptr_t argument2,
    uintptr_t argument3, uintptr_t argument4)
{
    (void)proxy;
    (void)argument1;
    (void)argument2;
    (void)argument3;
    (void)argument4;
    return S_OK;
}

static CompatD3D12CommandListProxy *compat_d3d12_command_list_proxy_create(
    void *native_command_list, const GUID *xbox_iid)
{
    SYSTEM_INFO system_info;
    SIZE_T code_size;
    DWORD old_protection;
    void **native_vtable, **vtable;
    unsigned char *code;
    CompatD3D12CommandListProxy *proxy;

    if (!native_command_list || !xbox_iid) return NULL;
    native_vtable = *(void ***)native_command_list;
    GetSystemInfo(&system_info);
    code_size = (COMPAT_D3D12_COMMAND_LIST_PROXY_SLOTS * COMPAT_D3D12_THUNK_STRIDE +
        system_info.dwPageSize - 1) & ~((SIZE_T)system_info.dwPageSize - 1);
    code = (unsigned char *)VirtualAlloc(NULL, code_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    vtable = (void **)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
        COMPAT_D3D12_COMMAND_LIST_PROXY_SLOTS * sizeof(*vtable));
    proxy = (CompatD3D12CommandListProxy *)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*proxy));
    if (!code || !vtable || !proxy) {
        if (code) VirtualFree(code, 0, MEM_RELEASE);
        if (vtable) HeapFree(GetProcessHeap(), 0, vtable);
        if (proxy) HeapFree(GetProcessHeap(), 0, proxy);
        return NULL;
    }

    memcpy(vtable, native_vtable, COMPAT_D3D12_COMMAND_LIST_PROXY_SLOTS * sizeof(*vtable));
    for (unsigned slot = 3; slot < COMPAT_D3D12_COMMAND_LIST_PROXY_SLOTS; ++slot) {
        unsigned char *cursor = code + slot * COMPAT_D3D12_THUNK_STRIDE;
        if (slot == 0x418 / sizeof(void *)) {
            vtable[slot] = (void *)compat_d3d12_command_list_noop_extension;
            continue;
        }
        if (slot == 0x420 / sizeof(void *)) {
            vtable[slot] = (void *)compat_d3d12_command_list_noop_extension;
            continue;
        }
        /* Replace the wrapper this pointer, load the native vtable, then
         * dispatch the same index to the underlying Windows command list. */
        *cursor++ = 0x48; *cursor++ = 0x8b; *cursor++ = 0x49; *cursor++ = 0x08;
        *cursor++ = 0x48; *cursor++ = 0x8b; *cursor++ = 0x01;
        *cursor++ = 0xff; *cursor++ = 0xa0;
        compat_write_u32(&cursor, slot * (uint32_t)sizeof(void *));
        vtable[slot] = code + slot * COMPAT_D3D12_THUNK_STRIDE;
    }
    vtable[0] = (void *)compat_d3d12_command_list_query_interface;
    vtable[1] = (void *)compat_d3d12_command_list_add_ref;
    vtable[2] = (void *)compat_d3d12_command_list_release;
    if (!VirtualProtect(code, code_size, PAGE_EXECUTE_READ, &old_protection)) {
        VirtualFree(code, 0, MEM_RELEASE);
        HeapFree(GetProcessHeap(), 0, vtable);
        HeapFree(GetProcessHeap(), 0, proxy);
        return NULL;
    }
    FlushInstructionCache(GetCurrentProcess(), code, code_size);
    proxy->vtable = vtable;
    proxy->native_command_list = native_command_list;
    proxy->references = 1;
    proxy->xbox_iid = *xbox_iid;
    proxy->forwarding_thunks = code;
    return proxy;
}

static HRESULT STDMETHODCALLTYPE compat_d3d12_proxy_create_command_list_xbox_slot73(
    CompatD3D12DeviceProxy *proxy, const CompatXboxCommandListDesc *description,
    void *allocator, void *initial_state, const GUID *xbox_iid, void **command_list)
{
    static const GUID iid_d3d12_graphics_command_list = {
        0x5b160d0f, 0xac1b, 0x4185, {0x8b,0xa8,0xb3,0xae,0x42,0xa5,0xa4,0x55}
    };
    typedef HRESULT (STDMETHODCALLTYPE *create_command_list_fn)(
        void *, UINT, int, void *, void *, const GUID *, void **);
    char message[512];
    void **native_vtable;
    HRESULT result;

    if (command_list) *command_list = NULL;
    if (!proxy || !description || !allocator || !xbox_iid || !command_list)
        return E_POINTER;

    /* The observed Xbox slot +0x248 call has the CreateCommandList shape:
     * packed descriptor, allocator, initial state, IID, and output. Static
     * reverse-engineering evidence identifies descriptor +0 as type and +8
     * as node mask; preserve both when translating. The meanings of +4 and
     * +0xc remain unknown. The Xbox IID differs from Windows, so request a
     * Windows graphics command-list interface here. This is still not an
     * Xbox command-list wrapper: Xbox-specific IID queries and extensions
     * are unsupported and must be handled at their actual call sites. */
    native_vtable = *(void ***)proxy->native_device;
    result = ((create_command_list_fn)native_vtable[12])(
        proxy->native_device, description->node_mask,
        description->type, allocator, initial_state,
        &iid_d3d12_graphics_command_list, command_list);
    if (SUCCEEDED(result) && *command_list &&
        compat_d3d12_noop_extensions_requested()) {
        CompatD3D12CommandListProxy *list_proxy =
            compat_d3d12_command_list_proxy_create(*command_list, xbox_iid);
        if (!list_proxy) {
            typedef ULONG (STDMETHODCALLTYPE *release_fn)(void *);
            ((release_fn)(*(void ***)*command_list)[2])(*command_list);
            *command_list = NULL;
            result = E_OUTOFMEMORY;
        } else {
            *command_list = list_proxy;
            OutputDebugStringA("[graphics] Xbox command-list proxy enabled; slots +0x418/+0x420 are experimental no-ops\n");
        }
    }
    snprintf(message, sizeof(message),
        "[graphics] CreateCommandList XboxSlot=0x248 XboxDesc={type=%ld unknown04=%08lx nodeMask=%08lx unknown0c=%08lx} allocator=%p initial=%p XboxIID=%08lx-%04x-%04x result=0x%08lx list=%p\n",
        (long)description->type, (unsigned long)description->unknown_04,
        (unsigned long)description->node_mask, (unsigned long)description->unknown_0c,
        allocator, initial_state, (unsigned long)xbox_iid->Data1,
        (unsigned)xbox_iid->Data2, (unsigned)xbox_iid->Data3,
        (unsigned long)result, *command_list);
    OutputDebugStringA(message);
    return result;
}

static BOOL compat_d3d12_device_vtable_init(void)
{
    SYSTEM_INFO system_info;
    SIZE_T code_size;
    DWORD old_protection;
    unsigned char *code;
    void **vtable;

    AcquireSRWLockExclusive(&g_d3d12_device_proxy_lock);
    if (g_d3d12_device11_vtable) {
        ReleaseSRWLockExclusive(&g_d3d12_device_proxy_lock);
        return TRUE;
    }

    GetSystemInfo(&system_info);
    code_size = (COMPAT_D3D12_DEVICE11_VTABLE_SLOTS * COMPAT_D3D12_THUNK_STRIDE +
        system_info.dwPageSize - 1) & ~((SIZE_T)system_info.dwPageSize - 1);
    code = (unsigned char *)VirtualAlloc(NULL, code_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    vtable = (void **)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
        COMPAT_D3D12_DEVICE11_VTABLE_SLOTS * sizeof(*vtable));
    if (!code || !vtable) {
        if (code) VirtualFree(code, 0, MEM_RELEASE);
        if (vtable) HeapFree(GetProcessHeap(), 0, vtable);
        ReleaseSRWLockExclusive(&g_d3d12_device_proxy_lock);
        return FALSE;
    }

    for (unsigned slot = 3; slot < COMPAT_D3D12_DEVICE11_VTABLE_SLOTS; ++slot) {
        unsigned char *cursor = code + slot * COMPAT_D3D12_THUNK_STRIDE;
        if (slot == 0x230 / sizeof(void *) &&
            compat_d3d12_noop_device_slot70_requested()) {
            vtable[slot] = (void *)compat_d3d12_device_noop_slot70;
            continue;
        }
        if (slot == COMPAT_D3D12_XBOX_SLOT_CREATE_COMMITTED_RESOURCE3 &&
            compat_d3d12_observed_committed_resource3_requested()) {
            vtable[slot] = (void *)compat_d3d12_device_create_committed_resource3_slot3f8;
            continue;
        }
        if (slot == 0x1f0 / sizeof(void *) && compat_d3d12_bvh_translation_requested()) {
            vtable[slot] = (void *)compat_d3d12_bvh_compute_pipeline_slot1f0;
            continue;
        }
        if (slot == 0x428 / sizeof(void *)) {
            /* This opt-in adapter recognizes only the exact startup XMem
             * buffer descriptor; other Xbox resource shapes fail closed. */
            vtable[slot] = compat_d3d12_xbox_slot428_buffer_requested()
                ? (void *)compat_d3d12_device_create_xmem_buffer_slot428 : NULL;
            continue;
        }
        if (slot == 0x278 / sizeof(void *) &&
            compat_d3d12_noop_device_slot278_requested()) {
            vtable[slot] = (void *)compat_d3d12_device_noop_slot278;
            continue;
        }
        BOOL structure_return = slot == COMPAT_D3D12_SLOT_GET_RESOURCE_ALLOCATION_INFO ||
            slot == COMPAT_D3D12_SLOT_GET_CUSTOM_HEAP_PROPERTIES ||
            slot == COMPAT_D3D12_SLOT_GET_RESOURCE_ALLOCATION_INFO1 ||
            slot == COMPAT_D3D12_SLOT_GET_RESOURCE_ALLOCATION_INFO2;
        if (structure_return) {
            /* Xbox: RCX=return buffer, RDX=proxy. Windows: RCX=this, RDX=return buffer. */
            *cursor++ = 0x49; *cursor++ = 0x89; *cursor++ = 0xca;       /* mov r10, rcx */
            *cursor++ = 0x48; *cursor++ = 0x8b; *cursor++ = 0x4a; *cursor++ = 0x08; /* mov rcx,[rdx+8] */
            *cursor++ = 0x4c; *cursor++ = 0x89; *cursor++ = 0xd2;       /* mov rdx, r10 */
        } else {
            /* Replace the proxy this pointer while preserving all arguments. */
            *cursor++ = 0x48; *cursor++ = 0x8b; *cursor++ = 0x49; *cursor++ = 0x08; /* mov rcx,[rcx+8] */
        }
        *cursor++ = 0x48; *cursor++ = 0x8b; *cursor++ = 0x01;           /* mov rax,[rcx] */
        *cursor++ = 0xff; *cursor++ = 0xa0;                             /* jmp qword ptr [rax+disp32] */
        compat_write_u32(&cursor, slot * (uint32_t)sizeof(void *));
        vtable[slot] = code + slot * COMPAT_D3D12_THUNK_STRIDE;
    }
    vtable[0] = (void *)compat_d3d12_proxy_query_interface;
    vtable[1] = (void *)compat_d3d12_proxy_add_ref;
    vtable[2] = (void *)compat_d3d12_proxy_release;
    vtable[8] = (void *)compat_d3d12_proxy_create_command_queue_slot8;
    vtable[14] = (void *)compat_d3d12_proxy_create_descriptor_heap;
    if (compat_d3d12_translate_observed_allocation_flags_requested())
        vtable[COMPAT_D3D12_SLOT_GET_RESOURCE_ALLOCATION_INFO] =
            (void *)compat_d3d12_device_get_allocation_info_slot25;
    /* The observed Xbox call at RVA 0x2478813 uses the same queue descriptor,
     * IID, and output-pointer ABI as CreateCommandQueue, despite landing at
     * Xbox slot +0x180. Translate it to the Windows CreateCommandQueue slot. */
    vtable[48] = (void *)compat_d3d12_proxy_create_command_queue_slot48;
    /* The observed Xbox slot +0x248 call passes a command-list descriptor,
     * allocator, initial state, IID, and output pointer. It does not match
     * Windows ID3D12Device11 slot 73 (CreateShaderCacheSession). */
    vtable[73] = (void *)compat_d3d12_proxy_create_command_list_xbox_slot73;
    if (compat_d3d12_noop_device_slot278_requested())
        OutputDebugStringA("[graphics] Xbox device slot +0x278 is an experimental no-op\n");
    if (compat_d3d12_noop_device_slot70_requested())
        OutputDebugStringA("[graphics] Xbox device slot +0x230 is an experimental no-op\n");
    if (!VirtualProtect(code, code_size, PAGE_EXECUTE_READ, &old_protection)) {
        VirtualFree(code, 0, MEM_RELEASE);
        HeapFree(GetProcessHeap(), 0, vtable);
        ReleaseSRWLockExclusive(&g_d3d12_device_proxy_lock);
        return FALSE;
    }
    FlushInstructionCache(GetCurrentProcess(), code, code_size);
    g_d3d12_device11_thunks = code;
    g_d3d12_device11_vtable = vtable;
    ReleaseSRWLockExclusive(&g_d3d12_device_proxy_lock);
    return TRUE;
}

static CompatD3D12DeviceProxy *compat_d3d12_device_proxy_create(void *native_device)
{
    CompatD3D12DeviceProxy *proxy;
    if (!native_device || !compat_d3d12_device_vtable_init()) return NULL;
    proxy = (CompatD3D12DeviceProxy *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*proxy));
    if (!proxy) return NULL;
    proxy->vtable = g_d3d12_device11_vtable;
    proxy->native_device = native_device;
    proxy->references = 1;
    return proxy;
}

static BOOL compat_d3d12_debug_requested(void)
{
    char value[16] = {0};
    DWORD length = GetEnvironmentVariableA("COMPAT_D3D12_DEBUG", value, sizeof(value));
    return length == 1 && value[0] == '1';
}

static HRESULT compat_d3d12_enable_debug_layer(HMODULE d3d12)
{
    static const GUID iid_d3d12_debug = {
        0x344488b7, 0x6846, 0x474b, {0xb9,0x89,0xf0,0x27,0x44,0x82,0x45,0xe0}
    };
    typedef HRESULT (WINAPI *get_debug_interface_fn)(REFIID, void **);
    typedef void (STDMETHODCALLTYPE *enable_debug_layer_fn)(void *);
    typedef ULONG (STDMETHODCALLTYPE *release_fn)(void *);
    get_debug_interface_fn get_debug_interface = d3d12
        ? (get_debug_interface_fn)GetProcAddress(d3d12, "D3D12GetDebugInterface") : NULL;
    void *debug = NULL;
    HRESULT result = get_debug_interface
        ? get_debug_interface(&iid_d3d12_debug, &debug) : E_NOTIMPL;
    if (SUCCEEDED(result) && debug) {
        void **vtable = *(void ***)debug;
        ((enable_debug_layer_fn)vtable[3])(debug);
        ((release_fn)vtable[2])(debug);
        OutputDebugStringA("[graphics] D3D12 debug layer enabled before device creation\n");
    } else {
        char message[160];
        snprintf(message, sizeof(message),
            "[graphics] D3D12 debug layer unavailable result=0x%08lx\n",
            (unsigned long)result);
        OutputDebugStringA(message);
    }
    return result;
}

static void CALLBACK compat_d3d12_info_queue_callback(
    int category, int severity, int message_id, const char *description, void *context)
{
    char message[2048];
    (void)context;
    if (severity > 2 || !description) return;
    snprintf(message, sizeof(message),
        "[d3d12-info] category=%d severity=%d id=%d %s\n",
        category, severity, message_id, description);
    OutputDebugStringA(message);
}

static HRESULT compat_d3d12_register_info_queue(void *device)
{
    static const GUID iid_d3d12_info_queue1 = {
        0x2852dd88, 0xb484, 0x4c0c, {0xb6,0xb1,0x67,0x16,0x85,0x00,0xe6,0x00}
    };
    typedef HRESULT (STDMETHODCALLTYPE *query_interface_fn)(void *, REFIID, void **);
    typedef HRESULT (STDMETHODCALLTYPE *register_callback_fn)(
        void *, void (CALLBACK *)(int, int, int, const char *, void *), UINT, void *, DWORD *);
    typedef ULONG (STDMETHODCALLTYPE *release_fn)(void *);
    void *info_queue = NULL;
    DWORD callback_cookie = 0;
    void **device_vtable, **queue_vtable;
    HRESULT result;

    if (!device) return E_POINTER;
    device_vtable = *(void ***)device;
    result = ((query_interface_fn)device_vtable[0])(
        device, &iid_d3d12_info_queue1, &info_queue);
    if (FAILED(result) || !info_queue) return result;
    queue_vtable = *(void ***)info_queue;
    /* ID3D12InfoQueue1::RegisterMessageCallback follows the 36 base methods. */
    result = ((register_callback_fn)queue_vtable[38])(
        info_queue, compat_d3d12_info_queue_callback, 0, NULL, &callback_cookie);
    ((release_fn)queue_vtable[2])(info_queue);
    {
        char message[192];
        snprintf(message, sizeof(message),
            "[graphics] D3D12 info queue callback result=0x%08lx cookie=%lu\n",
            (unsigned long)result, (unsigned long)callback_cookie);
        OutputDebugStringA(message);
    }
    return result;
}

__declspec(dllexport) HRESULT WINAPI D3D12XboxCreateDevice(void *adapter, const void *parameters, const GUID *iid, void **device)
{
    /* The Xbox entry point takes a parameter block, not D3D_FEATURE_LEVEL.
     * The observed IID is the standard ID3D12Device11 IID, so try creating
     * that exact interface through the host runtime. This does not translate
     * Xbox memory/resource semantics; any later unsupported operation must
     * still fail at its real call site. */
    static const GUID iid_d3d12_device11 = {
        0x5405c344, 0xd457, 0x444e,
        {0xb4, 0xdd, 0x23, 0x66, 0xe4, 0x5a, 0xee, 0x39}
    };
    typedef HRESULT (WINAPI *compat_d3d12_create_device_fn)(void *, int, REFIID, void **);
    char message[1200];
    HRESULT result = E_NOTIMPL;

    if (parameters) {
        const unsigned *p = (const unsigned *)parameters;
        snprintf(message, sizeof(message),
            "[graphics] D3D12XboxCreateDevice adapter=%p parameters=%p iid=%p out=%p; params[0..19]=%08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
            adapter, parameters, (const void *)iid, (void *)device,
            p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],p[8],p[9],
            p[10],p[11],p[12],p[13],p[14],p[15],p[16],p[17],p[18],p[19]);
        OutputDebugStringA(message);
    }
    if (device) *device = NULL;
    if (!device || !iid) return E_POINTER;
    if (adapter != NULL || !IsEqualGUID(iid, &iid_d3d12_device11)) return E_NOINTERFACE;

    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    if (!d3d12) d3d12 = LoadLibraryW(L"d3d12.dll");
    if (compat_d3d12_debug_requested()) compat_d3d12_enable_debug_layer(d3d12);
    compat_d3d12_create_device_fn create_device = d3d12
        ? (compat_d3d12_create_device_fn)GetProcAddress(d3d12, "D3D12CreateDevice")
        : NULL;
    if (create_device) {
        /* D3D_FEATURE_LEVEL_11_0 is 0xb000. The host call itself validates
         * device availability and support for the requested IID. */
        result = create_device(NULL, 0xb000, iid, device);
    }
    if (compat_d3d12_debug_requested() && SUCCEEDED(result) && device && *device)
        compat_d3d12_register_info_queue(*device);
    if (SUCCEEDED(result) && device && *device) {
        void *native_device = *device;
        CompatD3D12DeviceProxy *proxy = compat_d3d12_device_proxy_create(native_device);
        if (proxy) {
            *device = proxy;
        } else {
            typedef ULONG (STDMETHODCALLTYPE *release_fn)(void *);
            ((release_fn)(*(void ***)native_device)[2])(native_device);
            *device = NULL;
            result = E_OUTOFMEMORY;
        }
    }
    snprintf(message, sizeof(message),
        "[graphics] native D3D12CreateDevice result=0x%08lx proxy=%p native=%p module=%p export=%p\n",
        (unsigned long)result, device ? *device : NULL,
        (SUCCEEDED(result) && device && *device) ? ((CompatD3D12DeviceProxy *)*device)->native_device : NULL,
        (void *)d3d12,
        create_device ? (void *)create_device : NULL);
    OutputDebugStringA(message);
    return result;
}

__declspec(dllexport) HRESULT WINAPI XGCreateTextureComputer(void *device, const void *description, void **computer)
{
    (void)device;
    (void)description;
    if (computer) *computer = NULL;
    return E_NOTIMPL;
}

__declspec(dllexport) HRESULT WINAPI XGSuggestSwizzleMode(void *description, unsigned *mode)
{
    (void)description;
    if (mode) *mode = 0;
    return E_NOTIMPL;
}

__declspec(dllexport) HRESULT WINAPI DStorageGetFactory(const void *iid, void **factory)
{
    (void)iid;
    if (factory) *factory = NULL;
    return E_NOTIMPL;
}

__declspec(dllexport) HRESULT WINAPI GameInputCreate(void **input)
{
    if (input) *input = NULL;
    return E_NOTIMPL;
}

__declspec(dllexport) HRESULT WINAPI XDspActivate(void *context, unsigned flags)
{
    (void)context;
    (void)flags;
    return S_OK;
}

__declspec(dllexport) HRESULT WINAPI XDspConnectWithMaximumStreamLimit(void *context, unsigned limit)
{
    (void)context;
    (void)limit;
    return S_OK;
}

__declspec(dllexport) HRESULT WINAPI XDspDeactivate(void *context)
{
    (void)context;
    return S_OK;
}

__declspec(dllexport) HRESULT WINAPI XDspDisconnect(void *context)
{
    (void)context;
    return S_OK;
}

__declspec(dllexport) HRESULT WINAPI XDspSubmitCommand(void *context, const void *command, size_t size)
{
    (void)context;
    (void)command;
    (void)size;
    return S_OK;
}

__declspec(dllexport) HRESULT WINAPI XApuConnect(void *context, unsigned flags)
{
    (void)context;
    (void)flags;
    return S_OK;
}

__declspec(dllexport) HRESULT WINAPI XApuDequeueResult(void *context, void *result)
{
    (void)context;
    (void)result;
    return E_NOTIMPL;
}

__declspec(dllexport) HRESULT WINAPI XApuDisconnect(void *context)
{
    (void)context;
    return S_OK;
}

__declspec(dllexport) HRESULT WINAPI XApuEnqueueCommand(void *context, const void *command, size_t size)
{
    (void)context;
    (void)command;
    (void)size;
    return S_OK;
}

__declspec(dllexport) unsigned WINAPI XApuGetResultCount(void *context)
{
    (void)context;
    return 0;
}

__declspec(dllexport) HRESULT WINAPI AcpHalCreate(void **context, unsigned flags)
{
    (void)flags;
    if (context) *context = NULL;
    return E_NOTIMPL;
}

__declspec(dllexport) HRESULT WINAPI AcpHalAllocateShapeContexts(void *context, unsigned count, void **contexts)
{
    (void)context;
    (void)count;
    if (contexts) *contexts = NULL;
    return E_NOTIMPL;
}

__declspec(dllexport) HRESULT WINAPI AcpHalReleaseShapeContexts(void *context, void *contexts)
{
    (void)context;
    (void)contexts;
    return S_OK;
}

__declspec(dllexport) void *WINAPI ApuAlloc(size_t size, unsigned flags)
{
    (void)flags;
    return compat_alloc(size);
}

__declspec(dllexport) void *WINAPI ApuCreateHeap(size_t size, unsigned flags)
{
    (void)flags;
    return compat_alloc(size);
}

__declspec(dllexport) void WINAPI ApuFree(void *ptr, unsigned flags)
{
    (void)flags;
    compat_free(ptr);
}

__declspec(dllexport) BOOL WINAPI ApuIsVirtualAddressValid(const void *address)
{
    MEMORY_BASIC_INFORMATION information;
    return address && VirtualQuery(address, &information, sizeof(information)) != 0;
}

__declspec(dllexport) HRESULT WINAPI ApuMapApuAddress(void *address, size_t size, void *source, unsigned flags)
{
    (void)address;
    (void)size;
    (void)source;
    (void)flags;
    return E_NOTIMPL;
}

__declspec(dllexport) HRESULT WINAPI ApuMapVirtualAddress(void *address, size_t size, void *source, unsigned flags)
{
    (void)address;
    (void)size;
    (void)source;
    (void)flags;
    return E_NOTIMPL;
}
