// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "video_core/renderer_gxm/gxm_caja_negra.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <psp2/gxm.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/threadmgr.h>
#include "common/vita_diag.h"

namespace Gxm::BlackBox {

namespace {

/// Para encontrarla en el volcado: 16 bytes, sin cero al final.
constexpr char kMagic[16] = {'A', 'Z', 'A', 'H', 'A', 'R', 'C', 'A',
                             'J', 'A', 'N', 'E', 'G', 'R', 'A', '1'};
constexpr char kMagicEnd[16] = {'F', 'I', 'N', 'C', 'A', 'J', 'A', 'N',
                                'E', 'G', 'R', 'A', '0', '0', '0', '1'};
constexpr u32 kVersion = 1;
constexpr u32 kScenes = 16;
constexpr u32 kDraws = 48;
constexpr u32 kTextures = 3;
/// Sin avanzar la valla todo este rato, con escenas mandadas: colgada.
constexpr u32 kStallMs = 1500;
constexpr const char* kReportPath = "ux0:/data/azahar/gpu_colgada.txt";

/**
 * TODO DE 32 BITS Y SIN HUECOS: el decodificador del volcado (fuera de la
 * consola) lee esto a mano. Si cambia algo, sube kVersion.
 */
struct Draw {
    u8 kind;
    u8 primitive;
    u8 streams;
    u8 textures;
    u32 seq; ///< Dibujado numero tal desde el arranque.
    u32 count;
    u32 range;
    u32 indices;
    u32 vertex_program;
    u32 fragment_program;
    u32 stream[3];
    u32 stream_bytes[3];
    u32 texture[kTextures];
    u32 texture_size[kTextures]; ///< ancho << 16 | alto.
    u32 texture_format[kTextures];
    u32 bad; ///< Triangulos aplastados (ver DrawInfo::bad).
};
static_assert(sizeof(Draw) == 92);

struct Scene {
    u8 kind;
    u8 pad[3];
    u32 fence; ///< 0 mientras esta abierta.
    u32 color;
    u32 draws; ///< Dibujados en total; los ultimos kDraws estan en draw[n % kDraws].
    u32 seq;
    u32 open_ms;
    u32 close_ms;
    u32 reserved;
    Draw draw[kDraws];
};
static_assert(sizeof(Scene) == 32 + kDraws * sizeof(Draw));

struct Box {
    char magic[16];
    u32 version;
    u32 size;
    u32 scene_bytes;
    u32 draw_bytes;
    u32 scenes;
    u32 draws;
    u32 fence_sent;
    u32 fence_done; ///< La lee este hilo cada 100 ms.
    u32 last_advance_ms;
    u32 now_ms;
    u32 scene_seq; ///< Escenas abiertas desde el arranque.
    u32 draw_seq;
    u32 current; ///< Indice de la ultima escena abierta.
    u32 ring_base;
    u32 ring_size;
    u32 stalled;
    Scene scene[kScenes];
    char magic_end[16];
};

std::atomic<Box*> g_box{nullptr};
const volatile unsigned int* g_fence = nullptr;

// Solo los toca el hilo que dibuja.
bool g_scene_open = false;
Draw g_pending{}; ///< Las texturas del dibujado que viene.

u32 Address(const void* pointer) {
    return static_cast<u32>(reinterpret_cast<std::uintptr_t>(pointer));
}

u32 NowMs() {
    return static_cast<u32>(Common::VitaMicros() / 1000);
}

Scene* CurrentScene(Box& box) {
    return &box.scene[box.current % kScenes];
}

/// Un buffer para el informe: fuera de la pila, que ya la ocupa la caja.
char g_report[48 * 1024];

const char* SceneName(u8 kind) {
    switch (static_cast<SceneKind>(kind)) {
    case SceneKind::Draw:
        return "superficie";
    case SceneKind::Blit:
        return "copia de pantalla";
    case SceneKind::Clear:
        return "borrado";
    default:
        return "sin apuntar";
    }
}

const char* DrawName(u8 kind) {
    switch (static_cast<DrawKind>(kind)) {
    case DrawKind::VertexGpu:
        return "vs-gpu";
    case DrawKind::VertexCpu:
        return "vs-cpu";
    case DrawKind::Blit:
        return "blit";
    case DrawKind::Clear:
        return "borrado";
    default:
        return "?";
    }
}

std::size_t AppendDraw(std::size_t n, const Draw& d) {
    n += std::snprintf(g_report + n, sizeof(g_report) - n,
                       "  #%u %s prim %u n %u rango %u idx %08x vp %08x fp %08x", d.seq,
                       DrawName(d.kind), d.primitive, d.count, d.range, d.indices,
                       d.vertex_program, d.fragment_program);
    if (d.bad != 0) {
        n += std::snprintf(g_report + n, sizeof(g_report) - n, " APLASTADOS %u", d.bad);
    }
    for (u32 s = 0; s < d.streams && s < 3 && n < sizeof(g_report); s++) {
        n += std::snprintf(g_report + n, sizeof(g_report) - n, " f%u %08x+%u", s, d.stream[s],
                           d.stream_bytes[s]);
    }
    for (u32 t = 0; t < kTextures && n < sizeof(g_report); t++) {
        if (((d.textures >> t) & 1u) == 0) {
            continue;
        }
        n += std::snprintf(g_report + n, sizeof(g_report) - n, " t%u %08x %ux%u fmt %08x", t,
                           d.texture[t], d.texture_size[t] >> 16, d.texture_size[t] & 0xFFFF,
                           d.texture_format[t]);
    }
    if (n < sizeof(g_report)) {
        n += std::snprintf(g_report + n, sizeof(g_report) - n, "\n");
    }
    return n;
}

std::size_t AppendScene(std::size_t n, const Scene& s, bool full) {
    n += std::snprintf(g_report + n, sizeof(g_report) - n,
                       "escena %u (%s) valla %u color %08x: %u dibujados, abierta en %u ms, "
                       "cerrada en %u ms\n",
                       s.seq, SceneName(s.kind), s.fence, s.color, s.draws, s.open_ms,
                       s.close_ms);
    if (!full) {
        return n;
    }
    const u32 kept = s.draws < kDraws ? s.draws : kDraws;
    for (u32 i = s.draws - kept; i < s.draws && n + 512 < sizeof(g_report); i++) {
        n = AppendDraw(n, s.draw[i % kDraws]);
    }
    return n;
}

void WriteReport(const Box& box) {
    std::size_t n = 0;
    n += std::snprintf(g_report + n, sizeof(g_report) - n,
                       "gpu colgada: valla mandada %u, hecha %u, sin avanzar desde %u ms "
                       "(ahora %u ms); anillo de vertices %08x+%u\n",
                       box.fence_sent, box.fence_done, box.last_advance_ms, box.now_ms,
                       box.ring_base, box.ring_size);
    // Por orden de apertura: las pendientes enteras, de las hechas solo la
    // cabecera, para comparar.
    const u32 first = box.scene_seq > kScenes ? box.scene_seq - kScenes : 0;
    for (u32 seq = first; seq < box.scene_seq && n + 1024 < sizeof(g_report); seq++) {
        const Scene& s = box.scene[seq % kScenes];
        const bool pending = s.fence == 0 || static_cast<s32>(s.fence - box.fence_done) > 0;
        n = AppendScene(n, s, pending);
    }
    const SceUID fd = sceIoOpen(kReportPath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, g_report, static_cast<SceSize>(n));
        sceIoClose(fd);
    }
    Common::VitaNote("gxm caja negra", "la GPU no avanza: escrito gpu_colgada.txt");
}

void* BoxMain(void*) {
    // Por encima del compilador de shaders, que comparte nucleo y pasa minutos
    // sin soltarlo: si la GPU se cuelga, esto tiene que poder mirar la valla.
    sceKernelChangeThreadPriority(sceKernelGetThreadId(), Common::kVitaPriorityBackground - 1);
    Box box{};
    std::memcpy(box.magic, kMagic, sizeof(kMagic));
    std::memcpy(box.magic_end, kMagicEnd, sizeof(kMagicEnd));
    box.version = kVersion;
    box.size = sizeof(Box);
    box.scene_bytes = sizeof(Scene);
    box.draw_bytes = sizeof(Draw);
    box.scenes = kScenes;
    box.draws = kDraws;
    box.last_advance_ms = NowMs();
    g_box.store(&box, std::memory_order_release);
    u32 last_done = 0;
    while (true) {
        sceKernelDelayThread(100 * 1000);
        const u32 now = NowMs();
        const u32 done = g_fence != nullptr ? *g_fence : 0;
        box.now_ms = now;
        box.fence_done = done;
        if (done != last_done) {
            last_done = done;
            box.last_advance_ms = now;
        }
        const u32 sent = box.fence_sent;
        if (box.stalled == 0 && static_cast<s32>(sent - done) > 0 &&
            now - box.last_advance_ms > kStallMs) {
            box.stalled = 1;
            WriteReport(box);
        }
    }
    return nullptr;
}

} // Anonymous namespace

