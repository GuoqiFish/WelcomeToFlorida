#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

using PFN_VIRTUAL_ALLOC2 = PVOID (WINAPI *)(HANDLE, PVOID, SIZE_T, ULONG,
    ULONG, PMEM_EXTENDED_PARAMETER, ULONG);

static void *allocate_region(bool placeholder_topology, bool unified_reservation)
{
    constexpr SIZE_T region_size = 0x0b900000u;
    if (!placeholder_topology) {
        return VirtualAlloc(nullptr, region_size,
            unified_reservation ? MEM_RESERVE : (MEM_RESERVE | MEM_COMMIT),
            unified_reservation ? PAGE_NOACCESS : PAGE_READWRITE);
    }

    HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
    auto virtual_alloc2 = kernelbase
        ? reinterpret_cast<PFN_VIRTUAL_ALLOC2>(GetProcAddress(kernelbase, "VirtualAlloc2"))
        : nullptr;
    if (!virtual_alloc2) return nullptr;
    void *region = virtual_alloc2(GetCurrentProcess(), nullptr, region_size,
        MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
    if (!region) return nullptr;

    constexpr SIZE_T segments[] = {
        0x00a00000u, 0x00100000u, 0x01b00000u, 0x02f00000u, 0x06400000u
    };
    SIZE_T offset = 0;
    for (SIZE_T segment_size : segments) {
        void *segment = static_cast<unsigned char *>(region) + offset;
        if (offset + segment_size < 0x0b900000u) {
            if (!VirtualFree(segment, segment_size,
                    MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
                std::printf("VirtualFree split offset=0x%llx size=0x%llx failed error=%lu\n",
                    static_cast<unsigned long long>(offset),
                    static_cast<unsigned long long>(segment_size), GetLastError());
                return nullptr;
            }
        }
        void *replaced = virtual_alloc2(GetCurrentProcess(), segment, segment_size,
                MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
                nullptr, 0);
        if (replaced != segment) {
            std::printf("VirtualAlloc2 replace offset=0x%llx size=0x%llx result=%p error=%lu\n",
                static_cast<unsigned long long>(offset),
                static_cast<unsigned long long>(segment_size), replaced, GetLastError());
            return nullptr;
        }
        if (!VirtualAlloc(segment, segment_size, MEM_COMMIT, PAGE_READWRITE)) {
            std::printf("VirtualAlloc commit offset=0x%llx size=0x%llx failed error=%lu\n",
                static_cast<unsigned long long>(offset),
                static_cast<unsigned long long>(segment_size), GetLastError());
            return nullptr;
        }
        offset += segment_size;
    }
    return region;
}

static int report_failure(const char *stage, HRESULT result)
{
    std::printf("%s HRESULT=0x%08lx\n", stage, static_cast<unsigned long>(result));
    return 1;
}

static void dump_info_queue(ID3D12Device *device, const char *stage)
{
    ComPtr<ID3D12InfoQueue> queue;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&queue)))) return;
    const UINT64 count = queue->GetNumStoredMessages();
    for (UINT64 i = 0; i < count; ++i) {
        SIZE_T size = 0;
        if (FAILED(queue->GetMessage(i, nullptr, &size)) || !size) continue;
        auto *message = static_cast<D3D12_MESSAGE *>(HeapAlloc(GetProcessHeap(), 0, size));
        if (!message) continue;
        if (SUCCEEDED(queue->GetMessage(i, message, &size)))
            std::printf("D3D12_INFO stage=%s severity=%u id=%u text=%s\n",
                stage, static_cast<unsigned>(message->Severity),
                static_cast<unsigned>(message->ID), message->pDescription);
        HeapFree(GetProcessHeap(), 0, message);
    }
    queue->ClearStoredMessages();
}

