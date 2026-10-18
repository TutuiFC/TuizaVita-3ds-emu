// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <thread>
#include <vector>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/http.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include "citra_vita/vita_covers.h"
#include "citra_vita/vita_i18n.h"
#include "common/common_types.h"

namespace VitaFrontend::Covers {
namespace {

constexpr char kCoverDir[] = "ux0:/data/azahar/caratulas";
constexpr int kNetMemory = 256 * 1024;
constexpr unsigned int kHttpPool = 256 * 1024;
/// Una caratula de GameTDB ocupa ~100 KB; mas de esto no es una caratula.
constexpr std::size_t kMaxCover = 2 * 1024 * 1024;
constexpr unsigned int kTimeoutUs = 6'000'000;

std::thread g_thread;
std::atomic<bool> g_stop{false};
std::atomic<bool> g_active{false};
std::atomic<int> g_done{0};
std::atomic<int> g_total{0};

bool Exists(const std::string& path) {
    SceIoStat stat{};
    return sceIoGetstat(path.c_str(), &stat) >= 0;
}

bool ReadAt(SceUID fd, SceOff offset, void* out, SceSize size) {
    return sceIoLseek(fd, offset, SCE_SEEK_SET) == offset &&
           sceIoRead(fd, out, size) == static_cast<int>(size);
}

/// Region de GameTDB para el idioma del menu.
const char* MenuRegion() {
    switch (I18n::CurrentLanguage()) {
    case I18n::kSpanish:
    case I18n::kCatalan:
        return "ES";
    case I18n::kFrench:
        return "FR";
    case I18n::kGerman:
        return "DE";
    case I18n::kItalian:
        return "IT";
    case I18n::kPortuguese:
        return "PT";
    default:
        return "EN";
    }
}

/// Region por la ultima letra del ID (E = America, J = Japon, K = Corea...).
const char* GameRegion(char letter) {
    switch (letter) {
    case 'E':
        return "US";
    case 'J':
        return "JA";
    case 'K':
        return "KO";
    default:
        return "EN";
    }
}

struct Network {
    void* memory = nullptr;
    bool net_module = false;
    bool http_module = false;
    bool net = false;
    bool netctl = false;
    bool http = false;

    bool Init() {
        net_module = sceSysmoduleLoadModule(SCE_SYSMODULE_NET) >= 0;
        if (!net_module) {
            return false;
        }
        memory = std::malloc(kNetMemory);
        if (memory == nullptr) {
            return false;
        }
        SceNetInitParam param{};
        param.memory = memory;
        param.size = kNetMemory;
        param.flags = 0;
        net = sceNetInit(&param) >= 0;
        netctl = net && sceNetCtlInit() >= 0;
        http_module = netctl && sceSysmoduleLoadModule(SCE_SYSMODULE_HTTP) >= 0;
        http = http_module && sceHttpInit(kHttpPool) >= 0;
        return http;
    }

