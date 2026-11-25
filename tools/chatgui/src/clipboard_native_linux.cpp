#define FUSE_USE_VERSION 31
#include "clipboard_native.h"
#include <fuse3/fuse.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <thread>

namespace {
class LinuxClipboard final : public ClipboardNative {
public:
    std::shared_ptr<ClipboardFileTree> tree;
    std::string mount;
    ClipboardNativeLog log;
    struct fuse* session = nullptr;
    std::thread worker;
    static LinuxClipboard* self() { return static_cast<LinuxClipboard*>(fuse_get_context()->private_data); }
    int find(const char* path) const {
        if (!std::strcmp(path, "/")) return -2;
        for (std::size_t i = 0; i < tree->items.size(); i++) if (tree->items[i].path == path + 1) return int(i);
        return -1;
    }
    static int getattr(const char* path, struct stat* st, struct fuse_file_info*) {
        auto* ctx = self(); int index = ctx->find(path);
        if (index == -1) return -ENOENT;
        std::memset(st, 0, sizeof(*st)); st->st_uid = getuid(); st->st_gid = getgid(); st->st_ino = index + 3;
        const bool dir = index == -2 || ctx->tree->items[index].directory;
        /* Read-only задаёт mount; режим копии в проводнике должен позволять владельцу запись. */
        st->st_mode = dir ? S_IFDIR | 0700 : S_IFREG | 0600; st->st_nlink = dir ? 2 : 1;
        if (!dir) st->st_size = off_t(ctx->tree->items[index].size);
        st->st_blksize = 65536; st->st_blocks = (st->st_size + 511) / 512; return 0;
    }
    static int readdir(const char* path, void* buffer, fuse_fill_dir_t fill, off_t offset,
        struct fuse_file_info*, enum fuse_readdir_flags) {
        auto* ctx = self(); int index = ctx->find(path);
        if (index == -1) return -ENOENT;
        if (index >= 0 && !ctx->tree->items[index].directory) return -ENOTDIR;
        std::vector<std::string> names = {".", ".."};
        std::string prefix = index == -2 ? "" : ctx->tree->items[index].path + "/";
        for (const auto& item : ctx->tree->items) {
            if (item.path.compare(0, prefix.size(), prefix)) continue;
            std::string name = item.path.substr(prefix.size());
            if (!name.empty() && name.find('/') == std::string::npos) names.push_back(std::move(name));
        }
        for (std::size_t i = std::size_t(offset); i < names.size(); i++)
            if (fill(buffer, names[i].c_str(), nullptr, off_t(i + 1), fuse_fill_dir_flags(0))) break;
        return 0;
    }
    static int open(const char* path, struct fuse_file_info* info) {
        int index = self()->find(path);
        if (index == -1) return -ENOENT;
        if (index < 0 || self()->tree->items[index].directory) return -EISDIR;
        if ((info->flags & O_ACCMODE) != O_RDONLY) return -EROFS;
        info->fh = std::uint64_t(index); self()->tree->users++; return 0;
    }
    static int release(const char*, struct fuse_file_info*) { self()->tree->users--; return 0; }
    static int read(const char*, char* data, size_t size, off_t offset, struct fuse_file_info* info) {
        if (offset < 0) return -EINVAL;
        int result = self()->tree->read(std::size_t(info->fh), data, size, std::uint64_t(offset));
        return result < 0 ? -EIO : result;
    }
    ~LinuxClipboard() override {
        tree->cancel();
        if (session) { fuse_exit(session); fuse_unmount(session); }
        if (worker.joinable()) worker.join();
        if (session) fuse_destroy(session);
        log(false, "FUSE clipboard unmounted");
    }
    std::vector<std::string> paths() const override {
        std::vector<std::string> paths; for (const auto& root : tree->roots) paths.push_back(mount + "/" + root); return paths;
    }
    bool publish() override { return false; }
    bool drag() override { return false; }
};
}
std::shared_ptr<ClipboardNative> createClipboardNative(std::shared_ptr<ClipboardFileTree> tree,
    const std::string& mountPath, std::string& error, ClipboardNativeLog logs) {
    auto ctx = std::make_shared<LinuxClipboard>(); ctx->tree = std::move(tree); ctx->mount = mountPath; ctx->log = std::move(logs);
    struct fuse_operations ops{}; ops.getattr = LinuxClipboard::getattr; ops.readdir = LinuxClipboard::readdir;
    ops.open = LinuxClipboard::open; ops.read = LinuxClipboard::read; ops.release = LinuxClipboard::release;
    char app[] = "vibechat-clipboard", option[] = "-o", settings[] = "ro,default_permissions,fsname=vibechat-clipboard";
    char* argv[] = {app, option, settings}; struct fuse_args args = FUSE_ARGS_INIT(3, argv);
    ctx->session = fuse_new(&args, &ops, sizeof(ops), ctx.get()); fuse_opt_free_args(&args);
    if (!ctx->session || fuse_mount(ctx->session, mountPath.c_str())) {
        error = "Cannot mount clipboard FUSE filesystem: " + std::string(std::strerror(errno));
        ctx->log(true, error); return {};
    }
    auto* raw = ctx.get();
    try {
        ctx->worker = std::thread([raw] {
            int result = fuse_loop(raw->session);
            raw->log(result != 0, "FUSE clipboard loop ended result=" + std::to_string(result));
        });
    } catch (const std::system_error& failure) {
        error = "Cannot start FUSE clipboard worker: " + std::string(failure.what()); ctx->log(true, error); return {};
    }
    ctx->log(false, "FUSE clipboard mounted; files will be fetched on first read"); return ctx;
}
