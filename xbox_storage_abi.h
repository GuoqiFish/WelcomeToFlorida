#pragma once

// Private, minimal Xbox DirectStorage ABI contract for this executable.
// This is intentionally distinct from the PC dstorage.h types and interfaces.
// Evidence: original AMD64 PE callsites (preferred image base 0x140000000)
// and Microsoft GDK reference pages under:
// https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/
// Do not add virtual destructors: they would change these COM vtables.
#include <windows.h>
#include <unknwn.h>
#include <stddef.h>

struct ID3D12Fence;
struct XStorageFile;
struct XStorageStatusArray;

// These are the exact IIDs requested by the original executable, not the PC
// DirectStorage IIDs. Factory/queue identify the extended interfaces used here.
inline constexpr GUID X_IID_StorageFactory =
    {0x62959d11,0xd45f,0x4884,{0xa0,0xa3,0xf7,0x86,0x2e,0xa2,0x43,0xae}};
inline constexpr GUID X_IID_StorageStatusArray =
    {0xcd66d752,0x2786,0x44f7,{0x90,0x70,0xe2,0xa6,0xfd,0xad,0x3f,0x1b}};
inline constexpr GUID X_IID_StorageQueue =
    {0x3c02cb0c,0xc050,0x4195,{0x99,0xa1,0xa1,0xce,0x94,0xba,0x33,0xc5}};
inline constexpr GUID X_IID_StorageFile =
    {0x43704f9f,0x27c6,0x4a9f,{0x88,0x15,0xd1,0x65,0x27,0x89,0x4d,0xf4}};

enum XStorageSourceType : UINT64 {
    XStorageSourceFile = 0,
    XStorageSourceMemory = 1
};
enum XStoragePriority : INT8 {
    XStoragePriorityLow = -1,
    XStoragePriorityNormal = 0,
    XStoragePriorityHigh = 1,
    XStoragePriorityRealtime = 2
};

union XStorageQueueOptions {
    struct {
        UINT64 AutoSubmitDisabled : 1;
        UINT64 Reserved : 63;
    };
    UINT64 AsUINT64;
};

struct XStorageQueueDesc {
    XStorageSourceType SourceType;
    UINT16 Capacity;
    XStoragePriority Priority;
    const CHAR* Name;
};
struct XStorageQueueDesc1 {
    XStorageSourceType SourceType;
    UINT16 Capacity;
    XStoragePriority Priority;
    const CHAR* Name;
    XStorageQueueOptions Options;
};
struct XStorageQueueInfo {
    XStorageQueueDesc Desc;
    UINT16 EmptySlotCount;
    UINT16 RequestCountUntilAutoSubmit;
};
struct XStorageQueueInfo1 {
    XStorageQueueDesc1 Desc;
    UINT16 EmptySlotCount;
    UINT16 RequestCountUntilAutoSubmit;
};

// Xbox uses RFC 1950 zlib, BCPack and swizzle bits. PC compression format
// value1 is GDeflate; translating this word as PC options is incorrect.
// Prefer AsUINT64 masks when parsing input. Bitfields below assume MS ABI.
union XStorageRequestOptions {
    struct {
        UINT64 ZlibDecompress : 1;
        UINT64 BcpackMode : 4;
        UINT64 SwizzleMode : 16;
        UINT64 DestinationIsPhysicalPages : 1;
        UINT64 SourceType : 1;
        UINT64 SourceIsPhysicalPages : 1;
        UINT64 Reserved : 40;
    };
    UINT64 AsUINT64;
};

struct XStorageRequest {
    XStorageRequestOptions Options;
    union {
        void* Destination;
        struct {
            ULONG_PTR* DestinationPageArray;
            UINT16 DestinationPageOffset;
        };
    };
    UINT32 DestinationSize;
    union {
        struct {
            XStorageFile* File;
            UINT64 FileOffset;
        };
        void* Source;
        struct {
            ULONG_PTR* SourcePageArray;
            UINT16 SourcePageOffset;
        };
    };
    UINT32 SourceSize;
    UINT32 IntermediateSize;
    UINT64 CancellationTag;
    const CHAR* Name;
};