int main(int argc, char **argv)
{
    const bool placeholder_topology = argc > 1 && std::strcmp(argv[1], "topology") == 0;
    const bool unified_reservation = argc > 1 && std::strcmp(argv[1], "unified") == 0;
    constexpr SIZE_T resource_offset = 0x00b00000u;
    constexpr SIZE_T resource_size = 0x0ae00000u;
    void *region = allocate_region(placeholder_topology, unified_reservation);
    if (!region) {
        std::printf("allocate_region(%s) failed error=%lu\n",
            placeholder_topology ? "topology" :
                (unified_reservation ? "unified" : "committed"), GetLastError());
        return 1;
    }
    std::printf("mode=%s region=%p\n",
        placeholder_topology ? "placeholder-topology" :
            (unified_reservation ? "single-reservation-segment-commits" :
                "single-committed-region"), region);

    if (unified_reservation) {
        constexpr SIZE_T segments[] = {
            0x00a00000u, 0x00100000u, 0x01b00000u, 0x02f00000u, 0x06400000u
        };
        SIZE_T offset = 0;
        for (SIZE_T segment_size : segments) {
            void *segment = static_cast<unsigned char *>(region) + offset;
            if (!VirtualAlloc(segment, segment_size, MEM_COMMIT, PAGE_READWRITE)) {
                std::printf("unified commit offset=0x%llx failed error=%lu\n",
                    static_cast<unsigned long long>(offset), GetLastError());
                VirtualFree(region, 0, MEM_RELEASE);
                return 1;
            }
            offset += segment_size;
        }
    }

    ComPtr<ID3D12Debug> debug;
    const HRESULT debug_result = D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
    if (SUCCEEDED(debug_result)) debug->EnableDebugLayer();
    std::printf("D3D12 debug layer HRESULT=0x%08lx\n",
        static_cast<unsigned long>(debug_result));

    ComPtr<ID3D12Device> device;
    HRESULT result = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
        IID_PPV_ARGS(&device));
    if (FAILED(result)) {
        VirtualFree(region, 0, MEM_RELEASE);
        return report_failure("D3D12CreateDevice", result);
    }

    ComPtr<ID3D12Device3> device3;
    result = device.As(&device3);
    if (FAILED(result)) {
        device.Reset();
        VirtualFree(region, 0, MEM_RELEASE);
        return report_failure("QueryInterface(ID3D12Device3)", result);
    }

    ComPtr<ID3D12Heap> heap;
    void *resource_address = static_cast<unsigned char *>(region) + resource_offset;
    result = device3->OpenExistingHeapFromAddress(region, IID_PPV_ARGS(&heap));
    if (FAILED(result)) {
        device3.Reset();
        device.Reset();
        VirtualFree(region, 0, MEM_RELEASE);
        return report_failure("OpenExistingHeapFromAddress", result);
    }

    const D3D12_HEAP_DESC heap_desc = heap->GetDesc();
    std::printf("OpenExistingHeapFromAddress=OK base=%p size=0x%llx type=%u cpu_page=%u pool=%u flags=0x%x\n",
        region, static_cast<unsigned long long>(heap_desc.SizeInBytes),
        static_cast<unsigned>(heap_desc.Properties.Type),
        static_cast<unsigned>(heap_desc.Properties.CPUPageProperty),
        static_cast<unsigned>(heap_desc.Properties.MemoryPoolPreference),
        static_cast<unsigned>(heap_desc.Flags));

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = resource_size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
        D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;

    const D3D12_RESOURCE_ALLOCATION_INFO allocation =
        device->GetResourceAllocationInfo(0, 1, &desc);
    std::printf("buffer allocation size=0x%llx alignment=0x%llx\n",
        static_cast<unsigned long long>(allocation.SizeInBytes),
        static_cast<unsigned long long>(allocation.Alignment));

    ComPtr<ID3D12Resource> resource;
    result = device->CreatePlacedResource(heap.Get(), resource_offset, &desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource));
    if (FAILED(result)) {
        dump_info_queue(device.Get(), "common");
        heap.Reset();
        device3.Reset();
        device.Reset();
        if (placeholder_topology) {
            std::printf("exact placeholder topology cannot place the 174 MiB resource\n");
            constexpr SIZE_T segments[] = {
                0x00a00000u, 0x00100000u, 0x01b00000u, 0x02f00000u, 0x06400000u
            };
            SIZE_T offset = 0;
            for (SIZE_T segment_size : segments) {
                VirtualFree(static_cast<unsigned char *>(region) + offset, 0, MEM_RELEASE);
                offset += segment_size;
            }
            return 3;
        }
        VirtualFree(region, 0, MEM_RELEASE);
        return report_failure("CreatePlacedResource(buffer)", result);
    }

    D3D12_RANGE read_range = {0, 0};
    void *mapped = nullptr;
    result = resource->Map(0, &read_range, &mapped);
    if (FAILED(result)) {
        resource.Reset();
        heap.Reset();
        device3.Reset();
        device.Reset();
        if (placeholder_topology) {
            constexpr SIZE_T segments[] = {
                0x00a00000u, 0x00100000u, 0x01b00000u, 0x02f00000u, 0x06400000u
            };
            SIZE_T offset = 0;
            for (SIZE_T segment_size : segments) {
                VirtualFree(static_cast<unsigned char *>(region) + offset, 0, MEM_RELEASE);
                offset += segment_size;
            }
        } else VirtualFree(region, 0, MEM_RELEASE);
        return report_failure("ID3D12Resource::Map", result);
    }

    constexpr SIZE_T sentinel_offset = 0x200;
    const unsigned char sentinel = 0x5a;
    static_cast<unsigned char *>(mapped)[sentinel_offset] = sentinel;
    const unsigned char observed = static_cast<unsigned char *>(resource_address)[sentinel_offset];
    resource->Unmap(0, nullptr);

    const D3D12_GPU_VIRTUAL_ADDRESS gpu_address = resource->GetGPUVirtualAddress();
    std::printf("placed_buffer=OK cpu_map=%p expected_map=%p alias=%s gpu_va=0x%llx\n",
        mapped, resource_address,
        observed == sentinel ? "yes" : "no",
        static_cast<unsigned long long>(gpu_address));

    resource.Reset();
    heap.Reset();
    device3.Reset();
    device.Reset();
    if (placeholder_topology) {
        constexpr SIZE_T segments[] = {
            0x00a00000u, 0x00100000u, 0x01b00000u, 0x02f00000u, 0x06400000u
        };
        SIZE_T offset = 0;
        for (SIZE_T segment_size : segments) {
            VirtualFree(static_cast<unsigned char *>(region) + offset, 0, MEM_RELEASE);
            offset += segment_size;
        }
    } else VirtualFree(region, 0, MEM_RELEASE);
    return observed == sentinel ? 0 : 2;
}
