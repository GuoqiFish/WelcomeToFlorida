#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <dstorage.h>
#include <new>
#include <vector>
#include <mutex>
#include <string>
#include <cstdio>
#include <cstdarg>
#include <algorithm>
#include "xbox_storage_abi.h"

// This adapter implements the verified Xbox COM boundary, not an Xbox runtime.
// Only uncompressed virtual-memory destinations are supported. Unsupported
// operations stop with a diagnostic exception rather than reporting success.
static void trace(const char* format, ...) {
    char message[2048];
    int prefix = _snprintf_s(message, sizeof(message), _TRUNCATE,
        "[storage tid=%lu] ", GetCurrentThreadId());
    va_list args; va_start(args, format);
    _vsnprintf_s(message + prefix, sizeof(message) - prefix, _TRUNCATE, format, args);
    va_end(args);
    size_t n = strlen(message);
    if (n < sizeof(message) - 2) { message[n++] = '\n'; message[n] = 0; }
    OutputDebugStringA(message);
    wchar_t path[32768];
    DWORD count = GetEnvironmentVariableW(L"COMPAT_STORAGE_LOG", path, ARRAYSIZE(path));
    if (!count || count >= ARRAYSIZE(path)) return;
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                          nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written; WriteFile(f, message, (DWORD)n, &written, nullptr); CloseHandle(f);
}
[[noreturn]] static void unsupported(const char* what, UINT64 value = 0) {
    trace("UNSUPPORTED %s value=0x%llx", what, value);
    ULONG_PTR args[] = {(ULONG_PTR)what, (ULONG_PTR)value};
    RaiseException(0xE0535447, EXCEPTION_NONCONTINUABLE, 2, args);
    TerminateProcess(GetCurrentProcess(), 0xE0535447);
    __assume(0);
}
static void trace_iid(const char* what, REFIID iid) {
    trace("%s IID=%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", what,
        iid.Data1, iid.Data2, iid.Data3, iid.Data4[0], iid.Data4[1],
        iid.Data4[2], iid.Data4[3], iid.Data4[4], iid.Data4[5], iid.Data4[6], iid.Data4[7]);
}

class FileAdapter;
class StatusAdapter;
class QueueAdapter;

class FileAdapter final : public XStorageFile {
    volatile LONG refs_ = 1;
    std::wstring path_;
public:
    IDStorageFile* native;
    FileAdapter(IDStorageFile* f, const wchar_t* p) : path_(p), native(f) {}
    ~FileAdapter() { native->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER; *out = nullptr;
        if (iid != IID_IUnknown && iid != X_IID_StorageFile) { trace_iid("File.QI rejected", iid); return E_NOINTERFACE; }
        *out = static_cast<XStorageFile*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = InterlockedDecrement(&refs_); if (!r) delete this; return r;
    }
    void STDMETHODCALLTYPE Close() override { trace("File.Close %ls", path_.c_str()); native->Close(); }
    HANDLE STDMETHODCALLTYPE GetHandle() override {
        HANDLE h = CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        trace("File.GetHandle path=%ls handle=%p error=%lu", path_.c_str(), h,
              h == INVALID_HANDLE_VALUE ? GetLastError() : 0);
        return h;
    }
};

class StatusAdapter final : public XStorageStatusArray {
    volatile LONG refs_ = 1;
    // 0: native status owns result; 1: buffered; 2: aborted; 3: never enqueued.
    std::vector<LONG> buffered_;
public:
    IDStorageStatusArray* native;
    StatusAdapter(IDStorageStatusArray* p, UINT32 capacity) : buffered_(capacity, 3), native(p) {}
    ~StatusAdapter() { native->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER; *out = nullptr;
        if (iid != IID_IUnknown && iid != X_IID_StorageStatusArray) { trace_iid("Status.QI rejected", iid); return E_NOINTERFACE; }
        *out = static_cast<XStorageStatusArray*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = InterlockedDecrement(&refs_); if (!r) delete this; return r;
    }
    bool STDMETHODCALLTYPE IsComplete(UINT32 i) override { return GetHResult(i) != E_PENDING; }
    HRESULT STDMETHODCALLTYPE GetHResult(UINT32 i) override {
        if (i >= buffered_.size()) return E_INVALIDARG;
        LONG local = InterlockedCompareExchange(&buffered_[i], 0, 0);
        if (local == 1) return E_PENDING;
        if (local == 2) return E_ABORT;
        return native->GetHResult(i);
    }
    void mark_buffered(UINT32 i) {
        if (i >= buffered_.size()) unsupported("status index", i);
        for (;;) {
            LONG previous = InterlockedCompareExchange(&buffered_[i], 0, 0);
            if (previous == 1 || (previous == 0 && native->GetHResult(i) == E_PENDING))
                unsupported("reuse pending status", i);
            if (InterlockedCompareExchange(&buffered_[i], 1, previous) == previous) break;
        }
    }
    void replayed(UINT32 i) { InterlockedExchange(&buffered_[i], 0); }
    void aborted(UINT32 i) { InterlockedExchange(&buffered_[i], 2); }
};

