#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

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

__declspec(dllexport) void *WINAPI XMemAlloc(size_t size, unsigned flags)
{
    (void)flags;
    return compat_alloc(size);
}

__declspec(dllexport) void *WINAPI XMemAllocDefault(size_t size, unsigned flags)
{
    (void)flags;
    return compat_alloc(size);
}

__declspec(dllexport) void WINAPI XMemFree(void *ptr, unsigned flags)
{
    (void)flags;
    compat_free(ptr);
}

__declspec(dllexport) void WINAPI XMemFreeDefault(void *ptr, unsigned flags)
{
    (void)flags;
    compat_free(ptr);
}

__declspec(dllexport) void *WINAPI XMemVirtualAlloc(void *address, size_t size,
    unsigned allocation_flags, unsigned attributes, unsigned protection_flags)
{
    const unsigned xbox_64k_pages = 0x20400000u;
    const unsigned xbox_page_cluster = 0x20000000u;
    const unsigned windows_allocation_flags = MEM_COMMIT | MEM_RESERVE | MEM_RESET |
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
        result = VirtualAlloc(address, size, pc_allocation_flags, pc_protection_flags);
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

__declspec(dllexport) BOOL WINAPI XMemAllocatePhysicalPages(void *pages, size_t count, unsigned flags)
{
    (void)pages;
    (void)count;
    (void)flags;
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

__declspec(dllexport) BOOL WINAPI XMemFreePhysicalPages(void *pages, size_t count, unsigned flags)
{
    (void)pages;
    (void)count;
    (void)flags;
    return TRUE;
}

__declspec(dllexport) BOOL WINAPI XMemMapPhysicalPages(void *address, size_t count, void *pages, unsigned flags)
{
    (void)address;
    (void)count;
    (void)pages;
    (void)flags;
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
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
    return function(description, blob, error_blob);
}

__declspec(dllexport) HRESULT WINAPI D3D12XboxCreateDevice(void *adapter, const void *parameters, const GUID *iid, void **device)
{
    /* The Xbox call takes a parameters pointer, NOT a D3D_FEATURE_LEVEL.
     * Its returned device has Xbox extension slots and a different struct-
     * return ABI. A raw PC device is not safe to return at this boundary. */
    char message[1024];
    snprintf(message, sizeof(message),
        "[graphics] D3D12XboxCreateDevice adapter=%p parameters=%p iid=%p out=%p; device ABI adapter not implemented\n",
        adapter, parameters, (const void *)iid, (void *)device);
    OutputDebugStringA(message);
    if (parameters) {
        const unsigned *p = (const unsigned *)parameters;
        snprintf(message, sizeof(message),
            "[graphics] create parameters[0..19]=%08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
            p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],p[8],p[9],
            p[10],p[11],p[12],p[13],p[14],p[15],p[16],p[17],p[18],p[19]);
        OutputDebugStringA(message);
    }
    if (device) *device = NULL;
    return E_NOTIMPL;
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
