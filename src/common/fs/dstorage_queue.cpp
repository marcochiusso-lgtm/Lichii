// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/fs/dstorage_queue.h"

#ifdef LICHII_DIRECTSTORAGE

#include <mutex>

// INITGUID before any Windows header: defines the DirectStorage IIDs in this translation unit,
// so dstorage.lib does not need to be linked (dstorage.dll is loaded at run time).
#define INITGUID
#include <initguid.h>
#include <windows.h>
#include <dstorage.h>

#include "common/fs/fs_util.h"
#include "common/logging.h"

namespace Common::FS::DirectStorage {
namespace {

using DStorageGetFactoryFn = HRESULT(WINAPI*)(REFIID, void**);

struct Runtime {
    ~Runtime() {
        if (queue1) {
            queue1->Release();
        }
        if (queue) {
            queue->Release();
        }
        if (factory) {
            factory->Release();
        }
        if (dll) {
            FreeLibrary(dll);
        }
    }

    HMODULE dll{};
    IDStorageFactory* factory{};
    IDStorageQueue* queue{};
    IDStorageQueue1* queue1{};
    std::once_flag init_once{};
    std::mutex queue_mutex{};
    bool available{};
};

Runtime& GetRuntime() {
    static Runtime runtime;
    return runtime;
}

void Initialize() {
    auto& runtime = GetRuntime();
    runtime.dll = LoadLibraryW(L"dstorage.dll");
    if (!runtime.dll) {
        LOG_WARNING(Common_Filesystem, "DirectStorage: dstorage.dll not found, using regular reads");
        return;
    }
    const auto get_factory =
        reinterpret_cast<DStorageGetFactoryFn>(GetProcAddress(runtime.dll, "DStorageGetFactory"));
    if (!get_factory) {
        LOG_WARNING(Common_Filesystem, "DirectStorage: DStorageGetFactory not exported");
        return;
    }
    if (FAILED(get_factory(__uuidof(IDStorageFactory), reinterpret_cast<void**>(&runtime.factory)))) {
        LOG_WARNING(Common_Filesystem, "DirectStorage: DStorageGetFactory failed");
        return;
    }
    DSTORAGE_QUEUE_DESC desc{};
    desc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    desc.Capacity = DSTORAGE_MAX_QUEUE_CAPACITY;
    desc.Priority = DSTORAGE_PRIORITY_NORMAL;
    desc.Name = "lichii";
    if (FAILED(runtime.factory->CreateQueue(&desc, __uuidof(IDStorageQueue),
                                             reinterpret_cast<void**>(&runtime.queue)))) {
        LOG_WARNING(Common_Filesystem, "DirectStorage: CreateQueue failed");
        return;
    }
    // EnqueueSetEvent needs IDStorageQueue1 (DirectStorage 1.1+).
    if (FAILED(runtime.queue->QueryInterface(__uuidof(IDStorageQueue1),
                                             reinterpret_cast<void**>(&runtime.queue1)))) {
        LOG_WARNING(Common_Filesystem, "DirectStorage: IDStorageQueue1 unavailable");
        return;
    }
    runtime.available = true;
    LOG_INFO(Common_Filesystem,
             "DirectStorage runtime loaded: file reads of at least {} KiB go through its queue",
             MinDirectStorageReadSize / 1024);
}

} // namespace

bool IsAvailable() {
    auto& runtime = GetRuntime();
    std::call_once(runtime.init_once, Initialize);
    return runtime.available;
}

bool ReadFileAt(const std::filesystem::path& path, u64 offset, void* dst, size_t size) {
    auto& runtime = GetRuntime();
    std::call_once(runtime.init_once, Initialize);
    if (!runtime.available || size == 0 || size > UINT32_MAX) {
        return false;
    }
    // One synchronous read at a time: the queue has a fixed capacity and EnqueueRequest blocks
    // when it is full, so serializing keeps the request structs (stack) alive until completion.
    std::lock_guard lock{runtime.queue_mutex};

    IDStorageFile* file = nullptr;
    if (FAILED(runtime.factory->OpenFile(path.c_str(), __uuidof(IDStorageFile),
                                         reinterpret_cast<void**>(&file)))) {
        return false;
    }
    IDStorageStatusArray* status = nullptr;
    HRESULT hr = runtime.factory->CreateStatusArray(1, "lichii-read", __uuidof(IDStorageStatusArray),
                                                    reinterpret_cast<void**>(&status));
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (FAILED(hr) || !status || !event) {
        if (status) {
            status->Release();
        }
        if (event) {
            CloseHandle(event);
        }
        file->Release();
        return false;
    }

    DSTORAGE_REQUEST request{};
    request.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
    request.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    request.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
    request.Source.File.Source = file;
    request.Source.File.Offset = offset;
    request.Source.File.Size = static_cast<UINT32>(size);
    request.Destination.Memory.Buffer = dst;
    request.Destination.Memory.Size = static_cast<UINT32>(size);
    request.Name = "lichii-read";

    runtime.queue->EnqueueRequest(&request);
    runtime.queue->EnqueueStatus(status, 0);
    runtime.queue1->EnqueueSetEvent(event);
    runtime.queue->Submit();

    const bool ok = WaitForSingleObject(event, INFINITE) == WAIT_OBJECT_0 && status->IsComplete(0) &&
                    SUCCEEDED(status->GetHResult(0));
    if (!ok) {
        LOG_WARNING(Common_Filesystem, "DirectStorage: read of {} bytes at {:#x} in \"{}\" failed",
                    size, offset, Common::FS::PathToUTF8String(path));
    }

    CloseHandle(event);
    status->Release();
    file->Release();
    return ok;
}

} // namespace Common::FS::DirectStorage

#endif // LICHII_DIRECTSTORAGE