// Interface type checks keep foreign native COM objects out of translation.
static FileAdapter* checked_file(XStorageFile* f) {
    if (!f) unsupported("null source file");
    return static_cast<FileAdapter*>(f);
}
static StatusAdapter* checked_status(XStorageStatusArray* s) {
    if (!s) unsupported("null status array");
    return static_cast<StatusAdapter*>(s);
}

class QueueAdapter final : public XStorageQueue {
    volatile LONG refs_ = 1;
    IDStorageQueue1* native_;
    XStorageQueueDesc1 desc_;
    std::string name_;
    std::mutex mutex_;
    // DirectStorage can keep referencing these objects after Submit returns.
    // Hold one queue-owned reference per object until Close has drained it.
    std::vector<FileAdapter*> retained_files_;
    std::vector<StatusAdapter*> retained_status_;
    UINT64 request_count_ = 0;
    bool closed_ = false;
    enum class Kind { Request, Status, Signal, Event };
    struct Command {
        Kind kind;
        DSTORAGE_REQUEST request{};
        FileAdapter* file = nullptr;
        StatusAdapter* status = nullptr;
        UINT32 index = 0;
        ID3D12Fence* fence = nullptr;
        UINT64 value = 0;
        HANDLE event = nullptr;
    };
    std::vector<Command> pending_;
    bool buffered() const { return desc_.Options.AutoSubmitDisabled != 0; }
    void ready() { if (closed_) unsupported("closed queue"); }
    void retain_file(FileAdapter* file) {
        if (file && std::find(retained_files_.begin(), retained_files_.end(), file) == retained_files_.end()) {
            file->AddRef();
            retained_files_.push_back(file);
        }
    }
    void retain_status(StatusAdapter* status) {
        if (std::find(retained_status_.begin(), retained_status_.end(), status) == retained_status_.end()) {
            status->AddRef();
            retained_status_.push_back(status);
        }
    }
    void append(Command& c) {
        ready();
        if (pending_.size() >= (size_t)desc_.Capacity - 1)
            unsupported("autosubmit-disabled queue capacity exhausted", pending_.size());
        pending_.push_back(c);
    }
    static void release(Command& c) {
        if (c.file) c.file->Release();
        if (c.status) c.status->Release();
        if (c.fence) c.fence->Release();
    }
public:
    QueueAdapter(IDStorageQueue1* q, const XStorageQueueDesc1& d) : native_(q), desc_(d), name_(d.Name ? d.Name : "") {
        desc_.Name = name_.c_str();
    }
    ~QueueAdapter() { Close(); native_->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER; *out = nullptr;
        if (iid != IID_IUnknown && iid != X_IID_StorageQueue) { trace_iid("Queue.QI rejected", iid); return E_NOINTERFACE; }
        *out = static_cast<XStorageQueue*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = InterlockedDecrement(&refs_); if (!r) delete this; return r;
    }
    void STDMETHODCALLTYPE EnqueueRequest(const XStorageRequest* x) override {
        if (!x) unsupported("null request");
        // Bits: zlib0, BCPack1..4, swizzle5..20, destination-physical21,
        // source-kind22, source-physical23, reserved24..63.
        if (x->Options.AsUINT64 & ~(1ull << 22)) unsupported("request transform/physical/reserved flags", x->Options.AsUINT64);
        if (x->SourceSize != x->DestinationSize) unsupported("uncompressed size mismatch", x->SourceSize);
        DSTORAGE_REQUEST p{};
        bool memory_source = x->Options.SourceType != 0;
        p.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
        p.Options.SourceType = memory_source ? DSTORAGE_REQUEST_SOURCE_MEMORY : DSTORAGE_REQUEST_SOURCE_FILE;
        p.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
        p.Destination.Memory.Buffer = x->Destination;
        p.Destination.Memory.Size = x->DestinationSize;
        p.CancellationTag = x->CancellationTag; p.Name = x->Name;
        FileAdapter* f = nullptr;
        if (memory_source) {
            p.Source.Memory.Source = x->Source;
            p.Source.Memory.Size = x->SourceSize;
        } else {
            f = checked_file(x->File);
            p.Source.File.Source = f->native;
            p.Source.File.Offset = x->FileOffset;
            p.Source.File.Size = x->SourceSize;
        }
        std::lock_guard<std::mutex> lock(mutex_); ready();
        ++request_count_;
        if (request_count_ <= 32 || (request_count_ & 1023) == 0)
            trace("Queue.EnqueueRequest name=%s count=%llu options=0x%llx source=%p offset=0x%llx dest=%p bytes=%u",
                name_.c_str(), request_count_, x->Options.AsUINT64, memory_source ? x->Source : f,
                memory_source ? 0 : x->FileOffset, p.Destination.Memory.Buffer, x->SourceSize);
        if (!buffered()) {
            retain_file(f);
            native_->EnqueueRequest(&p);
            return;
        }
        Command c{}; c.kind = Kind::Request; c.request = p; c.file = f;
        retain_file(f);
        if (f) f->AddRef(); append(c);
    }
    void STDMETHODCALLTYPE EnqueueStatus(XStorageStatusArray* s, UINT32 i) override {
        auto a = checked_status(s);
        std::lock_guard<std::mutex> lock(mutex_); ready();
        a->mark_buffered(i);
        retain_status(a);
        if (!buffered()) { native_->EnqueueStatus(a->native, i); a->replayed(i); return; }
        Command c{}; c.kind = Kind::Status; c.status = a; c.index = i; a->AddRef(); append(c);
    }
    void STDMETHODCALLTYPE EnqueueSignal(ID3D12Fence* f, UINT64 value) override {
        // Xbox graphics interfaces require a separate adapter; never pass one
        // to PC DirectStorage before its ABI has been established.
        (void)f; unsupported("Xbox fence signal", value);
    }
    void STDMETHODCALLTYPE Submit() override {
        std::lock_guard<std::mutex> lock(mutex_); ready();
        trace("Queue.Submit name=%s pending=%zu total_requests=%llu", name_.c_str(), pending_.size(), request_count_);
        for (auto& c : pending_) {
            switch (c.kind) {
            case Kind::Request: native_->EnqueueRequest(&c.request); break;
            case Kind::Status: native_->EnqueueStatus(c.status->native, c.index); c.status->replayed(c.index); break;
            case Kind::Event: native_->EnqueueSetEvent(c.event); break;
            default: unsupported("buffered command kind");
            }
            release(c);
        }
        pending_.clear(); native_->Submit();
    }
    void STDMETHODCALLTYPE CancelRequestsWithTag(UINT64 mask, UINT64 value) override {
        std::lock_guard<std::mutex> lock(mutex_); ready();
        auto i = pending_.begin();
        while (i != pending_.end()) {
            if (i->kind == Kind::Request && (i->request.CancellationTag & mask) == value) {
                release(*i); i = pending_.erase(i);
            } else ++i;
        }
        native_->CancelRequestsWithTag(mask, value);
    }
    void STDMETHODCALLTYPE Close() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return; closed_ = true;
        for (auto& c : pending_) { if (c.status) c.status->aborted(c.index); release(c); }
        pending_.clear(); native_->Close();
        for (auto* file : retained_files_) file->Release();
        for (auto* status : retained_status_) status->Release();
        retained_files_.clear(); retained_status_.clear();
        trace("Queue.Close name=%s", name_.c_str());
    }
    HANDLE STDMETHODCALLTYPE GetErrorEvent() override { return native_->GetErrorEvent(); }
    void STDMETHODCALLTYPE RetrieveErrorRecord(void* record) override {
        (void)record;
        DSTORAGE_ERROR_RECORD native_record{}; native_->RetrieveErrorRecord(&native_record);
        trace("Queue.RetrieveErrorRecord failures=%u HRESULT=0x%08lx command=%d", native_record.FailureCount,
            native_record.FirstFailure.HResult, native_record.FirstFailure.CommandType);
        unsupported("Xbox error record translation");
    }
    void STDMETHODCALLTYPE Query(XStorageQueueInfo* output) override {
        if (!output) unsupported("null Queue.Query output");
        DSTORAGE_QUEUE_INFO info{}; native_->Query(&info);
        // Baseline descriptor is 24 bytes, followed by two UINT16 counters.
        memcpy(output, &desc_, 24);
        UINT16 counts[2] = {info.EmptySlotCount, info.RequestCountUntilAutoSubmit};
        if (buffered()) {
            std::lock_guard<std::mutex> lock(mutex_);
            counts[0] = (UINT16)(desc_.Capacity - 1 - pending_.size()); counts[1] = 0;
        }
        memcpy((BYTE*)output + 24, counts, sizeof(counts));
    }
    void STDMETHODCALLTYPE EnqueueSetEvent(HANDLE event) override {
        std::lock_guard<std::mutex> lock(mutex_); ready();
        if (!buffered()) { native_->EnqueueSetEvent(event); return; }
        Command c{}; c.kind = Kind::Event; c.event = event; append(c);
    }
    void STDMETHODCALLTYPE Query1(XStorageQueueInfo1* output) override {
        if (!output) unsupported("null Queue.Query1 output");
        XStorageQueueInfo baseline{}; Query(&baseline);
        memcpy(output, &desc_, sizeof(desc_));
        memcpy((BYTE*)output + sizeof(desc_), (BYTE*)&baseline + 24, 4);
    }
};

