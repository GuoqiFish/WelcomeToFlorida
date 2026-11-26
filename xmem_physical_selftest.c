#include <windows.h>
#include <stdint.h>
#include <stdio.h>

typedef BOOL (WINAPI *PFN_ALLOC)(ULONG_PTR, ULONG_PTR *, ULONG_PTR *);
typedef BOOL (WINAPI *PFN_FREE)(ULONG_PTR, ULONG_PTR *);
typedef void *(WINAPI *PFN_MAP)(void *, ULONG_PTR, ULONG_PTR *);
typedef void *(WINAPI *PFN_VIRTUAL_ALLOC)(void *, SIZE_T, unsigned, unsigned, unsigned);

static int fail(const char *message)
{
    fprintf(stderr, "FAIL: %s (error=%lu)\n", message, GetLastError());
    return 1;
}

int wmain(int argc, wchar_t **argv)
{
    HMODULE module;
    PFN_ALLOC allocate_pages;
    PFN_FREE free_pages;
    PFN_MAP map_pages;
    PFN_VIRTUAL_ALLOC virtual_alloc;
    ULONG_PTR pages[2] = { 0, 0 };
    ULONG_PTR count = 2;
    unsigned char *first = NULL, *second = NULL;
    unsigned char *virtual_page = NULL;
    int result = 1;

    if (argc != 2) {
        fprintf(stderr, "Usage: xmem_physical_selftest.exe <xmem.dll>\n");
        return 2;
    }
    module = LoadLibraryW(argv[1]);
    if (!module) return fail("LoadLibraryW");
    allocate_pages = (PFN_ALLOC)GetProcAddress(module, "XMemAllocatePhysicalPages");
    free_pages = (PFN_FREE)GetProcAddress(module, "XMemFreePhysicalPages");
    map_pages = (PFN_MAP)GetProcAddress(module, "XMemMapPhysicalPages");
    virtual_alloc = (PFN_VIRTUAL_ALLOC)GetProcAddress(module, "XMemVirtualAlloc");
    if (!allocate_pages || !free_pages || !map_pages || !virtual_alloc) {
        fail("resolve XMem exports");
        goto done;
    }

    virtual_page = (unsigned char *)virtual_alloc(NULL, 0x10000, 0x20401000u,
        0x283u, 0x42404u);
    if (!virtual_page || ((uintptr_t)virtual_page & 0xffffu)) {
        fail("translate observed 64 KiB graphics allocation");
        goto done;
    }
    virtual_page[0] = 0x5a;
    if (virtual_page[0] != 0x5a) {
        fail("read/write translated 64 KiB allocation");
        goto done;
    }

    first = (unsigned char *)VirtualAlloc(NULL, 0x20000, MEM_RESERVE, PAGE_NOACCESS);
    second = (unsigned char *)VirtualAlloc(NULL, 0x20000, MEM_RESERVE, PAGE_NOACCESS);
    if (!first || !second) {
        fail("reserve virtual ranges");
        goto done;
    }
    if (!allocate_pages(0x280, &count, pages)) {
        fail("allocate two 64 KiB pages");
        goto done;
    }
    if (count != 2 || !pages[0] || !pages[1] || pages[0] == pages[1] ||
        (pages[0] & 0xffffu) || (pages[1] & 0xffffu)) {
        fail("validate allocated page array");
        goto free_pages;
    }
    if (map_pages(first, count, pages) != first) {
        fail("map pages into first reservation");
        goto free_pages;
    }
    first[0] = 0x5a;
    first[0x10000] = 0xa5;
    if (map_pages(second, count, pages) != second ||
        second[0] != 0x5a || second[0x10000] != 0xa5) {
        fail("move pages and preserve contents");
        goto free_pages;
    }
    if (map_pages(second, count, NULL) != second) {
        fail("unmap pages");
        goto free_pages;
    }
    if (!free_pages(count, pages)) {
        fail("free physical pages");
        goto done;
    }
    result = 0;
    puts("PASS: 64 KiB graphics allocation and physical page allocate/map/move/unmap/free");
    goto done;

free_pages:
    /* Unmap whichever reservation currently owns the pages before release. */
    if (pages[0]) {
        map_pages(first, count, NULL);
        map_pages(second, count, NULL);
        free_pages(count, pages);
    }
done:
    if (virtual_page) VirtualFree(virtual_page, 0, MEM_RELEASE);
    if (first) VirtualFree(first, 0, MEM_RELEASE);
    if (second) VirtualFree(second, 0, MEM_RELEASE);
    if (module) FreeLibrary(module);
    return result;
}