void Start(const volatile unsigned int* fence) {
    g_fence = fence;
    static bool started = false;
    if (started) {
        return;
    }
    started = true;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    // La caja (unos 70 KB) va en esta pila.
    pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_t thread;
    if (pthread_create(&thread, &attr, &BoxMain, nullptr) == 0) {
        pthread_detach(thread);
        // Que este puesta antes del primer dibujado.
        for (u32 i = 0; i < 100 && g_box.load(std::memory_order_acquire) == nullptr; i++) {
            sceKernelDelayThread(1000);
        }
    }
    pthread_attr_destroy(&attr);
    Common::VitaNote("gxm caja negra", g_box.load(std::memory_order_acquire) != nullptr
                                           ? "lista (ultimas 16 escenas en la pila de su hilo)"
                                           : "no se pudo crear su hilo");
}

void NoteVertexRing(const void* base, u32 size) {
    Box* const box = g_box.load(std::memory_order_relaxed);
    if (box == nullptr) {
        return;
    }
    box->ring_base = Address(base);
    box->ring_size = size;
}

void OpenScene(SceneKind kind, u32 color) {
    Box* const box = g_box.load(std::memory_order_relaxed);
    if (box == nullptr) {
        return;
    }
    const u32 index = box->scene_seq % kScenes;
    Scene& s = box->scene[index];
    s.kind = static_cast<u8>(kind);
    s.fence = 0;
    s.color = color;
    s.draws = 0;
    s.seq = box->scene_seq;
    s.open_ms = NowMs();
    s.close_ms = 0;
    box->current = index;
    box->scene_seq++;
    g_scene_open = true;
}