class FactoryAdapter final : public XStorageFactory {
    volatile LONG refs_ = 1;
    IDStorageFactory* native_;
public:
    explicit FactoryAdapter(IDStorageFactory* p) : native_(p) {}
    ~FactoryAdapter() { native_->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER; *out = nullptr;
        if (iid != IID_IUnknown && iid != X_IID_StorageFactory) { trace_iid("Factory.QI rejected", iid); return E_NOINTERFACE; }
        *out = static_cast<XStorageFactory*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = InterlockedDecrement(&refs_); if (!r) delete this; return r;
    }
    HRESULT STDMETHODCALLTYPE CreateQueue(const XStorageQueueDesc* d, REFIID iid, void** out) override {
        if (!d) return E_INVALIDARG;
        XStorageQueueDesc1 d1{}; memcpy(&d1, d, sizeof(*d)); return CreateQueue1(&d1, iid, out);
    }
    HRESULT STDMETHODCALLTYPE OpenFile(const wchar_t* path, REFIID iid, void** out) override {
        if (!out) return E_POINTER; *out = nullptr;
        trace_iid("Factory.OpenFile", iid); trace("Factory.OpenFile path=%ls", path ? path : L"(null)");
        if (iid != X_IID_StorageFile) return E_NOINTERFACE;
        if (!path) return E_INVALIDARG;
        IDStorageFile* p = nullptr;
        HRESULT hr = native_->OpenFile(path, IID_PPV_ARGS(&p));
        if (FAILED(hr)) { trace("Factory.OpenFile HRESULT=0x%08lx", hr); return hr; }
        auto f = new(std::nothrow) FileAdapter(p, path);
        if (!f) { p->Release(); return E_OUTOFMEMORY; }
        *out = f; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateStatusArray(UINT32 capacity, const char* name, REFIID iid, void** out) override {
        if (!out) return E_POINTER; *out = nullptr;
        trace_iid("Factory.CreateStatusArray", iid); trace("Factory.CreateStatusArray capacity=%u name=%s", capacity, name ? name : "");
        if (iid != X_IID_StorageStatusArray) return E_NOINTERFACE;
        IDStorageStatusArray* p = nullptr;
        HRESULT hr = native_->CreateStatusArray(capacity, name, IID_PPV_ARGS(&p));
        if (FAILED(hr)) return hr;
        auto s = new(std::nothrow) StatusAdapter(p, capacity);
        if (!s) { p->Release(); return E_OUTOFMEMORY; }
        *out = s; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetCpuAffinity(UINT64 mask) override { unsupported("SetCpuAffinity", mask); }
    void STDMETHODCALLTYPE SetDebugFlags(UINT32 flags) override {
        trace("Factory.SetDebugFlags 0x%x", flags); native_->SetDebugFlags(flags);
    }
    HRESULT STDMETHODCALLTYPE SetStagingBufferSize(UINT32 size) override {
        HRESULT hr = native_->SetStagingBufferSize(size);
        trace("Factory.SetStagingBufferSize bytes=%u HRESULT=0x%08lx", size, hr); return hr;
    }
    void STDMETHODCALLTYPE GetStats(void* stats) override { (void)stats; unsupported("Factory.GetStats"); }
    HRESULT STDMETHODCALLTYPE CreateQueue1(const XStorageQueueDesc1* d, REFIID iid, void** out) override {
        if (!out) return E_POINTER; *out = nullptr;
        if (!d) return E_INVALIDARG;
        trace_iid("Factory.CreateQueue1", iid);
        trace("Factory.CreateQueue1 source=%llu capacity=%u priority=%d name=%s options=0x%llx", d->SourceType, d->Capacity,
              d->Priority, d->Name ? d->Name : "", d->Options.AsUINT64);
        if (iid != X_IID_StorageQueue) return E_NOINTERFACE;
        if (d->Options.AsUINT64 & ~1ull) return E_NOTIMPL;
        DSTORAGE_QUEUE_DESC pc{};
        pc.SourceType = static_cast<DSTORAGE_REQUEST_SOURCE_TYPE>(d->SourceType);
        pc.Capacity = d->Capacity; pc.Priority = static_cast<DSTORAGE_PRIORITY>(d->Priority); pc.Name = d->Name;
        IDStorageQueue1* p = nullptr;
        HRESULT hr = native_->CreateQueue(&pc, IID_PPV_ARGS(&p));
        trace("Factory.CreateQueue1 native HRESULT=0x%08lx", hr);
        if (FAILED(hr)) return hr;
        auto q = new(std::nothrow) QueueAdapter(p, *d);
        if (!q) { p->Release(); return E_OUTOFMEMORY; }
        *out = q; return S_OK;
    }
};

extern "C" __declspec(dllexport) HRESULT WINAPI XboxDStorageGetFactory(REFIID iid, void** out) {
    if (!out) return E_POINTER; *out = nullptr;
    trace_iid("DStorageGetFactory", iid);
    if (iid != X_IID_StorageFactory) return E_NOINTERFACE;
    // Resolve only the PC runtime beside this adapter. Do not search PATH.
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&XboxDStorageGetFactory), &self)) return HRESULT_FROM_WIN32(GetLastError());
    wchar_t dll[MAX_PATH];
    DWORD length = GetModuleFileNameW(self, dll, ARRAYSIZE(dll));
    if (!length || length >= ARRAYSIZE(dll)) return E_FAIL;
    auto slash = wcsrchr(dll, L'\\'); if (!slash) return E_FAIL;
    wcscpy_s(slash + 1, ARRAYSIZE(dll) - (slash + 1 - dll), L"dstorage.dll");
    HMODULE runtime = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!runtime) { HRESULT hr = HRESULT_FROM_WIN32(GetLastError()); trace("Load PC runtime HRESULT=0x%08lx", hr); return hr; }
    using GetFactory = HRESULT(WINAPI*)(REFIID, void**);
    auto get_factory = reinterpret_cast<GetFactory>(GetProcAddress(runtime, "DStorageGetFactory"));
    if (!get_factory) { FreeLibrary(runtime); return E_NOINTERFACE; }
    IDStorageFactory* p = nullptr;
    HRESULT hr = get_factory(IID_PPV_ARGS(&p));
    trace("PC DStorageGetFactory HRESULT=0x%08lx", hr);
    if (FAILED(hr)) { FreeLibrary(runtime); return hr; }
    // Keep runtime loaded for the lifetime of its asynchronous worker threads.
    auto f = new(std::nothrow) FactoryAdapter(p);
    if (!f) { p->Release(); return E_OUTOFMEMORY; }
    *out = f; return S_OK;
}
