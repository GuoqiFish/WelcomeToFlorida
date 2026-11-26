#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <cstring>

typedef HRESULT (WINAPI *create_xbox_device_fn)(void *, const void *, const GUID *, void **);
typedef D3D12_RESOURCE_ALLOCATION_INFO *(STDMETHODCALLTYPE *get_allocation_info_xbox_fn)(
    D3D12_RESOURCE_ALLOCATION_INFO *, void *, UINT, UINT, const D3D12_RESOURCE_DESC *);
typedef ULONG (STDMETHODCALLTYPE *release_fn)(void *);

static const GUID iid_d3d12_device11 = {
    0x5405c344, 0xd457, 0x444e, {0xb4,0xdd,0x23,0x66,0xe4,0x5a,0xee,0x39}
};

static bool check_invalid_query(get_allocation_info_xbox_fn get_info, void *proxy,
    UINT mask, UINT count, const D3D12_RESOURCE_DESC *desc, const char *label)
{
    D3D12_RESOURCE_ALLOCATION_INFO actual = {};
    D3D12_RESOURCE_ALLOCATION_INFO *returned = get_info(&actual, proxy, mask, count, desc);
    if (returned != &actual || actual.SizeInBytes != 0xffffffffffffffffull) {
        std::fprintf(stderr, "%s unexpectedly changed: result=%p size=0x%llx alignment=0x%llx\n",
            label, returned, (unsigned long long)actual.SizeInBytes,
            (unsigned long long)actual.Alignment);
        return false;
    }
    return true;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc != 2) {
        fwprintf(stderr, L"Usage: xbox_allocation_info_selftest.exe D3D12_XS_DLL\n");
        return 2;
    }
    HMODULE module = LoadLibraryW(argv[1]);
    if (!module) {
        std::fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError());
        return 3;
    }
    void *exported = (void *)GetProcAddress(module, "D3D12XboxCreateDevice");
    if (!exported) {
        std::fprintf(stderr, "D3D12XboxCreateDevice export is missing\n");
        FreeLibrary(module);
        return 4;
    }
    SetEnvironmentVariableA("COMPAT_D3D12_TRANSLATE_OBSERVED_ALLOCATION_FLAGS", "1");
    void *proxy = nullptr;
    HRESULT hr = ((create_xbox_device_fn)exported)(nullptr, nullptr, &iid_d3d12_device11, &proxy);
    if (FAILED(hr) || !proxy) {
        std::fprintf(stderr, "D3D12XboxCreateDevice failed: 0x%08lx\n", (unsigned long)hr);
        FreeLibrary(module);
        return 5;
    }

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 3840;
    desc.Height = 2160;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc = {1, 0};
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = (D3D12_RESOURCE_FLAGS)0x00300005;
    const D3D12_RESOURCE_DESC original_desc = desc;
    D3D12_RESOURCE_DESC two_descs[] = {desc, desc};
    void **vtable = *(void ***)proxy;
    get_allocation_info_xbox_fn get_info = (get_allocation_info_xbox_fn)vtable[25];
    bool passed = true;

    D3D12_RESOURCE_ALLOCATION_INFO translated = {};
    D3D12_RESOURCE_ALLOCATION_INFO *returned = get_info(&translated, proxy, 1, 1, &desc);
    if (returned != &translated || translated.SizeInBytes == 0xffffffffffffffffull || translated.SizeInBytes == 0 ||
        translated.Alignment == 0 || std::memcmp(&desc, &original_desc, sizeof(desc)) != 0) {
        std::fprintf(stderr, "exact observed query translation failed: result=%p size=0x%llx alignment=0x%llx\n",
            returned, (unsigned long long)translated.SizeInBytes,
            (unsigned long long)translated.Alignment);
        passed = false;
    } else {
        std::printf("exact observed query translated: result=%p size=0x%llx alignment=0x%llx\n",
            returned, (unsigned long long)translated.SizeInBytes,
            (unsigned long long)translated.Alignment);
    }

    D3D12_RESOURCE_DESC near_miss = desc;
    near_miss.Flags = (D3D12_RESOURCE_FLAGS)0x00300004;
    passed &= check_invalid_query(get_info, proxy, 1, 1, &near_miss, "flag near-miss");
    passed &= check_invalid_query(get_info, proxy, 2, 1, &desc, "visible-mask near-miss");
    passed &= check_invalid_query(get_info, proxy, 1, 2, two_descs, "count near-miss");

    SetEnvironmentVariableA("COMPAT_D3D12_TRANSLATE_OBSERVED_ALLOCATION_FLAGS", "0");
    passed &= check_invalid_query(get_info, proxy, 1, 1, &desc, "translation-disabled query");
    if (std::memcmp(&desc, &original_desc, sizeof(desc)) != 0) {
        std::fprintf(stderr, "source descriptor was modified\n");
        passed = false;
    }

    ((release_fn)vtable[2])(proxy);
    FreeLibrary(module);
    if (!passed) return 6;
    std::printf("slot 25 allocation-info ABI and fail-closed gates: ok\n");
    return 0;
}
