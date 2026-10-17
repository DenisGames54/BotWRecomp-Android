// coreinit FS (synchronous API) and nn_save. Guest paths: /vol/content -> <game>/content,
// /vol/code -> <game>/code, /vol/save -> <user>/save. Handles index a host-side table.
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mutex>
#include <string>
#include <vector>

#include "hle.h"
#include "os.h"

#include <TargetConditionals.h>
#if TARGET_OS_IOS  // iOS port: case-insensitive lookup in the game folder (ios/src/fs_case_ios.cpp)
std::string botw_ios_fix_case(const std::string& host);
#endif

namespace {
enum : int32_t {
    FS_OK = 0, FS_END = -2, FS_MAX = -3, FS_EXISTS = -5, FS_NOT_FOUND = -6, FS_NOT_FILE = -7, FS_NOT_DIR = -8,
    FS_PERMISSION = -10, FS_FATAL = -0x400,
};
constexpr uint32_t FLAG_DIR = 0x80000000u, FLAG_FILE = 0x01000000u;

struct Handle {
    FILE* f = nullptr;
    DIR* d = nullptr;
    std::string host;
};
std::mutex g_fs_lock;
std::vector<Handle> g_handles(1);  // index 0 unused
std::string g_cwd = "/vol/content";

#if TARGET_OS_IOS
std::string host_path_exact(std::string p);
std::string host_path(std::string p) { return botw_ios_fix_case(host_path_exact(std::move(p))); }
#define host_path host_path_exact
#endif
std::string host_path(std::string p) {
    if (p.empty() || p[0] != '/') p = g_cwd + "/" + p;
    // normalise duplicate slashes
    std::string n;
    for (char ch : p)
        if (!(ch == '/' && !n.empty() && n.back() == '/')) n += ch;
    auto starts = [&](const char* pre) { return n.compare(0, strlen(pre), pre) == 0; };
    if (starts("/vol/content")) return g_game_root + "/content" + n.substr(12);
    if (starts("/vol/code")) return g_game_root + "/code" + n.substr(9);
    if (starts("/vol/meta")) return g_game_root + "/meta" + n.substr(9);
    if (starts("/vol/save")) return g_user_root + "/save" + n.substr(9);
    if (starts("/vol/aoc")) return g_game_root + "/aoc" + n.substr(8);
    return g_user_root + "/unknown_volume" + n;
}
#if TARGET_OS_IOS
#undef host_path
#endif

void mkdirs(const std::string& p) {
    for (size_t i = 1; i < p.size(); i++)
        if (p[i] == '/') mkdir(p.substr(0, i).c_str(), 0755);
    mkdir(p.c_str(), 0755);
}

uint32_t add_handle(Handle h) {
    for (size_t i = 1; i < g_handles.size(); i++)
        if (!g_handles[i].f && !g_handles[i].d) {
            g_handles[i] = h;
            return (uint32_t)i;
        }
    g_handles.push_back(h);
    return (uint32_t)g_handles.size() - 1;
}
Handle* get(uint32_t h) { return h && h < g_handles.size() ? &g_handles[h] : nullptr; }

void fill_stat(uint32_t out, const struct stat& st) {
    memset(mem::host(out), 0, 0x64);
    bool dir = S_ISDIR(st.st_mode);
    mem::w32(out + 0x00, dir ? FLAG_DIR : FLAG_FILE);
    mem::w32(out + 0x04, 0x666);
    mem::w32(out + 0x10, dir ? 0 : (uint32_t)st.st_size);
    mem::w32(out + 0x14, dir ? 0 : (uint32_t)st.st_size);
}

int32_t fail(PPCContext* c, int32_t err, uint32_t mask, const char* what, const std::string& path) {
    // errHandling mask: bit set = the game handles this error itself. Others are fatal on hardware.
    bool handled = mask == 0xFFFFFFFFu || (err == FS_NOT_FOUND && (mask & 0x40)) || (err == FS_EXISTS && (mask & 0x20)) ||
                   (err == FS_NOT_FILE && (mask & 0x80)) || (err == FS_NOT_DIR && (mask & 0x100));
    TRACE("FS %s %s -> %d", what, path.c_str(), err);
    if (!handled) LOG("FS %s %s failed (%d) with errHandling %x", what, path.c_str(), err, mask);
    return err;
}

int32_t open_file(PPCContext* c, const std::string& gpath, const char* mode, uint32_t handle_out, uint32_t mask) {
    std::string hp = host_path(gpath);
    struct stat st;
    bool exists = stat(hp.c_str(), &st) == 0;
    if (exists && S_ISDIR(st.st_mode)) return fail(c, FS_NOT_FILE, mask, "open", gpath);
    std::string m = mode;
    if (!exists && m.find('r') != std::string::npos && m.find('+') == std::string::npos)
        return fail(c, FS_NOT_FOUND, mask, "open", gpath);
    if (m.find('w') != std::string::npos || m.find('a') != std::string::npos) {
        size_t s = hp.find_last_of('/');
        mkdirs(hp.substr(0, s));
    }
    std::string hm;
    for (char ch : m)
        if (ch != 'b') hm += ch;
    hm += 'b';
    FILE* f = fopen(hp.c_str(), hm.c_str());
    if (!f) return fail(c, errno == ENOENT ? FS_NOT_FOUND : FS_PERMISSION, mask, "open", gpath);
    std::lock_guard<std::mutex> l(g_fs_lock);
    uint32_t h = add_handle({f, nullptr, hp});
    mem::w32(handle_out, h);
    TRACE("FS open %s (%s) -> %u", gpath.c_str(), mode, h);
    return FS_OK;
}

int32_t open_dir(PPCContext* c, const std::string& gpath, uint32_t out, uint32_t mask) {
    std::string hp = host_path(gpath);
    DIR* d = opendir(hp.c_str());
    if (!d) return fail(c, FS_NOT_FOUND, mask, "opendir", gpath);
    std::lock_guard<std::mutex> l(g_fs_lock);
    mem::w32(out, add_handle({nullptr, d, hp}));
    return FS_OK;
}

int32_t get_stat(PPCContext* c, const std::string& gpath, uint32_t out, uint32_t mask) {
    struct stat st;
    if (stat(host_path(gpath).c_str(), &st) != 0) return fail(c, FS_NOT_FOUND, mask, "stat", gpath);
    fill_stat(out, st);
    return FS_OK;
}

std::string save_path(uint32_t account, const char* path) {
    std::string base = account == 0xFF ? "/vol/save/common" : "/vol/save/" + std::to_string(0x80000000u + account);
    std::string p = path ? path : "";
    if (!p.empty() && p[0] != '/') p = "/" + p;
    return base + p;
}
}  // namespace

