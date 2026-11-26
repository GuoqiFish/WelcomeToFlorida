#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

typedef void *(WINAPI *XMemVirtualAllocFn)(void *, SIZE_T, unsigned,
    unsigned, unsigned);

typedef struct Segment {
    SIZE_T size;
    unsigned protection;
} Segment;

static const Segment segments[] = {
    { 0x00a00000u, 0x1004u },
    { 0x00100000u, 0x1404u },
    { 0x01b00000u, 0x8404u },
    { 0x02f00000u, 0x2404u },
    { 0x06400000u, 0x2004u }
};

int wmain(int argc, wchar_t **argv)
{
    const SIZE_T total = 0x0b900000u;
    HMODULE module;
    XMemVirtualAllocFn allocate;
    unsigned char *base = NULL;
    SIZE_T offset = 0;
    int result = 1;

    if (argc != 2) {
        fwprintf(stderr, L"Usage: xmem_unified_selftest.exe <xmem.dll>\n");
        return 2;
    }
    SetEnvironmentVariableW(L"COMPAT_XMEM_UNIFIED_RESERVATION", L"1");
    module = LoadLibraryW(argv[1]);
    if (!module) {
        fprintf(stderr, "LoadLibraryW failed: %lu\n", GetLastError());
        return 3;
    }
    allocate = (XMemVirtualAllocFn)GetProcAddress(module, "XMemVirtualAlloc");
    if (!allocate) {
        fprintf(stderr, "GetProcAddress failed: %lu\n", GetLastError());
        goto cleanup;
    }

    base = (unsigned char *)allocate(NULL, total, 0x00042000u, 0x7u,
        PAGE_NOACCESS);
    if (!base) {
        fprintf(stderr, "unified reservation failed: %lu\n", GetLastError());
        goto cleanup;
    }
    printf("unified reserve: base=%p size=0x%llx\n", base,
        (unsigned long long)total);

    for (SIZE_T i = 0; i != sizeof(segments) / sizeof(segments[0]); ++i) {
        unsigned char *target = base + offset;
        void *committed = allocate(target, segments[i].size, 0x20406000u,
            0x27u, segments[i].protection);
        MEMORY_BASIC_INFORMATION mbi;
        if (committed != target) {
            fprintf(stderr, "segment %llu commit failed: result=%p error=%lu\n",
                (unsigned long long)i, committed, GetLastError());
            goto cleanup;
        }
        if (VirtualQuery(target, &mbi, sizeof(mbi)) != sizeof(mbi) ||
            mbi.State != MEM_COMMIT || mbi.AllocationBase != base) {
            fprintf(stderr,
                "segment %llu query mismatch: state=0x%lx allocation=%p expected=%p\n",
                (unsigned long long)i, mbi.State, mbi.AllocationBase, base);
            goto cleanup;
        }
        printf("unified commit %llu: address=%p size=0x%llx allocation=%p\n",
            (unsigned long long)i, target,
            (unsigned long long)segments[i].size, mbi.AllocationBase);
        offset += segments[i].size;
    }
    if (offset != total) {
        fprintf(stderr, "segment total mismatch: 0x%llx != 0x%llx\n",
            (unsigned long long)offset, (unsigned long long)total);
        goto cleanup;
    }
    result = 0;

cleanup:
    if (base && !VirtualFree(base, 0, MEM_RELEASE)) {
        fprintf(stderr, "reservation release failed: %lu\n", GetLastError());
        result = 4;
    }
    FreeLibrary(module);
    return result;
}
