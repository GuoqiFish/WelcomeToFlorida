#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>

using Microsoft::WRL::ComPtr;

static constexpr UINT64 kArenaSize = 0x04000000ull;
static constexpr UINT64 kObservedAllocationSize = 0x01fe0000ull;
static constexpr UINT64 kObservedAllocationAlignment = 0x00010000ull;

static UINT64 align_up(UINT64 value, UINT64 alignment)
{
    if (!alignment || (alignment & (alignment - 1)) || value > UINT64_MAX - (alignment - 1))
        return UINT64_MAX;
    return (value + alignment - 1) & ~(alignment - 1);
}

static int fail(const char *stage, HRESULT hr)
{
    std::fprintf(stderr, "%s failed: HRESULT=0x%08lx\n", stage, (unsigned long)hr);
    return 1;
}

int main()
{
    ComPtr<ID3D12Device> device;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0,
        IID_PPV_ARGS(&device));
    if (FAILED(hr)) return fail("D3D12CreateDevice", hr);

    ComPtr<ID3D12Device10> device10;
    hr = device.As(&device10);
    if (FAILED(hr)) return fail("QueryInterface(ID3D12Device10)", hr);

    D3D12_RESOURCE_DESC1 desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 3840;
    desc.Height = 2160;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc = {1, 0};
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_RESOURCE_DESC legacy_desc = {};
    legacy_desc.Dimension = desc.Dimension;
    legacy_desc.Alignment = desc.Alignment;
    legacy_desc.Width = desc.Width;
    legacy_desc.Height = desc.Height;
    legacy_desc.DepthOrArraySize = desc.DepthOrArraySize;
    legacy_desc.MipLevels = desc.MipLevels;
    legacy_desc.Format = desc.Format;
    legacy_desc.SampleDesc = desc.SampleDesc;
    legacy_desc.Layout = desc.Layout;
    legacy_desc.Flags = desc.Flags;
    const D3D12_RESOURCE_ALLOCATION_INFO allocation =
        device->GetResourceAllocationInfo(1, 1, &legacy_desc);
    if (allocation.SizeInBytes == UINT64_MAX || !allocation.SizeInBytes || !allocation.Alignment)
        return fail("GetResourceAllocationInfo", E_FAIL);

    UINT64 offsets[2] = {};
    UINT64 cursor = 0;
    for (UINT i = 0; i < 2; ++i) {
        offsets[i] = align_up(cursor, allocation.Alignment);
        if (offsets[i] == UINT64_MAX || allocation.SizeInBytes > kArenaSize - offsets[i]) {
            std::fprintf(stderr,
                "two resources do not fit: arena=0x%llx size=0x%llx alignment=0x%llx\n",
                (unsigned long long)kArenaSize,
                (unsigned long long)allocation.SizeInBytes,
                (unsigned long long)allocation.Alignment);
            return 2;
        }
        cursor = offsets[i] + allocation.SizeInBytes;
    }

    std::printf("allocation size=0x%llx alignment=0x%llx; "
        "observed Xbox size/alignment=0x%llx/0x%llx\n",
        (unsigned long long)allocation.SizeInBytes,
        (unsigned long long)allocation.Alignment,
        (unsigned long long)kObservedAllocationSize,
        (unsigned long long)kObservedAllocationAlignment);
    std::printf("64 MiB arena placements: [0x%llx,0x%llx), [0x%llx,0x%llx); remaining=0x%llx\n",
        (unsigned long long)offsets[0],
        (unsigned long long)(offsets[0] + allocation.SizeInBytes),
        (unsigned long long)offsets[1],
        (unsigned long long)(offsets[1] + allocation.SizeInBytes),
        (unsigned long long)(kArenaSize - cursor));

    D3D12_HEAP_DESC heap_desc = {};
    heap_desc.SizeInBytes = kArenaSize;
    heap_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_desc.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_desc.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap_desc.Properties.CreationNodeMask = 1;
    heap_desc.Properties.VisibleNodeMask = 1;
    D3D12_HEAP_DESC display_heap_desc = heap_desc;
    display_heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_DISPLAY;
    ComPtr<ID3D12Heap> display_heap;
    const HRESULT display_heap_result = device->CreateHeap(
        &display_heap_desc, IID_PPV_ARGS(&display_heap));
    std::printf("CreateHeap(ALLOW_DISPLAY) HRESULT=0x%08lx\n",
        (unsigned long)display_heap_result);

    /* ALLOW_DISPLAY is not accepted by CreateHeap on desktop D3D12. Use a
     * plain DEFAULT heap here to isolate shared-backing placement support. */
    heap_desc.Flags = D3D12_HEAP_FLAG_NONE;

    ComPtr<ID3D12Heap> heap;
    hr = device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap));
    if (FAILED(hr)) return fail("CreateHeap(64 MiB DEFAULT)", hr);

    D3D12_CLEAR_VALUE clear_value = {};
    clear_value.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ComPtr<ID3D12Resource> resources[2];
    for (UINT i = 0; i < 2; ++i) {
        hr = device10->CreatePlacedResource2(heap.Get(), offsets[i], &desc,
            D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, &clear_value, 0, nullptr,
            IID_PPV_ARGS(&resources[i]));
        if (FAILED(hr)) {
            char stage[96];
            std::snprintf(stage, sizeof(stage), "CreatePlacedResource2[%u]", i);
            return fail(stage, hr);
        }
        std::printf("placed texture[%u]=%p gpuVA=0x%llx\n", i, resources[i].Get(),
            (unsigned long long)resources[i]->GetGPUVirtualAddress());
    }

    /* Probe whether an ordinary CPU VirtualAlloc arena can serve as genuine
     * backing for the same texture shape through D3D12's diagnostic heap API.
     * This result is reported separately; the API wraps system memory and is
     * not equivalent to an Xbox GPU-local heap. */
    void *cpu_arena = VirtualAlloc(nullptr, (SIZE_T)kArenaSize,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!cpu_arena) {
        std::printf("OpenExistingHeapFromAddress probe skipped: VirtualAlloc error=%lu\n",
            GetLastError());
    } else {
        ComPtr<ID3D12Device3> device3;
        ComPtr<ID3D12Heap> diagnostic_heap;
        ComPtr<ID3D12Resource> diagnostic_resources[2];
        const char *diagnostic_stage = "QueryInterface(ID3D12Device3)";
        HRESULT diagnostic_result = device.As(&device3);
        if (SUCCEEDED(diagnostic_result)) {
            diagnostic_stage = "OpenExistingHeapFromAddress";
            diagnostic_result = device3->OpenExistingHeapFromAddress(
                cpu_arena, IID_PPV_ARGS(&diagnostic_heap));
        }
        if (SUCCEEDED(diagnostic_result)) {
            for (UINT i = 0; i < 2; ++i) {
                diagnostic_stage = i == 0
                    ? "CreatePlacedResource2(system heap)[0]"
                    : "CreatePlacedResource2(system heap)[1]";
                diagnostic_result = device10->CreatePlacedResource2(
                    diagnostic_heap.Get(), offsets[i], &desc,
                    D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, &clear_value, 0, nullptr,
                    IID_PPV_ARGS(&diagnostic_resources[i]));
                if (FAILED(diagnostic_result)) break;
            }
        }
        std::printf("VirtualAlloc-backed diagnostic heap stage=%s HRESULT=0x%08lx\n",
            diagnostic_stage, (unsigned long)diagnostic_result);
        diagnostic_resources[1].Reset();
        diagnostic_resources[0].Reset();
        diagnostic_heap.Reset();
        device3.Reset();
        VirtualFree(cpu_arena, 0, MEM_RELEASE);
    }

    resources[1].Reset();
    resources[0].Reset();
    heap.Reset();
    display_heap.Reset();
    device10.Reset();
    device.Reset();
    std::puts("PASS: both observed 4K textures were backed by one native 64 MiB D3D12 heap.");
    return 0;
}