HLE_EXPORT(coreinit, FSInit) {}
HLE_EXPORT(coreinit, FSShutdown) {}
HLE_EXPORT(coreinit, FSAddClient) { RET(0); }
HLE_EXPORT(coreinit, FSDelClient) { RET(0); }
HLE_EXPORT(coreinit, FSInitCmdBlock) { memset(mem::host(ARG(0)), 0, 0xA80); }
HLE_EXPORT(coreinit, FSSetCmdPriority) { RET(0); }
HLE_EXPORT(coreinit, FSGetVolumeState) { RET(1); }
HLE_EXPORT(coreinit, FSGetLastError) { RET(0); }
HLE_EXPORT(coreinit, FSGetLastErrorCodeForViewer) { RET(0); }
HLE_EXPORT(coreinit, FSSetStateChangeNotification) {}

// FSOpenFile(client, block, path, mode, FSFileHandle* out, errHandling)
HLE_EXPORT(coreinit, FSOpenFile) { RET(open_file(c, mem::str(ARG(2)), mem::str(ARG(3)), ARG(4), ARG(5))); }
HLE_EXPORT(coreinit, FSCloseFile) {
    std::lock_guard<std::mutex> l(g_fs_lock);
    Handle* h = get(ARG(2));
    if (h && h->f) {
        fclose(h->f);
        *h = Handle{};
    }
    RET(0);
}
// FSReadFile(client, block, dst, size, count, handle, flag, err) -> elements read
HLE_EXPORT(coreinit, FSReadFile) {
    uint32_t dst = ARG(2), size = ARG(3), count = ARG(4);
    Handle* h;
    {
        std::lock_guard<std::mutex> l(g_fs_lock);
        h = get(ARG(5));
    }
    if (!h || !h->f) {
        RET(FS_FATAL);
        return;
    }
    size_t n = fread(mem::host(dst), 1, (size_t)size * count, h->f);
    RET(size ? (uint32_t)(n / size) : 0);
}
HLE_EXPORT(coreinit, FSWriteFile) {
    uint32_t src = ARG(2), size = ARG(3), count = ARG(4);
    Handle* h;
    {
        std::lock_guard<std::mutex> l(g_fs_lock);
        h = get(ARG(5));
    }
    if (!h || !h->f) {
        RET(FS_FATAL);
        return;
    }
    size_t n = fwrite(mem::host(src), 1, (size_t)size * count, h->f);
    RET(size ? (uint32_t)(n / size) : 0);
}
HLE_EXPORT(coreinit, FSSetPosFile) {
    std::lock_guard<std::mutex> l(g_fs_lock);
    Handle* h = get(ARG(2));
    if (h && h->f) fseeko(h->f, ARG(3), SEEK_SET);
    RET(0);
}
HLE_EXPORT(coreinit, FSFlushFile) {
    std::lock_guard<std::mutex> l(g_fs_lock);
    Handle* h = get(ARG(2));
    if (h && h->f) fflush(h->f);
    RET(0);
}
HLE_EXPORT(coreinit, FSGetStatFile) {
    std::lock_guard<std::mutex> l(g_fs_lock);
    Handle* h = get(ARG(2));
    struct stat st;
    if (!h || !h->f || fstat(fileno(h->f), &st) != 0) {
        RET(FS_FATAL);
        return;
    }
    fflush(h->f);
    fstat(fileno(h->f), &st);
    fill_stat(ARG(3), st);
    RET(0);
}
// FSGetStat(client, block, path, FSStat* out, err)
HLE_EXPORT(coreinit, FSGetStat) { RET(get_stat(c, mem::str(ARG(2)), ARG(3), ARG(4))); }
HLE_EXPORT(coreinit, FSOpenDir) { RET(open_dir(c, mem::str(ARG(2)), ARG(3), ARG(4))); }
// FSReadDir(client, block, dirHandle, FSDirEntry* out, err): 0 or -2 at the end
HLE_EXPORT(coreinit, FSReadDir) {
    uint32_t out = ARG(3);
    std::lock_guard<std::mutex> l(g_fs_lock);
    Handle* h = get(ARG(2));
    if (!h || !h->d) {
        RET(FS_FATAL);
        return;
    }
    for (;;) {
        struct dirent* e = readdir(h->d);
        if (!e) {
            RET(FS_END);
            return;
        }
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || e->d_name[0] == '.') continue;
        struct stat st;
        std::string full = h->host + "/" + e->d_name;
        if (stat(full.c_str(), &st) != 0) continue;
        fill_stat(out, st);
        memset(mem::host(out + 0x64), 0, 0x80);
        strncpy((char*)mem::host(out + 0x64), e->d_name, 0x7F);
        RET(0);
        return;
    }
}
HLE_EXPORT(coreinit, FSCloseDir) {
    std::lock_guard<std::mutex> l(g_fs_lock);
    Handle* h = get(ARG(2));
    if (h && h->d) {
        closedir(h->d);
        *h = Handle{};
    }
    RET(0);
}
HLE_EXPORT(coreinit, FSMakeDir) {
    std::string hp = host_path(mem::str(ARG(2)));
    struct stat st;
    if (stat(hp.c_str(), &st) == 0) {
        RET(fail(c, FS_EXISTS, ARG(3), "mkdir", mem::str(ARG(2))));
        return;
    }
    mkdirs(hp);
    RET(0);
}
HLE_EXPORT(coreinit, FSGetCwd) {
    uint32_t buf = ARG(2), len = ARG(3);
    if (len) {
        strncpy((char*)mem::host(buf), g_cwd.c_str(), len - 1);
        mem::w8(buf + len - 1, 0);
    }
    RET(0);
}

