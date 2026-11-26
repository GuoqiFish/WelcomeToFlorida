#include <windows.h>
#include <d3d12.h>
#include <stdio.h>
#include <wchar.h>

typedef HRESULT (WINAPI *create_xbox_device_fn)(
    void *, const void *, const GUID *, void **);
typedef HRESULT (STDMETHODCALLTYPE *query_interface_fn)(
    void *, const GUID *, void **);
typedef ULONG (STDMETHODCALLTYPE *release_fn)(void *);
typedef HRESULT (STDMETHODCALLTYPE *create_descriptor_heap_fn)(
    void *, const D3D12_DESCRIPTOR_HEAP_DESC *, const GUID *, void **);
typedef D3D12_DESCRIPTOR_HEAP_DESC *(STDMETHODCALLTYPE *get_desc_xbox_fn)(
    D3D12_DESCRIPTOR_HEAP_DESC *, void *);
typedef HRESULT (STDMETHODCALLTYPE *get_device_fn)(void *, const GUID *, void **);
typedef SIZE_T (STDMETHODCALLTYPE *get_handle_xbox_fn)(void *);

static const GUID iid_d3d12_device11 = {
    0x5405c344, 0xd457, 0x444e, {0xb4,0xdd,0x23,0x66,0xe4,0x5a,0xee,0x39}
};
static const GUID iid_d3d12_descriptor_heap = {
    0x8efb471d, 0x616c, 0x4f49, {0x90,0xf7,0x12,0x7b,0xb7,0x63,0xfa,0x51}
};
static const GUID iid_iunknown = {
    0x00000000, 0x0000, 0x0000, {0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}
};

