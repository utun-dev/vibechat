#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "clipboard_native.h"
#include <windows.h>
#include <ole2.h>
#include <shlobj.h>
#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

namespace {
std::wstring wide(const std::string& text) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0);
    std::wstring result(n, 0);
    if (n) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), result.data(), n);
    std::replace(result.begin(), result.end(), L'/', L'\\'); return result;
}
class FileStream final : public IStream {
    std::atomic<ULONG> refs{1};
    std::shared_ptr<ClipboardFileTree> tree;
    std::size_t index;
    std::uint64_t position = 0;
public:
    FileStream(std::shared_ptr<ClipboardFileTree> files, std::size_t file, std::uint64_t offset = 0)
        : tree(std::move(files)), index(file), position(offset) { tree->users++; }
    ~FileStream() { tree->users--; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_ISequentialStream && iid != IID_IStream) return E_NOINTERFACE;
        *out = static_cast<IStream*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG count = --refs; if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE Read(void* data, ULONG count, ULONG* read) override {
        if (read) *read = 0;
        if (!data && count) return STG_E_INVALIDPOINTER;
        int n = tree->read(index, static_cast<char*>(data), count, position);
        if (n < 0) return STG_E_READFAULT;
        position += std::uint64_t(n); if (read) *read = ULONG(n); return ULONG(n) == count ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* out) override {
        if (origin > STREAM_SEEK_END) return STG_E_INVALIDFUNCTION;
        std::uint64_t base = origin == STREAM_SEEK_SET ? 0 : origin == STREAM_SEEK_CUR ? position : tree->items[index].size;
        if (move.QuadPart < 0 && std::uint64_t(-(move.QuadPart + 1)) + 1 > base) return STG_E_INVALIDFUNCTION;
        if (move.QuadPart > 0 && std::uint64_t(move.QuadPart) > INT64_MAX - base) return STG_E_INVALIDFUNCTION;
        position = move.QuadPart < 0 ? base - (std::uint64_t(-(move.QuadPart + 1)) + 1) : base + std::uint64_t(move.QuadPart);
        if (out) out->QuadPart = position;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream* destination, ULARGE_INTEGER count, ULARGE_INTEGER* read, ULARGE_INTEGER* written) override {
        if (read) read->QuadPart = 0;
        if (written) written->QuadPart = 0;
        if (!destination) return E_POINTER;
        char buffer[65536]; std::uint64_t total = 0;
        while (total < count.QuadPart) {
            ULONG n = 0, copied = 0; HRESULT rc = Read(buffer, ULONG(std::min<std::uint64_t>(sizeof(buffer), count.QuadPart - total)), &n);
            if (FAILED(rc)) return rc;
            if (!n) break;
            HRESULT writeResult = destination->Write(buffer, n, &copied);
            if (read) read->QuadPart += n;
            if (written) written->QuadPart += copied;
            if (FAILED(writeResult)) return writeResult;
            if (copied != n) return STG_E_MEDIUMFULL;
            total += n;
        }
        return total == count.QuadPart ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* out, DWORD flags) override {
        if (!out) return E_POINTER;
        std::memset(out, 0, sizeof(*out)); out->type = STGTY_STREAM; out->cbSize.QuadPart = tree->items[index].size; out->grfMode = STGM_READ;
        if (!(flags & STATFLAG_NONAME)) {
            auto name = wide(tree->items[index].path); std::size_t bytes = (name.size() + 1) * sizeof(wchar_t);
            out->pwcsName = static_cast<wchar_t*>(CoTaskMemAlloc(bytes));
            if (!out->pwcsName) return E_OUTOFMEMORY;
            std::memcpy(out->pwcsName, name.c_str(), bytes);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream** out) override {
        if (!out) return E_POINTER;
        *out = new FileStream(tree, index, position); return S_OK;
    }
};
class FileData final : public IDataObject {
    std::atomic<ULONG> refs{1};
    std::shared_ptr<ClipboardFileTree> tree;
    CLIPFORMAT descriptor = CLIPFORMAT(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW));
    CLIPFORMAT contents = CLIPFORMAT(RegisterClipboardFormatW(L"FileContents"));
    CLIPFORMAT effect = CLIPFORMAT(RegisterClipboardFormatW(L"Preferred DropEffect"));
public:
    explicit FileData(std::shared_ptr<ClipboardFileTree> files) : tree(std::move(files)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDataObject) return E_NOINTERFACE;
        *out = static_cast<IDataObject*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG count = --refs; if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
        if (!format) return E_POINTER;
        if (format->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;
        if (format->cfFormat == descriptor || format->cfFormat == effect) return format->tymed & TYMED_HGLOBAL ? S_OK : DV_E_TYMED;
        if (format->cfFormat == contents) {
            if (!(format->tymed & TYMED_ISTREAM)) return DV_E_TYMED;
            if (format->lindex < 0 || std::size_t(format->lindex) >= tree->items.size() || tree->items[format->lindex].directory) return DV_E_LINDEX;
            return S_OK;
        }
        return DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* out) override {
        if (!out) return E_POINTER;
        std::memset(out, 0, sizeof(*out));
        HRESULT rc = QueryGetData(format); if (FAILED(rc)) return rc;
        if (format->cfFormat == contents) {
            out->tymed = TYMED_ISTREAM; out->pstm = new FileStream(tree, std::size_t(format->lindex)); return S_OK;
        }
        std::size_t size = format->cfFormat == effect ? sizeof(DWORD) : sizeof(UINT) + sizeof(FILEDESCRIPTORW) * tree->items.size();
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size); if (!memory) return E_OUTOFMEMORY;
        void* bytes = GlobalLock(memory); if (!bytes) { GlobalFree(memory); return E_OUTOFMEMORY; }
        if (format->cfFormat == effect) *static_cast<DWORD*>(bytes) = DROPEFFECT_COPY;
        else {
            auto* group = static_cast<FILEGROUPDESCRIPTORW*>(bytes); group->cItems = UINT(tree->items.size());
            for (std::size_t i = 0; i < tree->items.size(); i++) {
                const auto& item = tree->items[i]; auto& fd = group->fgd[i]; auto path = wide(item.path);
                fd.dwFlags = FD_ATTRIBUTES | FD_UNICODE | FD_PROGRESSUI;
                fd.dwFileAttributes = item.directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
                if (!item.directory) { fd.dwFlags |= FD_FILESIZE; fd.nFileSizeHigh = DWORD(item.size >> 32); fd.nFileSizeLow = DWORD(item.size); }
                std::memcpy(fd.cFileName, path.c_str(), (path.size() + 1) * sizeof(wchar_t));
            }
        }
        GlobalUnlock(memory); out->tymed = TYMED_HGLOBAL; out->hGlobal = memory; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return DATA_E_FORMATETC; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
        if (out) out->ptd = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM* data, BOOL release) override {
        if (release && data) ReleaseStgMedium(data);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override {
        if (direction != DATADIR_GET) return E_NOTIMPL;
        FORMATETC formats[] = {{descriptor, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
            {contents, nullptr, DVASPECT_CONTENT, -1, TYMED_ISTREAM}, {effect, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}};
        return SHCreateStdEnumFmtEtc(3, formats, out);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
};
class DropSource final : public IDropSource {
    std::atomic<ULONG> refs{1};
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDropSource) return E_NOINTERFACE;
        *out = static_cast<IDropSource*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG count = --refs; if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override {
        return escape ? DRAGDROP_S_CANCEL : !(keys & MK_LBUTTON) ? DRAGDROP_S_DROP : S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};
class WindowsClipboard final : public ClipboardNative {
public:
    std::shared_ptr<ClipboardFileTree> tree;
    ClipboardNativeLog log;
    std::thread worker;
    std::mutex mutex;
    std::condition_variable changed;
    DWORD threadId = 0;
    IStream* marshalled = nullptr;
    IDataObject* proxy = nullptr; // GUI apartment; release до остановки worker apartment.
    HRESULT status = E_PENDING;
    bool prepared = false;
    void run() {
        HRESULT initialized = OleInitialize(nullptr);
        FileData* data = SUCCEEDED(initialized) ? new FileData(tree) : nullptr;
        MSG message{}; PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
        {
            std::lock_guard<std::mutex> lock(mutex); threadId = GetCurrentThreadId();
            status = data ? CoMarshalInterThreadInterfaceInStream(IID_IDataObject, data, &marshalled) : initialized;
            prepared = true; changed.notify_all();
        }
        if (data) while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        if (data) {
            if (OleIsCurrentClipboard(data) == S_OK) OleSetClipboard(nullptr);
            CoDisconnectObject(data, 0); data->Release();
        }
        if (SUCCEEDED(initialized)) OleUninitialize();
    }
    ~WindowsClipboard() override {
        tree->cancel();
        if (proxy) { if (OleIsCurrentClipboard(proxy) == S_OK) OleSetClipboard(nullptr); proxy->Release(); }
        if (threadId) PostThreadMessageW(threadId, WM_QUIT, 0, 0);
        if (worker.joinable()) worker.join();
        if (marshalled) marshalled->Release();
        log(false, "Windows virtual clipboard stopped");
    }
    std::vector<std::string> paths() const override { return {}; }
    bool publish() override {
        HRESULT result = OleSetClipboard(proxy);
        log(FAILED(result), "Windows virtual clipboard published result=" + std::to_string(result)); return SUCCEEDED(result);
    }
    bool drag() override {
        auto* source = new DropSource; DWORD effect = DROPEFFECT_NONE;
        HRESULT result = DoDragDrop(proxy, source, DROPEFFECT_COPY, &effect); source->Release();
        log(FAILED(result), "Windows virtual file drag completed result=" + std::to_string(result));
        return result == DRAGDROP_S_DROP;
    }
};
}
std::shared_ptr<ClipboardNative> createClipboardNative(std::shared_ptr<ClipboardFileTree> tree,
    const std::string&, std::string& error, ClipboardNativeLog logs) {
    for (const auto& item : tree->items) if (wide(item.path).empty() || wide(item.path).size() >= MAX_PATH) {
        error = "Virtual file path exceeds Windows FILEDESCRIPTOR limit"; logs(true, error); return {};
    }
    auto native = std::make_shared<WindowsClipboard>(); native->tree = std::move(tree); native->log = std::move(logs);
    try { native->worker = std::thread([raw = native.get()] { raw->run(); }); }
    catch (const std::system_error& failure) {
        error = "Cannot start Windows clipboard worker: " + std::string(failure.what()); native->log(true, error); return {};
    }
    std::unique_lock<std::mutex> lock(native->mutex); native->changed.wait(lock, [&] { return native->prepared; });
    HRESULT result = native->status; IStream* marshalled = native->marshalled; native->marshalled = nullptr; lock.unlock();
    if (SUCCEEDED(result)) result = CoGetInterfaceAndReleaseStream(marshalled, IID_IDataObject, reinterpret_cast<void**>(&native->proxy));
    else if (marshalled) marshalled->Release();
    if (FAILED(result)) { error = "Cannot create Windows virtual file provider: " + std::to_string(result); native->log(true, error); return {}; }
    return native;
}