static_assert(sizeof(void*) == 8, "The game and this ABI are AMD64 only");
static_assert(sizeof(XStorageRequestOptions) == 8);
static_assert(sizeof(XStorageQueueOptions) == 8);
static_assert(sizeof(XStorageQueueDesc) == 24);
static_assert(offsetof(XStorageQueueDesc, Capacity) == 8);
static_assert(offsetof(XStorageQueueDesc, Priority) == 10);
static_assert(offsetof(XStorageQueueDesc, Name) == 16);
static_assert(sizeof(XStorageQueueDesc1) == 32);
static_assert(offsetof(XStorageQueueDesc1, Options) == 24);
static_assert(sizeof(XStorageQueueInfo) == 32);
static_assert(offsetof(XStorageQueueInfo, EmptySlotCount) == 24);
static_assert(sizeof(XStorageQueueInfo1) == 40);
static_assert(offsetof(XStorageQueueInfo1, EmptySlotCount) == 32);
static_assert(sizeof(XStorageRequest) == 72);
static_assert(offsetof(XStorageRequest, Options) == 0);
static_assert(offsetof(XStorageRequest, Destination) == 8);
static_assert(offsetof(XStorageRequest, DestinationSize) == 24);
static_assert(offsetof(XStorageRequest, File) == 32);
static_assert(offsetof(XStorageRequest, FileOffset) == 40);
static_assert(offsetof(XStorageRequest, SourceSize) == 48);
static_assert(offsetof(XStorageRequest, IntermediateSize) == 52);
static_assert(offsetof(XStorageRequest, CancellationTag) == 56);
static_assert(offsetof(XStorageRequest, Name) == 64);

struct XStorageFile : IUnknown {
    virtual void STDMETHODCALLTYPE Close() = 0;               // slot3 +0x18
    virtual HANDLE STDMETHODCALLTYPE GetHandle() = 0;         // slot4 +0x20
    // GetHandle returns a new owned Win32 handle; caller must CloseHandle it.
};

struct XStorageStatusArray : IUnknown {
    // slot3 observed at VA1400E8981 / VA144F65847; AL tested as bool.
    virtual bool STDMETHODCALLTYPE IsComplete(UINT32 index) = 0; // slot3 +0x18
    virtual HRESULT STDMETHODCALLTYPE GetHResult(UINT32 index) = 0; // slot4 +0x20
    // GetHResult signature documented, slot order mirrors PC but its game
    // callsite is not yet verified. Preserve fail-closed diagnostics.
};

struct XStorageQueue : IUnknown {
    // slot3 is observed at VA1428C9A0B and VA144FFD76F, request size72.
    virtual void STDMETHODCALLTYPE EnqueueRequest(const XStorageRequest*) = 0;
    // slot4 observed at VA1400E8940 (status pointer, index zero).
    virtual void STDMETHODCALLTYPE EnqueueStatus(XStorageStatusArray*, UINT32) = 0;
    virtual void STDMETHODCALLTYPE EnqueueSignal(ID3D12Fence*, UINT64) = 0;
    // slot6 observed at VA1400E894D after EnqueueStatus.
    virtual void STDMETHODCALLTYPE Submit() = 0;
    virtual void STDMETHODCALLTYPE CancelRequestsWithTag(UINT64, UINT64) = 0;
    // slot8 is observed during queue cleanup at VA1428C9637.
    virtual void STDMETHODCALLTYPE Close() = 0;
    virtual HANDLE STDMETHODCALLTYPE GetErrorEvent() = 0;
    // Opaque Xbox error record: never write a PC error record to this pointer.
    virtual void STDMETHODCALLTYPE RetrieveErrorRecord(void*) = 0;
    virtual void STDMETHODCALLTYPE Query(XStorageQueueInfo*) = 0;
    // Documented inheritance: X1 appends SetEvent, X2 appends Query1.
    // These are NOT PC Queue2's GetCompressionSupport slot13.
    virtual void STDMETHODCALLTYPE EnqueueSetEvent(HANDLE) = 0; // slot12 +0x60
    virtual void STDMETHODCALLTYPE Query1(XStorageQueueInfo1*) = 0; // slot13 +0x68
    // Unobserved baseline slots use documented signatures and PC-compatible
    // ordering as a provisional ABI. Diagnose unexpected calls before use.
};

struct XStorageFactory : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateQueue(const XStorageQueueDesc*, REFIID, void**) = 0;
    // slot4 observed at VA1428C8C1E, IID at RVA62FDAC8.
    virtual HRESULT STDMETHODCALLTYPE OpenFile(const WCHAR*, REFIID, void**) = 0;
    // slot5 observed at VA1428C9522, IID at RVA62FC7E8.
    virtual HRESULT STDMETHODCALLTYPE CreateStatusArray(UINT32, const CHAR*, REFIID, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCpuAffinity(UINT64) = 0;
    virtual void STDMETHODCALLTYPE SetDebugFlags(UINT32) = 0;
    // slot8 observed at VA141D8E515 (32 MiB), HRESULT checked.
    virtual HRESULT STDMETHODCALLTYPE SetStagingBufferSize(UINT32) = 0;
    // IDStorageFactoryX1 adds GetStats at slot9. Output layout intentionally
    // opaque: not observed and not safe to fill using PC structures.
    virtual void STDMETHODCALLTYPE GetStats(void*) = 0;
    // IDStorageFactoryX2 adds CreateQueue1 at slot10 +0x50, directly observed
    // VA1428C95C7. DESC1.Options == 1 (AutoSubmitDisabled) at that callsite.
    virtual HRESULT STDMETHODCALLTYPE CreateQueue1(const XStorageQueueDesc1*, REFIID, void**) = 0;
};