int wmain(int argc, wchar_t **argv)
{
    enum test_mode { TEST_ABI, TEST_OVERSIZED_RETRY, TEST_OVERSIZED_NEAR_MISS };
    test_mode mode = TEST_ABI;
    HMODULE module = NULL;
    void *device = NULL;
    void *heap = NULL;
    void *unknown = NULL;
    void *heap_device = NULL;
    create_xbox_device_fn create_xbox_device = NULL;
    create_descriptor_heap_fn create_heap = NULL;
    query_interface_fn query_interface = NULL;
    get_device_fn get_device = NULL;
    get_desc_xbox_fn get_desc = NULL;
    get_handle_xbox_fn get_cpu = NULL;
    get_handle_xbox_fn get_gpu = NULL;
    void **device_vtable = NULL;
    void **heap_vtable = NULL;
    D3D12_DESCRIPTOR_HEAP_DESC requested = {};
    D3D12_DESCRIPTOR_HEAP_DESC actual = {};
    D3D12_DESCRIPTOR_HEAP_DESC *desc_result = NULL;
    SIZE_T cpu_handle = 0;
    SIZE_T gpu_handle = 0;
    int result_code = 1;
    HRESULT hr;

    if (argc == 3 && wcscmp(argv[2], L"--oversized-retry") == 0)
        mode = TEST_OVERSIZED_RETRY;
    else if (argc == 3 && wcscmp(argv[2], L"--oversized-near-miss") == 0)
        mode = TEST_OVERSIZED_NEAR_MISS;
    else if (argc != 2) {
        fwprintf(stderr, L"Usage: xbox_descriptor_heap_abi_selftest.exe D3D12_XS_DLL [--oversized-retry|--oversized-near-miss]\n");
        return 2;
    }
    SetEnvironmentVariableA("COMPAT_D3D12_CLAMP_OBSERVED_DESCRIPTOR_HEAP",
        mode == TEST_ABI ? NULL : "1");
    module = LoadLibraryW(argv[1]);
    if (!module) {
        fwprintf(stderr, L"LoadLibrary failed: %lu\n", GetLastError());
        return 3;
    }

    create_xbox_device = (create_xbox_device_fn)
        GetProcAddress(module, "D3D12XboxCreateDevice");
    if (!create_xbox_device) {
        fwprintf(stderr, L"D3D12XboxCreateDevice export is missing\n");
        goto cleanup;
    }
    hr = create_xbox_device(NULL, NULL, &iid_d3d12_device11, &device);
    if (FAILED(hr) || !device) {
        fwprintf(stderr, L"D3D12XboxCreateDevice failed: 0x%08lx\n", (unsigned long)hr);
        goto cleanup;
    }

    requested.Type = mode == TEST_ABI ? D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER :
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    requested.NumDescriptors = mode == TEST_ABI ? 128 :
        (mode == TEST_OVERSIZED_RETRY ? 1245184 : 1245183);
    requested.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    requested.NodeMask = 1;

    device_vtable = *(void ***)device;
    create_heap = (create_descriptor_heap_fn)device_vtable[14];
    hr = create_heap(device, &requested, &iid_d3d12_descriptor_heap, &heap);
    if (mode == TEST_OVERSIZED_NEAR_MISS) {
        if (hr != E_INVALIDARG || heap) {
            fwprintf(stderr, L"near-miss heap request unexpectedly retried or succeeded: HRESULT=0x%08lx heap=%p\n",
                (unsigned long)hr, heap);
            goto cleanup;
        }
        wprintf(L"oversized descriptor-heap near-miss: no retry\n");
        result_code = 0;
        goto cleanup;
    }
    if (FAILED(hr) || !heap) {
        fwprintf(stderr, L"proxy device slot 14 failed: 0x%08lx\n", (unsigned long)hr);
        goto cleanup;
    }

    heap_vtable = *(void ***)heap;
    query_interface = (query_interface_fn)heap_vtable[0];
    hr = query_interface(heap, &iid_iunknown, &unknown);
    if (FAILED(hr) || unknown != heap) {
        fwprintf(stderr, L"descriptor-heap QueryInterface identity failed: 0x%08lx\n",
            (unsigned long)hr);
        goto cleanup;
    }
    ((release_fn)(*(void ***)unknown)[2])(unknown);
    unknown = NULL;

    get_device = (get_device_fn)heap_vtable[7];
    hr = get_device(heap, &iid_d3d12_device11, &heap_device);
    if (FAILED(hr) || heap_device != device) {
        fwprintf(stderr, L"descriptor heap GetDevice proxy identity failed: 0x%08lx\n",
            (unsigned long)hr);
        goto cleanup;
    }
    ((release_fn)(*(void ***)heap_device)[2])(heap_device);
    heap_device = NULL;

    get_desc = (get_desc_xbox_fn)heap_vtable[8];
    desc_result = get_desc(&actual, heap);
    if (desc_result != &actual || actual.Type != requested.Type ||
        actual.NumDescriptors != (mode == TEST_OVERSIZED_RETRY ? 1000000 : requested.NumDescriptors) ||
        actual.Flags != requested.Flags || actual.NodeMask != requested.NodeMask) {
        fwprintf(stderr, L"GetDesc return-buffer order or physical descriptor count failed\n");
        goto cleanup;
    }

    get_cpu = (get_handle_xbox_fn)heap_vtable[9];
    get_gpu = (get_handle_xbox_fn)heap_vtable[10];
    cpu_handle = get_cpu(heap);
    gpu_handle = get_gpu(heap);
    if (!cpu_handle || !gpu_handle) {
        fwprintf(stderr, L"descriptor handle RAX return failed: CPU=%p GPU=%p\n",
            (void *)cpu_handle, (void *)gpu_handle);
        goto cleanup;
    }

    wprintf(L"descriptor heap ABI: slot14=ok GetDesc=ok count=%lu CPU=%p GPU=%p\n",
        actual.NumDescriptors,
        (void *)cpu_handle, (void *)gpu_handle);
    result_code = 0;

cleanup:
    if (heap_device) ((release_fn)(*(void ***)heap_device)[2])(heap_device);
    if (unknown) ((release_fn)(*(void ***)unknown)[2])(unknown);
    if (heap) {
        ULONG remaining = ((release_fn)(*(void ***)heap)[2])(heap);
        if (remaining != 0) {
            fwprintf(stderr, L"descriptor heap proxy release count=%lu\n", remaining);
            result_code = 4;
        }
    }
    if (device) {
        ULONG remaining = ((release_fn)(*(void ***)device)[2])(device);
        if (remaining != 0) {
            fwprintf(stderr, L"device proxy release count=%lu\n", remaining);
            result_code = 5;
        }
    }
    if (module) FreeLibrary(module);
    return result_code;
}
