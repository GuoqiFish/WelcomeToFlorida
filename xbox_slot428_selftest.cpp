#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <cstring>

using CreateXboxDeviceFn = HRESULT (WINAPI *)(void *, const void *, REFIID, void **);
using CreateXmemBufferFn = HRESULT (STDMETHODCALLTYPE *)(void *, void *,
    const void *, UINT32, const void *, REFIID, void **);

static bool reserve_and_commit(void **base_out)
{
    constexpr SIZE_T total = 0x0b900000u;
    constexpr SIZE_T segments[] = {
        0x00a00000u, 0x00100000u, 0x01b00000u, 0x02f00000u, 0x06400000u
    };
    void *base = VirtualAlloc(nullptr, total, MEM_RESERVE, PAGE_NOACCESS);
    if (!base) {
        std::printf("VirtualAlloc reserve failed error=%lu\n", GetLastError());
        return false;
    }
    SIZE_T offset = 0;
    for (SIZE_T size : segments) {
        void *segment = static_cast<unsigned char *>(base) + offset;
        if (VirtualAlloc(segment, size, MEM_COMMIT, PAGE_READWRITE) != segment) {
            std::printf("VirtualAlloc commit offset=0x%llx failed error=%lu\n",
                static_cast<unsigned long long>(offset), GetLastError());
            VirtualFree(base, 0, MEM_RELEASE);
            return false;
        }
        offset += size;
    }
    *base_out = base;
    return true;
}

int wmain(int argc, wchar_t **argv)
{
    static const GUID iid_d3d12_device11 = {
        0x5405c344, 0xd457, 0x444e, {0xb4,0xdd,0x23,0x66,0xe4,0x5a,0xee,0x39}
    };
    static const GUID iid_d3d12_resource = {
        0x696442be, 0xa72e, 0x4059, {0xbc,0x79,0x5b,0x5c,0x98,0x04,0x0f,0xad}
    };
    constexpr SIZE_T offset = 0x00b00000u;
    constexpr SIZE_T width = 0x0ae00000u;
    if (argc > 2) {
        fwprintf(stderr, L"Usage: xbox_slot428_selftest.exe [d3d12_xs.dll]\n");
        return 2;
    }
    SetEnvironmentVariableW(L"COMPAT_D3D12_XBOX_SLOT428_BUFFER", L"1");
    SetEnvironmentVariableW(L"COMPAT_D3D12_DEBUG", L"0");

    wchar_t sibling_dll[MAX_PATH];
    const wchar_t *dll_path = argc == 2 ? argv[1] : sibling_dll;
    if (argc == 1) {
        DWORD path_length = GetModuleFileNameW(NULL, sibling_dll, MAX_PATH);
        if (!path_length || path_length >= MAX_PATH) return 2;
        wchar_t *slash = wcsrchr(sibling_dll, L'\\');
        if (!slash || (size_t)(slash - sibling_dll) + 1 +
                wcslen(L"d3d12_xs.dll") >= MAX_PATH) return 2;
        wcscpy_s(slash + 1, MAX_PATH - (size_t)(slash + 1 - sibling_dll),
            L"d3d12_xs.dll");
    }
    HMODULE module = LoadLibraryW(dll_path);
    if (!module) {
        std::printf("LoadLibrary d3d12_xs.dll failed error=%lu\n", GetLastError());
        return 3;
    }
    auto create_device = reinterpret_cast<CreateXboxDeviceFn>(
        GetProcAddress(module, "D3D12XboxCreateDevice"));
    if (!create_device) {
        std::printf("D3D12XboxCreateDevice export missing error=%lu\n", GetLastError());
        FreeLibrary(module);
        return 4;
    }

    void *region = nullptr;
    void *proxy = nullptr;
    void *resource = nullptr;
    int result = 1;
    HRESULT hr = S_OK;
    if (!reserve_and_commit(&region)) goto cleanup;
    {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = width;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x00020004u);
        unsigned char xbox_desc[64] = {};
        std::memcpy(xbox_desc, &desc, sizeof(desc));

        hr = create_device(nullptr, nullptr, iid_d3d12_device11, &proxy);
        if (FAILED(hr) || !proxy) {
            std::printf("D3D12XboxCreateDevice HRESULT=0x%08lx proxy=%p\n",
                static_cast<unsigned long>(hr), proxy);
            goto cleanup;
        }
        auto **vtable = *reinterpret_cast<void ***>(proxy);
        auto create_resource = reinterpret_cast<CreateXmemBufferFn>(
            vtable[0x428 / sizeof(void *)]);
        if (!create_resource) {
            std::printf("proxy slot +0x428 is null\n");
            goto cleanup;
        }
        hr = create_resource(proxy, static_cast<unsigned char *>(region) + offset,
            xbox_desc, 0xffffffffu, nullptr, iid_d3d12_resource, &resource);
        std::printf("slot428 HRESULT=0x%08lx resource=%p cpu_address=%p\n",
            static_cast<unsigned long>(hr), resource,
            static_cast<unsigned char *>(region) + offset);
        if (FAILED(hr) || !resource) goto cleanup;

        auto **resource_vtable = *reinterpret_cast<void ***>(resource);
        using GetGpuAddressFn = D3D12_GPU_VIRTUAL_ADDRESS (STDMETHODCALLTYPE *)(void *);
        using MapFn = HRESULT (STDMETHODCALLTYPE *)(void *, UINT, const D3D12_RANGE *, void **);
        using UnmapFn = void (STDMETHODCALLTYPE *)(void *, UINT, const D3D12_RANGE *);
        const auto gpu_address = reinterpret_cast<GetGpuAddressFn>(resource_vtable[11])(resource);
        void *mapped = nullptr;
        D3D12_RANGE read_range = {0, 0};
        hr = reinterpret_cast<MapFn>(resource_vtable[8])(resource, 0, &read_range, &mapped);
        if (FAILED(hr) || mapped != static_cast<unsigned char *>(region) + offset) {
            std::printf("resource Map HRESULT=0x%08lx mapped=%p expected=%p\n",
                static_cast<unsigned long>(hr), mapped,
                static_cast<unsigned char *>(region) + offset);
            goto cleanup;
        }
        static_cast<unsigned char *>(mapped)[0x200] = 0x5a;
        const bool alias_ok = static_cast<unsigned char *>(region)[offset + 0x200] == 0x5a;
        reinterpret_cast<UnmapFn>(resource_vtable[9])(resource, 0, nullptr);
        std::printf("slot428 placed resource map=%p alias=%s gpuVA=0x%llx\n",
            mapped, alias_ok ? "yes" : "no",
            static_cast<unsigned long long>(gpu_address));
        if (!alias_ok) goto cleanup;
        result = 0;
    }

cleanup:
    if (resource) {
        auto **resource_vtable = *reinterpret_cast<void ***>(resource);
        using ReleaseFn = ULONG (STDMETHODCALLTYPE *)(void *);
        reinterpret_cast<ReleaseFn>(resource_vtable[2])(resource);
    }
    if (proxy) {
        auto **proxy_vtable = *reinterpret_cast<void ***>(proxy);
        using ReleaseFn = ULONG (STDMETHODCALLTYPE *)(void *);
        reinterpret_cast<ReleaseFn>(proxy_vtable[2])(proxy);
    }
    if (region && !VirtualFree(region, 0, MEM_RELEASE)) {
        std::printf("region cleanup failed error=%lu\n", GetLastError());
        result = 5;
    }
    FreeLibrary(module);
    return result;
}
