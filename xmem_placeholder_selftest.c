#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

typedef void *(WINAPI *XMemVirtualAllocFn)(void *, SIZE_T, unsigned,
    unsigned, unsigned);

typedef struct PlaceholderSegment {
    SIZE_T size;
    unsigned protection;
} PlaceholderSegment;

static const PlaceholderSegment segments[] = {
    { 0x00a00000u, 0x1004u },
    { 0x00100000u, 0x1404u },
    { 0x01b00000u, 0x8404u },
    { 0x02f00000u, 0x2404u },
    { 0x06400000u, 0x2004u }
};

int wmain(int argc, wchar_t **argv)
{
    const SIZE_T total_size = 0x0b900000u;
    HMODULE module = NULL;
    XMemVirtualAllocFn xmem_virtual_alloc = NULL;
    unsigned char *base = NULL;
    unsigned char *replaced[sizeof(segments) / sizeof(segments[0])] = { 0 };
    unsigned char *pending_placeholder = NULL;
    unsigned char *remaining_placeholder = NULL;
    SIZE_T offset = 0;
    BOOL initial_reservation = FALSE;
    BOOL pending_is_placeholder = FALSE;
    BOOL remaining_is_placeholder = FALSE;
    BOOL cleanup_ok = TRUE;
    int result = 1;

    if (argc != 2) {
        fwprintf(stderr, L"Usage: xmem_placeholder_selftest.exe <xmem.dll>\n");
        return 2;
    }
    SetEnvironmentVariableW(L"COMPAT_XMEM_EXACT_VA_RESERVATION", L"1");
    module = LoadLibraryW(argv[1]);
    if (!module) {
        fprintf(stderr, "LoadLibraryW failed: %lu\n", GetLastError());
        return 3;
    }
    xmem_virtual_alloc = (XMemVirtualAllocFn)GetProcAddress(module,
        "XMemVirtualAlloc");
    if (!xmem_virtual_alloc) {
        fprintf(stderr, "GetProcAddress failed: %lu\n", GetLastError());
        goto cleanup;
    }

    base = (unsigned char *)xmem_virtual_alloc(NULL, total_size, 0x00042000u,
        0x7u, PAGE_NOACCESS);
    if (!base) {
        fprintf(stderr, "XMem placeholder reserve failed: %lu\n", GetLastError());
        goto cleanup;
    }
    initial_reservation = TRUE;
    printf("XMem placeholder reserve: base=%p size=0x%llx\n", base,
        (unsigned long long)total_size);

    for (SIZE_T i = 0; i < sizeof(segments) / sizeof(segments[0]); ++i) {
        SIZE_T size = segments[i].size;
        unsigned char *target = base + offset;

        if (i + 1 < sizeof(segments) / sizeof(segments[0])) {
            SetLastError(ERROR_SUCCESS);
            if (!VirtualFree(target, size,
                    MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
                fprintf(stderr, "split segment %llu failed: %lu\n",
                    (unsigned long long)i, GetLastError());
                goto cleanup;
            }
            pending_placeholder = target;
            pending_is_placeholder = TRUE;
            remaining_placeholder = target + size;
            remaining_is_placeholder = TRUE;
            initial_reservation = FALSE;
            printf("placeholder split %llu: address=%p size=0x%llx\n",
                (unsigned long long)i, target, (unsigned long long)size);
        } else {
            /* The final 100 MiB is the untouched remainder of the original
             * reservation after the first four placeholder splits. */
            pending_placeholder = target;
            pending_is_placeholder = TRUE;
        }

        replaced[i] = (unsigned char *)xmem_virtual_alloc(target, size,
            0x20406000u, 0x27u, segments[i].protection);
        if (replaced[i] != target) {
            fprintf(stderr,
                "replace segment %llu failed: result=%p expected=%p error=%lu\n",
                (unsigned long long)i, replaced[i], target, GetLastError());
            goto cleanup;
        }
        pending_is_placeholder = FALSE;
        if (i + 1 == sizeof(segments) / sizeof(segments[0])) {
            remaining_placeholder = NULL;
            remaining_is_placeholder = FALSE;
        }

        MEMORY_BASIC_INFORMATION mbi;
        SIZE_T queried = VirtualQuery(target, &mbi, sizeof(mbi));
        if (queried != sizeof(mbi) || mbi.State != MEM_RESERVE ||
            (unsigned char *)mbi.BaseAddress > target ||
            (unsigned char *)mbi.BaseAddress + mbi.RegionSize < target + size) {
            fprintf(stderr,
                "VirtualQuery segment %llu mismatch: query=%llu base=%p size=0x%llx state=0x%lx\n",
                (unsigned long long)i, (unsigned long long)queried,
                mbi.BaseAddress, (unsigned long long)mbi.RegionSize, mbi.State);
            goto cleanup;
        }
        printf("placeholder replace %llu: address=%p size=0x%llx protect=0x%lx state=MEM_RESERVE\n",
            (unsigned long long)i, target, (unsigned long long)size,
            mbi.Protect);
        offset += size;
    }

    if (offset != total_size) {
        fprintf(stderr, "segment sizes total 0x%llx, expected 0x%llx\n",
            (unsigned long long)offset, (unsigned long long)total_size);
        goto cleanup;
    }
    result = 0;

cleanup:
    for (SIZE_T i = 0; i < sizeof(segments) / sizeof(segments[0]); ++i) {
        if (replaced[i]) {
            SetLastError(ERROR_SUCCESS);
            if (!VirtualFree(replaced[i], 0, MEM_RELEASE)) {
                fprintf(stderr, "release replacement %llu failed: %lu\n",
                    (unsigned long long)i, GetLastError());
                cleanup_ok = FALSE;
            }
        }
    }
    if (pending_is_placeholder && pending_placeholder) {
        SetLastError(ERROR_SUCCESS);
        if (!VirtualFree(pending_placeholder, 0, MEM_RELEASE)) {
            fprintf(stderr, "release pending placeholder failed: %lu\n",
                GetLastError());
            cleanup_ok = FALSE;
        }
        if (pending_placeholder == base)
            initial_reservation = FALSE;
    }
    if (remaining_is_placeholder && remaining_placeholder) {
        SetLastError(ERROR_SUCCESS);
        if (!VirtualFree(remaining_placeholder, 0, MEM_RELEASE)) {
            fprintf(stderr, "release remaining placeholder failed: %lu\n",
                GetLastError());
            cleanup_ok = FALSE;
        }
        if (remaining_placeholder == base)
            initial_reservation = FALSE;
    }
    if (initial_reservation && base) {
        SetLastError(ERROR_SUCCESS);
        if (!VirtualFree(base, 0, MEM_RELEASE)) {
            fprintf(stderr, "release original placeholder failed: %lu\n",
                GetLastError());
            cleanup_ok = FALSE;
        }
    }
    if (!cleanup_ok) result = 4;
    FreeLibrary(module);
    return result;
}
