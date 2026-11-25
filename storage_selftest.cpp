#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include "xbox_storage_abi.h"

// Integration test of the Xbox-facing COM boundary. The input is opened read
// only; successful tests compare real DirectStorage transfers with ReadFile.
struct TestFailure {};
static void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s (Win32=%lu)\n", message, GetLastError());
        throw TestFailure{};
    }
}
static void require_hr(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        std::fprintf(stderr, "FAIL: %s HRESULT=0x%08lx\n", operation, result);
        throw TestFailure{};
    }
}
template<class T> struct ComOwner {
    T* value = nullptr;
    ~ComOwner() { if (value) value->Release(); }
    T* operator->() const { return value; }
    ComOwner() = default;
    ComOwner(const ComOwner&) = delete;
    ComOwner& operator=(const ComOwner&) = delete;
};
struct QueueOwner : ComOwner<XStorageQueue> {
    ~QueueOwner() { if (value) value->Close(); }
};
struct FileOwner : ComOwner<XStorageFile> {
    ~FileOwner() { if (value) value->Close(); }
};
struct HandleOwner {
    HANDLE value;
    explicit HandleOwner(HANDLE handle) : value(handle) {}
    ~HandleOwner() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    HandleOwner(const HandleOwner&) = delete;
    HandleOwner& operator=(const HandleOwner&) = delete;
};
struct BufferOwner {
    BYTE* value;
    explicit BufferOwner(SIZE_T bytes) : value(static_cast<BYTE*>(VirtualAlloc(
        nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))) {
        require(value != nullptr, "VirtualAlloc test buffer");
    }
    ~BufferOwner() { VirtualFree(value, 0, MEM_RELEASE); }
    BufferOwner(const BufferOwner&) = delete;
    BufferOwner& operator=(const BufferOwner&) = delete;
};

static constexpr UINT32 kBytes = 4096;

