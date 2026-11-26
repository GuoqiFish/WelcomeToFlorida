#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif
#ifndef MEM_64K_PAGES
#define MEM_64K_PAGES 0x20400000
#endif

typedef PVOID (WINAPI *VirtualAlloc2Fn)(HANDLE, PVOID, SIZE_T, ULONG,
                                        ULONG, void *, ULONG);

static void show_region(const char *label, const void *address) {
    MEMORY_BASIC_INFORMATION mbi;
    SIZE_T got = VirtualQuery(address, &mbi, sizeof(mbi));
    if (!got) {
        printf("  %s: VirtualQuery failed error=%lu\n", label, GetLastError());
        return;
    }
    printf("  %s: base=%p allocationBase=%p size=0x%llx state=0x%lx "
           "protect=0x%lx type=0x%lx\n",
           label, mbi.BaseAddress, mbi.AllocationBase,
           (unsigned long long)mbi.RegionSize, mbi.State, mbi.Protect, mbi.Type);
}

static BOOL run_case(VirtualAlloc2Fn virtualAlloc2, const char *name,
                     ULONG replacementFlags, ULONG protection) {
    const SIZE_T total = 0x0b900000;
    const SIZE_T replacedSize = 0x00a00000;
    const ULONG placeholderFlags = MEM_RESERVE | MEM_RESERVE_PLACEHOLDER;
    PVOID base = NULL;
    PVOID tail;
    PVOID replaced;
    BOOL splitOk;
    BOOL replacedOk;
    DWORD error;

    printf("\ncase %s: flags=0x%08lx protection=0x%lx size=0x%llx\n",
           name, replacementFlags, protection,
           (unsigned long long)replacedSize);

    SetLastError(ERROR_SUCCESS);
    base = virtualAlloc2(GetCurrentProcess(), NULL, total, placeholderFlags,
                         PAGE_NOACCESS, NULL, 0);
    error = base ? ERROR_SUCCESS : GetLastError();
    printf("  placeholder reserve: base=%p error=%lu flags=0x%lx\n",
           base, error, placeholderFlags);
    if (!base)
        return FALSE;

    SetLastError(ERROR_SUCCESS);
    splitOk = VirtualFree(base, replacedSize,
                          MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    error = splitOk ? ERROR_SUCCESS : GetLastError();
    printf("  split placeholder: ok=%d error=%lu\n", splitOk, error);
    if (!splitOk) {
        SetLastError(ERROR_SUCCESS);
        replacedOk = VirtualFree(base, 0, MEM_RELEASE);
        error = replacedOk ? ERROR_SUCCESS : GetLastError();
        printf("  cleanup whole placeholder: ok=%d error=%lu\n",
               replacedOk, error);
        return FALSE;
    }

    tail = (BYTE *)base + replacedSize;
    show_region("replacement target before", base);
    show_region("untouched placeholder tail", tail);

    SetLastError(ERROR_SUCCESS);
    replaced = virtualAlloc2(GetCurrentProcess(), base, replacedSize,
                             replacementFlags, protection, NULL, 0);
    error = replaced ? ERROR_SUCCESS : GetLastError();
    replacedOk = replaced != NULL;
    printf("  replace placeholder: result=%p ok=%d error=%lu\n",
           replaced, replacedOk, error);
    if (replacedOk)
        show_region("replacement result", replaced);
    else
        show_region("placeholder after failed replacement", base);

    /* No bytes are read or written. Release the replaced region and tail. */
    if (replacedOk) {
        SetLastError(ERROR_SUCCESS);
        splitOk = VirtualFree(replaced, 0, MEM_RELEASE);
        error = splitOk ? ERROR_SUCCESS : GetLastError();
        printf("  cleanup replaced region: ok=%d error=%lu\n", splitOk, error);
        if (!splitOk)
            return FALSE;
    } else {
        SetLastError(ERROR_SUCCESS);
        splitOk = VirtualFree(base, 0, MEM_RELEASE);
        error = splitOk ? ERROR_SUCCESS : GetLastError();
        printf("  cleanup target placeholder: ok=%d error=%lu\n", splitOk, error);
        if (!splitOk)
            return FALSE;
    }

    SetLastError(ERROR_SUCCESS);
    splitOk = VirtualFree(tail, 0, MEM_RELEASE);
    error = splitOk ? ERROR_SUCCESS : GetLastError();
    printf("  cleanup tail placeholder: ok=%d error=%lu\n", splitOk, error);
    return splitOk;
}

int main(void) {
    HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
    VirtualAlloc2Fn virtualAlloc2 = kernelbase
        ? (VirtualAlloc2Fn)(void *)GetProcAddress(kernelbase, "VirtualAlloc2")
        : NULL;
    BOOL allCleaned = TRUE;

    if (!virtualAlloc2) {
        printf("VirtualAlloc2 unavailable, error=%lu\n", GetLastError());
        return 2;
    }

    /* Keep the basic reserve/replace checks separate from commit behavior. */
    allCleaned &= run_case(virtualAlloc2, "RESERVE|REPLACE_PLACEHOLDER",
                           MEM_RESERVE | MEM_REPLACE_PLACEHOLDER,
                           PAGE_NOACCESS);
    allCleaned &= run_case(virtualAlloc2,
                           "RESERVE|COMMIT|REPLACE_PLACEHOLDER",
                           MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
                           PAGE_READWRITE);
    allCleaned &= run_case(virtualAlloc2,
                           "RESERVE|REPLACE_PLACEHOLDER|MEM_64K_PAGES",
                           MEM_RESERVE | MEM_REPLACE_PLACEHOLDER | MEM_64K_PAGES,
                           PAGE_NOACCESS);
    allCleaned &= run_case(virtualAlloc2,
                           "RESERVE|COMMIT|REPLACE_PLACEHOLDER|MEM_64K_PAGES",
                           MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER |
                               MEM_64K_PAGES,
                           PAGE_READWRITE);

    printf("\nall cleanup paths completed: %s\n", allCleaned ? "yes" : "no");
    return allCleaned ? 0 : 1;
}
