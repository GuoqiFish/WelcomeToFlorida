#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <cstring>

typedef HRESULT (WINAPI *create_xbox_device_fn)(void *, const void *, const GUID *, void **);
typedef HRESULT (STDMETHODCALLTYPE *create_committed_resource3_xbox_fn)(
    void *, const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC1 *, D3D12_BARRIER_LAYOUT,
    const D3D12_CLEAR_VALUE *, ID3D12ProtectedResourceSession *, UINT,
    const DXGI_FORMAT *, const GUID *, void **);
typedef ULONG (STDMETHODCALLTYPE *release_fn)(void *);

static const GUID iid_d3d12_device11 = {
    0x5405c344, 0xd457, 0x444e, {0xb4,0xdd,0x23,0x66,0xe4,0x5a,0xee,0x39}
};
static const GUID iid_d3d12_resource = {
    0x696442be, 0xa72e, 0x4059, {0xbc,0x79,0x5b,0x5c,0x98,0x04,0x0f,0xad}
};

int wmain(int argc, wchar_t **argv)
{
    if (argc != 2) {
        fwprintf(stderr, L"Usage: xbox_committed_resource3_selftest.exe D3D12_XS_DLL\n");
        return 2;
    }
    SetEnvironmentVariableA("COMPAT_D3D12_XBOX_SLOT3F8_COMMITTED_RESOURCE3", "1");
    HMODULE module = LoadLibraryW(argv[1]);
    if (!module) {
        std::fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError());
        return 3;
    }
    create_xbox_device_fn create_device = (create_xbox_device_fn)
        GetProcAddress(module, "D3D12XboxCreateDevice");
    if (!create_device) {
        std::fprintf(stderr, "D3D12XboxCreateDevice export is missing\n");
        FreeLibrary(module);
        return 4;
    }
    void *proxy = nullptr;
    HRESULT hr = create_device(nullptr, nullptr, &iid_d3d12_device11, &proxy);
    if (FAILED(hr) || !proxy) {
        std::fprintf(stderr, "D3D12XboxCreateDevice failed: 0x%08lx\n", (unsigned long)hr);
        FreeLibrary(module);
        return 5;
    }

    D3D12_HEAP_PROPERTIES heap_properties = {
        D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
        D3D12_MEMORY_POOL_UNKNOWN, 1, 1
    };
    D3D12_RESOURCE_DESC1 desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 3840;
    desc.Height = 2160;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc = {1, 0};
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = (D3D12_RESOURCE_FLAGS)0x00300005;
    const D3D12_RESOURCE_DESC1 original_desc = desc;
    D3D12_CLEAR_VALUE clear_value = {};
    clear_value.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    void **vtable = *(void ***)proxy;
    create_committed_resource3_xbox_fn create_resource =
        (create_committed_resource3_xbox_fn)vtable[0x3f8 / sizeof(void *)];
    bool passed = true;

    void *resource = nullptr;
    hr = create_resource(proxy, &heap_properties, (D3D12_HEAP_FLAGS)8, &desc,
        D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, &clear_value, nullptr, 0, nullptr,
        &iid_d3d12_resource, &resource);
    if (FAILED(hr) || !resource || std::memcmp(&desc, &original_desc, sizeof(desc)) != 0) {
        std::fprintf(stderr, "exact observed resource creation failed: hr=0x%08lx resource=%p\n",
            (unsigned long)hr, resource);
        passed = false;
    } else {
        std::printf("exact observed resource creation translated: hr=0x%08lx resource=%p\n",
            (unsigned long)hr, resource);
        ((release_fn)(*(void ***)resource)[2])(resource);
        resource = nullptr;
    }

    SetEnvironmentVariableA("COMPAT_D3D12_XBOX_SLOT3F8_COMMITTED_RESOURCE3", "0");
    hr = create_resource(proxy, &heap_properties, (D3D12_HEAP_FLAGS)8, &desc,
        D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, &clear_value, nullptr, 0, nullptr,
        &iid_d3d12_resource, &resource);
    if (SUCCEEDED(hr) || resource) {
        std::fprintf(stderr, "disabled adapter unexpectedly retried: hr=0x%08lx resource=%p\n",
            (unsigned long)hr, resource);
        if (resource) ((release_fn)(*(void ***)resource)[2])(resource);
        passed = false;
    }

    SetEnvironmentVariableA("COMPAT_D3D12_XBOX_SLOT3F8_COMMITTED_RESOURCE3", "1");
    D3D12_RESOURCE_DESC1 near_miss = desc;
    near_miss.Flags = (D3D12_RESOURCE_FLAGS)0x00300004;
    const D3D12_RESOURCE_DESC1 near_miss_original = near_miss;
    hr = create_resource(proxy, &heap_properties, (D3D12_HEAP_FLAGS)8, &near_miss,
        D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, &clear_value, nullptr, 0, nullptr,
        &iid_d3d12_resource, &resource);
    if (SUCCEEDED(hr) || resource || std::memcmp(&near_miss, &near_miss_original, sizeof(near_miss)) != 0) {
        std::fprintf(stderr, "resource descriptor near-miss was incorrectly adapted: hr=0x%08lx resource=%p\n",
            (unsigned long)hr, resource);
        if (resource) ((release_fn)(*(void ***)resource)[2])(resource);
        passed = false;
    }

    ((release_fn)vtable[2])(proxy);
    FreeLibrary(module);
    if (!passed) return 6;
    std::printf("Xbox slot +0x3f8 CreateCommittedResource3 mapping and fail-closed gates: ok\n");
    return 0;
}
