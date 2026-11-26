#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif
#ifndef MEM_COALESCE_PLACEHOLDERS
#define MEM_COALESCE_PLACEHOLDERS 0x00000001
#endif

typedef PVOID (WINAPI *VirtualAlloc2Fn)(HANDLE, PVOID, SIZE_T, ULONG,
                                        ULONG, void *, ULONG);

static void describe_region(const char *label, const void *address) {
    MEMORY_BASIC_INFORMATION mbi;
    SIZE_T got = VirtualQuery(address, &mbi, sizeof(mbi));
    if (got == 0) {
        printf("%s: VirtualQuery(%p) failed, error=%lu\n", label, address,
               GetLastError());
        return;
    }
    printf("%s: query=%llu Base=%p AllocationBase=%p RegionSize=0x%llx "
           "State=0x%lx Protect=0x%lx Type=0x%lx\n",
           label, (unsigned long long)got, mbi.BaseAddress, mbi.AllocationBase,
           (unsigned long long)mbi.RegionSize, mbi.State, mbi.Protect, mbi.Type);
}

int main(void) {
    const SIZE_T total = 0x0b900000;
    const SIZE_T first = 0x00a00000;
    const ULONG reserveFlags = MEM_RESERVE | MEM_RESERVE_PLACEHOLDER;
    HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
    VirtualAlloc2Fn virtualAlloc2 = kernelbase
        ? (VirtualAlloc2Fn)(void *)GetProcAddress(kernelbase, "VirtualAlloc2")
        : NULL;
    PVOID base;
    BOOL ok;
    BOOL splitDone;
    DWORD error;

    printf("placeholder self-test: total=0x%llx split=0x%llx\n",
           (unsigned long long)total, (unsigned long long)first);
    if (!virtualAlloc2) {
        printf("VirtualAlloc2 unavailable, error=%lu\n", GetLastError());
        return 2;
    }

    /* Exact ordinary-reservation control: do not touch the memory contents. */
    base = VirtualAlloc(NULL, total, MEM_RESERVE, PAGE_NOACCESS);
    error = base ? ERROR_SUCCESS : GetLastError();
    printf("ordinary VirtualAlloc: base=%p error=%lu\n", base, error);
    if (base) {
        describe_region("ordinary before split", base);
        SetLastError(ERROR_SUCCESS);
        ok = VirtualFree(base, first, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
        error = ok ? ERROR_SUCCESS : GetLastError();
        printf("ordinary split attempt: ok=%d error=%lu\n", ok, error);
        describe_region("ordinary after split attempt", base);
        if (!ok) {
            SetLastError(ERROR_SUCCESS);
            ok = VirtualFree(base, 0, MEM_RELEASE);
            error = ok ? ERROR_SUCCESS : GetLastError();
            printf("ordinary cleanup: ok=%d error=%lu\n", ok, error);
        }
        /* If the control behaves unexpectedly, process exit reclaims its VA. */
    }

    /* Correct placeholder path, using the same size and split offset. */
    SetLastError(ERROR_SUCCESS);
    base = virtualAlloc2(GetCurrentProcess(), NULL, total, reserveFlags,
                         PAGE_NOACCESS, NULL, 0);
    error = base ? ERROR_SUCCESS : GetLastError();
    printf("VirtualAlloc2 placeholder: base=%p error=%lu flags=0x%lx\n",
           base, error, reserveFlags);
    if (!base)
        return 3;

    describe_region("placeholder before split", base);
    describe_region("placeholder split boundary", (BYTE *)base + first);

    SetLastError(ERROR_SUCCESS);
    ok = VirtualFree(base, first, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    splitDone = ok;
    error = ok ? ERROR_SUCCESS : GetLastError();
    printf("placeholder split: ok=%d error=%lu flags=0x%lx\n", ok, error,
           (DWORD)(MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER));
    if (ok) {
        describe_region("first placeholder after split", base);
        describe_region("second placeholder after split", (BYTE *)base + first);

        SetLastError(ERROR_SUCCESS);
        ok = VirtualFree(base, total, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
        error = ok ? ERROR_SUCCESS : GetLastError();
        printf("placeholder coalesce: ok=%d error=%lu\n", ok, error);
    }
    if (!ok) {
        if (splitDone) {
            /* If still split, release each placeholder individually. */
            VirtualFree(base, 0, MEM_RELEASE);
            VirtualFree((BYTE *)base + first, 0, MEM_RELEASE);
        } else {
            VirtualFree(base, 0, MEM_RELEASE);
        }
    } else {
        /* Coalesced to one placeholder: release its full range. */
        SetLastError(ERROR_SUCCESS);
        ok = VirtualFree(base, 0, MEM_RELEASE);
        error = ok ? ERROR_SUCCESS : GetLastError();
        printf("placeholder final release: ok=%d error=%lu\n", ok, error);
    }
    return 0;
}