static void test_transfer(XStorageFactory* factory, const wchar_t* path,
                          const BYTE* expected, bool memory_source) {
    const char* name = memory_source ? "selftest.memory" : "selftest.file";
    BufferOwner source(kBytes);
    BufferOwner destination(kBytes);
    std::memcpy(source.value, expected, kBytes);
    std::memset(destination.value, 0xcc, kBytes);
    BYTE sentinel[kBytes];
    std::memset(sentinel, 0xcc, sizeof(sentinel));

    XStorageQueueDesc1 descriptor{};
    descriptor.SourceType = memory_source ? XStorageSourceMemory : XStorageSourceFile;
    descriptor.Capacity = 128;
    descriptor.Priority = memory_source ? XStoragePriorityRealtime : XStoragePriorityNormal;
    descriptor.Name = name;
    descriptor.Options.AutoSubmitDisabled = 1;
    QueueOwner queue;
    require_hr(factory->CreateQueue1(&descriptor, X_IID_StorageQueue,
        reinterpret_cast<void**>(&queue.value)), "CreateQueue1");
    require(queue.value != nullptr, "CreateQueue1 non-null output");

    ComOwner<XStorageStatusArray> status;
    require_hr(factory->CreateStatusArray(1, name, X_IID_StorageStatusArray,
        reinterpret_cast<void**>(&status.value)), "CreateStatusArray");
    require(status.value != nullptr, "CreateStatusArray non-null output");
    HandleOwner event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    require(event.value != nullptr, "CreateEvent");

    XStorageQueueInfo initial{};
    queue->Query(&initial);
    require(initial.Desc.Capacity == 128 && initial.Desc.SourceType == descriptor.SourceType,
        "Query returns Xbox baseline descriptor");
    require(initial.EmptySlotCount == 127, "Query reports empty capacity minus reserved slot");

    XStorageRequest request{};
    ComOwner<XStorageFile> request_file;
    if (!memory_source) {
        require_hr(factory->OpenFile(path, X_IID_StorageFile,
            reinterpret_cast<void**>(&request_file.value)), "open independently retained request file");
    }
    request.Options.SourceType = memory_source ? 1 : 0;
    request.Destination = destination.value;
    request.DestinationSize = kBytes;
    request.SourceSize = kBytes;
    request.CancellationTag = memory_source ? 0x4d454d : 0x46494c45;
    request.Name = name;
    if (memory_source) request.Source = source.value;
    else { request.File = request_file.value; request.FileOffset = 0; }

    queue->EnqueueRequest(&request);
    // The queue must retain its own reference while this command is buffered.
    // Release the caller reference without Close(), which closes regardless
    // of outstanding references by design.
    if (request_file.value) { request_file.value->Release(); request_file.value = nullptr; }
    queue->EnqueueStatus(status.value, 0);
    queue->EnqueueSetEvent(event.value);
    // Leave enough time for an accidental early submission to become visible.
    Sleep(40);
    require(status->GetHResult(0) == E_PENDING, "status is E_PENDING before Submit");
    require(!status->IsComplete(0), "IsComplete is false before Submit");
    require(WaitForSingleObject(event.value, 0) == WAIT_TIMEOUT,
        "completion event stays unsignaled before Submit");
    require(std::memcmp(destination.value, sentinel, kBytes) == 0,
        "destination remains unchanged before Submit");

    XStorageQueueInfo1 queued{};
    queue->Query1(&queued);
    require(queued.Desc.Capacity == descriptor.Capacity &&
        queued.Desc.SourceType == descriptor.SourceType &&
        queued.Desc.Options.AutoSubmitDisabled == 1 && queued.Desc.Name &&
        std::strcmp(queued.Desc.Name, name) == 0, "Query1 preserves extended Xbox descriptor");
    require(queued.EmptySlotCount == 124, "Query1 accounts for three buffered commands");

    queue->Submit();
    DWORD waited = WaitForSingleObject(event.value, 10000);
    require(waited == WAIT_OBJECT_0, "real queue completion event within 10 seconds");
    require_hr(status->GetHResult(0), "completed transfer status");
    require(status->GetHResult(0) == S_OK && status->IsComplete(0),
        "completed transfer reports S_OK and IsComplete");
    require(std::memcmp(destination.value, expected, kBytes) == 0,
        "DirectStorage destination matches Win32 ReadFile bytes");
    require(WaitForSingleObject(queue->GetErrorEvent(), 0) == WAIT_TIMEOUT,
        "successful transfer has no error event");
    std::printf("PASS %s: %u bytes match ReadFile; Submit gating, status, event and Query1 verified\n",
        name, kBytes);
}

static void test_close_before_submit(XStorageFactory* factory) {
    BufferOwner source(kBytes);
    BufferOwner destination(kBytes);
    std::memset(source.value, 0x27, kBytes);
    std::memset(destination.value, 0xc3, kBytes);
    BYTE sentinel[kBytes];
    std::memset(sentinel, 0xc3, sizeof(sentinel));
    XStorageQueueDesc1 descriptor{};
    descriptor.SourceType = XStorageSourceMemory;
    descriptor.Capacity = 128;
    descriptor.Priority = XStoragePriorityRealtime;
    descriptor.Name = "selftest.close-before-submit";
    descriptor.Options.AutoSubmitDisabled = 1;
    QueueOwner queue;
    require_hr(factory->CreateQueue1(&descriptor, X_IID_StorageQueue,
        reinterpret_cast<void**>(&queue.value)), "CreateQueue1 close-before-submit");
    ComOwner<XStorageStatusArray> status;
    require_hr(factory->CreateStatusArray(1, descriptor.Name, X_IID_StorageStatusArray,
        reinterpret_cast<void**>(&status.value)), "CreateStatusArray close-before-submit");
    XStorageRequest request{};
    request.Options.SourceType = 1;
    request.Source = source.value;
    request.SourceSize = kBytes;
    request.Destination = destination.value;
    request.DestinationSize = kBytes;
    request.Name = descriptor.Name;
    queue->EnqueueRequest(&request);
    queue->EnqueueStatus(status.value, 0);
    require(status->GetHResult(0) == E_PENDING, "buffered abandoned status is initially pending");
    queue->Close();
    require(status->GetHResult(0) == E_ABORT && status->IsComplete(0),
        "Close-before-Submit reports completed E_ABORT");
    require(std::memcmp(destination.value, sentinel, kBytes) == 0,
        "Close-before-Submit leaves destination unchanged");
    std::printf("PASS Close-before-Submit: E_ABORT status and unchanged destination\n");
}