// ---- nn_save: per-account save folders under /vol/save -------------------------------------------
HLE_EXPORT(nn_save, SAVEInit) {
    mkdirs(g_user_root + "/save/common");
    mkdirs(g_user_root + "/save/80000001");
    RET(0);
}
HLE_EXPORT(nn_save, SAVEShutdown) {}
HLE_EXPORT(nn_save, SAVEInitSaveDir) {
    mkdirs(host_path(save_path(ARG(0), "")));
    RET(0);
}
HLE_EXPORT(nn_save, SAVEFlushQuota) { RET(0); }
// SAVEGetStat(client, block, account, path, FSStat*, err)
HLE_EXPORT(nn_save, SAVEGetStat) { RET(get_stat(c, save_path(ARG(2), mem::str(ARG(3))), ARG(4), ARG(5))); }
// SAVEOpenFile(client, block, account, path, mode, handle*, err)
HLE_EXPORT(nn_save, SAVEOpenFile) {
    RET(open_file(c, save_path(ARG(2), mem::str(ARG(3))), mem::str(ARG(4)), ARG(5), ARG(6)));
}
HLE_EXPORT(nn_save, SAVEOpenDir) { RET(open_dir(c, save_path(ARG(2), mem::str(ARG(3))), ARG(4), ARG(5))); }
HLE_EXPORT(nn_save, SAVEMakeDir) {
    std::string hp = host_path(save_path(ARG(2), mem::str(ARG(3))));
    struct stat st;
    if (stat(hp.c_str(), &st) == 0) {
        RET(fail(c, FS_EXISTS, ARG(4), "mkdir", hp));
        return;
    }
    mkdirs(hp);
    RET(0);
}

// Remove a file or an empty directory.
static int32_t remove_path(PPCContext* c, const std::string& gpath, uint32_t mask) {
    std::string hp = host_path(gpath);
    struct stat st;
    if (stat(hp.c_str(), &st) != 0) return fail(c, FS_NOT_FOUND, mask, "remove", gpath);
    if ((S_ISDIR(st.st_mode) ? rmdir(hp.c_str()) : unlink(hp.c_str())) != 0) return fail(c, FS_PERMISSION, mask, "remove", gpath);
    TRACE("FS remove %s", gpath.c_str());
    return FS_OK;
}
// FSRemove(client, block, path, err)
HLE_EXPORT(coreinit, FSRemove) { RET(remove_path(c, mem::str(ARG(2)), ARG(3))); }
// SAVERemove(client, block, account, path, err)
HLE_EXPORT(nn_save, SAVERemove) { RET(remove_path(c, save_path(ARG(2), mem::str(ARG(3))), ARG(4))); }
