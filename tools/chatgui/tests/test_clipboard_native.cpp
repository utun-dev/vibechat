/* Настоящие file promises ОС: metadata не читает файлы, первый read получает точные байты.
 * Windows тестируется также cross build под Wine; Linux монтирует read-only FUSE в собственном temp. */
#include "../src/clipboard_native.h"
#include <atomic>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstring>
#include <chrono>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <ole2.h>
#include <shlobj.h>
#endif
class Tree final : public ClipboardFileTree {
public:
    std::atomic<int> reads{0};
    Tree() {
        roots = {"folder"};
        items = {{"folder", 0, true}, {"folder/text.txt", 7, false}, {"folder/empty", 0, true}, {"folder/zero.txt", 0, false}};
    }
    int read(std::size_t index, char* data, std::size_t size, std::uint64_t offset) override {
        reads++;
        if (index != 1 || offset >= 7) return 0;
        size = std::min<std::size_t>(size, 7 - offset); std::memcpy(data, "payload" + offset, size); return int(size);
    }
    void cancel() override {}
};
int main() {
#ifdef _WIN32
    HRESULT initialized = OleInitialize(nullptr); assert(SUCCEEDED(initialized));
#endif
    auto tree = std::make_shared<Tree>();
    auto mount = std::filesystem::temp_directory_path() /
        ("vibechat-native-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    assert(std::filesystem::create_directory(mount)); std::string error;
    auto native = createClipboardNative(tree, mount.string(), error, [](bool, const std::string& s) { std::cerr << s << '\n'; });
    if (!native) { std::cerr << error << '\n'; std::filesystem::remove(mount); return 77; }
    assert(tree->reads == 0);
#ifdef _WIN32
    assert(native->publish());
    IDataObject* clipboard = nullptr; assert(SUCCEEDED(OleGetClipboard(&clipboard)) && clipboard);
    FORMATETC format{CLIPFORMAT(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW)), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM data{}; assert(SUCCEEDED(clipboard->GetData(&format, &data)));
    auto* descriptors = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(data.hGlobal));
    assert(descriptors && descriptors->cItems == 4 && (descriptors->fgd[0].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY));
    assert(!std::wcscmp(descriptors->fgd[1].cFileName, L"folder\\text.txt") && descriptors->fgd[1].nFileSizeLow == 7);
    GlobalUnlock(data.hGlobal); ReleaseStgMedium(&data); assert(tree->reads == 0);
    format.cfFormat = CLIPFORMAT(RegisterClipboardFormatW(L"FileContents")); format.lindex = 1; format.tymed = TYMED_ISTREAM;
    assert(SUCCEEDED(clipboard->GetData(&format, &data))); assert(tree->reads == 0);
    char buffer[16]{}; ULONG n = 0; assert(data.pstm->Read(buffer, 3, &n) == S_OK && n == 3 && !std::memcmp(buffer, "pay", 3));
    LARGE_INTEGER seek{}; seek.QuadPart = 2; assert(data.pstm->Seek(seek, STREAM_SEEK_SET, nullptr) == S_OK);
    assert(data.pstm->Read(buffer, 16, &n) == S_FALSE && n == 5 && !std::memcmp(buffer, "yload", 5));
    ReleaseStgMedium(&data); clipboard->Release(); OleSetClipboard(nullptr);
#else
    assert(std::filesystem::is_directory(mount / "folder/empty"));
    assert(std::filesystem::file_size(mount / "folder/text.txt") == 7);
    assert(std::filesystem::file_size(mount / "folder/zero.txt") == 0);
    for (const auto& entry : std::filesystem::directory_iterator(mount / "folder")) (void)entry;
    assert(tree->reads == 0);
    std::ifstream file(mount / "folder/text.txt"); std::string contents((std::istreambuf_iterator<char>(file)), {});
    assert(contents == "payload"); file.close();
#endif
    assert(tree->reads > 0); native.reset(); assert(std::filesystem::remove(mount));
#ifdef _WIN32
    OleUninitialize();
#endif
    std::cout << "PASS native clipboard: lazy files, folders, metadata, exact reads and cleanup\n"; return 0;
}