static void run_tests(HMODULE adapter, const wchar_t* path) {
    using GetFactory = HRESULT(WINAPI*)(REFIID, void**);
    auto get_factory = reinterpret_cast<GetFactory>(GetProcAddress(adapter, "DStorageGetFactory"));
    require(get_factory != nullptr, "adapter exports DStorageGetFactory");

    ComOwner<XStorageFactory> factory;
    require_hr(get_factory(X_IID_StorageFactory, reinterpret_cast<void**>(&factory.value)),
        "Xbox DStorageGetFactory");
    require(factory.value != nullptr, "Xbox factory non-null output");
    require_hr(factory->SetStagingBufferSize(32 * 1024 * 1024), "SetStagingBufferSize");

    HandleOwner reference(CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    require(reference.value != INVALID_HANDLE_VALUE, "open read-only reference file");
    BYTE expected[kBytes];
    DWORD got = 0;
    require(ReadFile(reference.value, expected, sizeof(expected), &got, nullptr) && got == kBytes,
        "reference input contains at least 4096 bytes");

    FileOwner file;
    require_hr(factory->OpenFile(path, X_IID_StorageFile, reinterpret_cast<void**>(&file.value)),
        "Xbox OpenFile");
    require(file.value != nullptr, "Xbox file non-null output");
    {
        HandleOwner owned(file->GetHandle());
        require(owned.value != INVALID_HANDLE_VALUE && owned.value != nullptr,
            "Xbox GetHandle returns owned Win32 handle");
        BYTE handle_bytes[16]; got = 0;
        require(ReadFile(owned.value, handle_bytes, sizeof(handle_bytes), &got, nullptr) &&
            got == sizeof(handle_bytes) && std::memcmp(handle_bytes, expected, sizeof(handle_bytes)) == 0,
            "owned Xbox file handle reads reference bytes");
    }
    std::printf("PASS File.GetHandle: ReadFile matched and owned handle closed\n");

    // This checks a genuine native failure path without manufacturing an error
    // in the adapter or depending on its as-yet unsupported error-record ABI.
    std::wstring missing = path;
    missing += L".storage-selftest-missing-" + std::to_wstring(GetCurrentProcessId());
    ComOwner<XStorageFile> absent;
    HRESULT missing_hr = factory->OpenFile(missing.c_str(), X_IID_StorageFile,
        reinterpret_cast<void**>(&absent.value));
    require(FAILED(missing_hr) && absent.value == nullptr, "missing input fails with null file output");
    std::printf("PASS missing file returns native failure HRESULT=0x%08lx\n", missing_hr);

    test_transfer(factory.value, path, expected, false);
    test_transfer(factory.value, path, expected, true);
    test_close_before_submit(factory.value);
}

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc != 3) {
        std::fprintf(stderr, "Usage: storage_selftest.exe ABSOLUTE_ADAPTER_DLL ABSOLUTE_INPUT_FILE\n");
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("Testing Xbox-facing adapter: %ls\nRead-only reference: %ls\n", argv[1], argv[2]);
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(co)) {
        std::fprintf(stderr, "FAIL: CoInitializeEx HRESULT=0x%08lx\n", co);
        return 2;
    }
    HMODULE adapter = LoadLibraryExW(argv[1], nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    int result = 0;
    try {
        require(adapter != nullptr, "LoadLibraryEx adapter with local dependencies");
        run_tests(adapter, argv[2]);
        std::printf("ALL STORAGE SELF-TESTS PASSED\n");
    } catch (const TestFailure&) { result = 1; }
    if (adapter) FreeLibrary(adapter);
    CoUninitialize();
    return result;
}