    ~Network() {
        if (http) {
            sceHttpTerm();
        }
        if (http_module) {
            sceSysmoduleUnloadModule(SCE_SYSMODULE_HTTP);
        }
        if (netctl) {
            sceNetCtlTerm();
        }
        if (net) {
            sceNetTerm();
        }
        if (net_module) {
            sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
        }
        std::free(memory);
    }
};

/// GET de una URL. Devuelve el codigo HTTP, o -1 si fallo la red.
int Download(int templ, const std::string& url, std::vector<u8>& out) {
    out.clear();
    const int connection = sceHttpCreateConnectionWithURL(templ, url.c_str(), SCE_FALSE);
    if (connection < 0) {
        return -1;
    }
    int status = -1;
    const int request =
        sceHttpCreateRequestWithURL(connection, SCE_HTTP_METHOD_GET, url.c_str(), 0);
    if (request >= 0) {
        if (sceHttpSendRequest(request, nullptr, 0) >= 0 &&
            sceHttpGetStatusCode(request, &status) >= 0) {
            if (status == 200) {
                u8 chunk[16 * 1024];
                while (!g_stop.load(std::memory_order_relaxed)) {
                    const int read = sceHttpReadData(request, chunk, sizeof(chunk));
                    if (read < 0) {
                        status = -1;
                        break;
                    }
                    if (read == 0) {
                        break;
                    }
                    out.insert(out.end(), chunk, chunk + read);
                    if (out.size() > kMaxCover) {
                        status = -1;
                        break;
                    }
                }
            }
        } else {
            status = -1;
        }
        sceHttpDeleteRequest(request);
    }
    sceHttpDeleteConnection(connection);
    return status;
}

bool WriteFile(const std::string& path, const void* data, std::size_t size) {
    const SceUID fd = sceIoOpen(path.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) {
        return false;
    }
    const bool ok = size == 0 || sceIoWrite(fd, data, static_cast<SceSize>(size)) ==
                                     static_cast<int>(size);
    sceIoClose(fd);
    return ok;
}

void Worker(std::vector<std::string> paths) {
    Network network;
    if (!network.Init()) {
        g_active.store(false);
        return;
    }
    // Sin conexion a internet no se intenta nada (y no se marca nada como
    // inexistente: la proxima vez puede haber wifi).
    int state = 0;
    for (int i = 0; i < 30 && !g_stop.load(); i++) {
        if (sceNetCtlInetGetState(&state) >= 0 && state == SCE_NETCTL_STATE_CONNECTED) {
            break;
        }
        sceKernelDelayThread(100'000);
    }
    if (state != SCE_NETCTL_STATE_CONNECTED) {
        g_active.store(false);
        return;
    }

    const int templ = sceHttpCreateTemplate("AzaharVita/1.0", SCE_HTTP_VERSION_1_1, SCE_TRUE);
    if (templ < 0) {
        g_active.store(false);
        return;
    }
    sceHttpSetConnectTimeOut(templ, kTimeoutUs);
    sceHttpSetRecvTimeOut(templ, kTimeoutUs);
    sceIoMkdir(kCoverDir, 0777);

    std::vector<u8> data;
    for (const std::string& path : paths) {
        if (g_stop.load()) {
            break;
        }
        const std::string id = ReadProductId(path);
        if (id.empty() || Exists(CoverPath(id)) ||
            Exists(std::string(kCoverDir) + "/" + id + ".none")) {
            g_done.fetch_add(1);
            continue;
        }
        const char* regions[] = {MenuRegion(), GameRegion(id[3]), "EN", "US", "JA"};
        bool found = false;
        bool network_error = false;
        for (std::size_t i = 0; i < std::size(regions) && !found && !g_stop.load(); i++) {
            bool repeated = false;
            for (std::size_t j = 0; j < i; j++) {
                repeated |= std::strcmp(regions[i], regions[j]) == 0;
            }
            if (repeated) {
                continue;
            }
            const std::string url = std::string("http://art.gametdb.com/3ds/box/") +
                                    regions[i] + "/" + id + ".png";
            const int status = Download(templ, url, data);
            if (status == 200 && data.size() > 8 && data[0] == 0x89 && data[1] == 'P' &&
                data[2] == 'N' && data[3] == 'G') {
                // A un temporal y luego renombrar: el menu nunca ve un PNG a medias.
                const std::string temp = CoverPath(id) + ".tmp";
                if (WriteFile(temp, data.data(), data.size())) {
                    sceIoRename(temp.c_str(), CoverPath(id).c_str());
                    found = true;
                }
            } else if (status < 0 || status >= 500) {
                network_error = true;
                break;
            }
        }
        if (!found && !network_error && !g_stop.load()) {
            WriteFile(std::string(kCoverDir) + "/" + id + ".none", nullptr, 0);
        }
        g_done.fetch_add(1);
    }
    sceHttpDeleteTemplate(templ);
    g_active.store(false);
}

} // Anonymous namespace

std::string ReadProductId(const std::string& rom_path) {
    const SceUID fd = sceIoOpen(rom_path.c_str(), SCE_O_RDONLY, 0);
    if (fd < 0) {
        return {};
    }
    u8 header[0x200];
    std::string code;
    if (ReadAt(fd, 0, header, sizeof(header))) {
        SceOff ncch = -1;
        if (std::memcmp(header + 0x100, "NCSD", 4) == 0) {
            // .3ds/.cci: la particion 0 es el juego, en unidades de 0x200.
            u32 offset = 0;
            std::memcpy(&offset, header + 0x120, sizeof(offset));
            ncch = static_cast<SceOff>(offset) * 0x200;
            if (!ReadAt(fd, ncch, header, sizeof(header))) {
                ncch = -1;
            }
        } else {
            ncch = 0;
        }
        if (ncch >= 0 && std::memcmp(header + 0x100, "NCCH", 4) == 0) {
            code.assign(reinterpret_cast<const char*>(header + 0x150),
                        strnlen(reinterpret_cast<const char*>(header + 0x150), 16));
        }
    }
    sceIoClose(fd);

    // "CTR-P-ECLA" -> "ECLA"
    const std::size_t dash = code.find_last_of('-');
    std::string id = dash == std::string::npos ? code : code.substr(dash + 1);
    if (id.size() != 4) {
        return {};
    }
    for (const char c : id) {
        if (!std::isalnum(static_cast<unsigned char>(c))) {
            return {};
        }
    }
    return id;
}

std::string CoverPath(const std::string& id) {
    return std::string(kCoverDir) + "/" + id + ".png";
}

void Start(const std::vector<std::string>& rom_paths) {
    Stop();
    if (rom_paths.empty()) {
        return;
    }
    g_stop.store(false);
    g_done.store(0);
    g_total.store(static_cast<int>(rom_paths.size()));
    g_active.store(true);
    g_thread = std::thread(Worker, rom_paths);
}

void Stop() {
    g_stop.store(true);
    if (g_thread.joinable()) {
        g_thread.join();
    }
    g_active.store(false);
}

Progress GetProgress() {
    return {g_active.load(), g_done.load(), g_total.load()};
}

} // namespace VitaFrontend::Covers