void CloseScene(u32 fence) {
    Box* const box = g_box.load(std::memory_order_relaxed);
    if (box == nullptr) {
        return;
    }
    if (g_scene_open) {
        Scene& s = *CurrentScene(*box);
        s.fence = fence;
        s.close_ms = NowMs();
    }
    box->fence_sent = fence;
    g_scene_open = false;
}

void NoteTexture(u32 slot, const SceGxmTexture* texture) {
    if (slot >= kTextures || texture == nullptr) {
        return;
    }
    g_pending.textures |= static_cast<u8>(1u << slot);
    g_pending.texture[slot] = Address(sceGxmTextureGetData(texture));
    g_pending.texture_size[slot] =
        (sceGxmTextureGetWidth(texture) << 16) | (sceGxmTextureGetHeight(texture) & 0xFFFF);
    g_pending.texture_format[slot] = static_cast<u32>(sceGxmTextureGetFormat(texture));
}

void Record(const DrawInfo& info) {
    Box* const box = g_box.load(std::memory_order_relaxed);
    if (box == nullptr) {
        g_pending = Draw{};
        return;
    }
    if (!g_scene_open) {
        // Un camino sin enganchar: se apunta igual, en una escena "sin apuntar".
        OpenScene(SceneKind::Unknown, 0);
    }
    Scene& s = *CurrentScene(*box);
    Draw& d = s.draw[s.draws % kDraws];
    d = g_pending;
    d.kind = static_cast<u8>(info.kind);
    d.primitive = info.primitive;
    d.streams = info.streams;
    d.seq = box->draw_seq++;
    d.count = info.count;
    d.range = info.range;
    d.indices = Address(info.indices);
    d.vertex_program = Address(info.vertex_program);
    d.fragment_program = Address(info.fragment_program);
    for (u32 i = 0; i < 3; i++) {
        d.stream[i] = Address(info.stream[i]);
        d.stream_bytes[i] = info.stream_bytes[i];
    }
    d.bad = info.bad;
    s.draws++;
    g_pending = Draw{};
}

} // namespace Gxm::BlackBox
