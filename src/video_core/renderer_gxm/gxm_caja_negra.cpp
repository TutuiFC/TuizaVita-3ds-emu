// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "video_core/renderer_gxm/gxm_caja_negra.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <psp2/gxm.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
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

/*
 * EL CRONOMETRO DEL CHIP (0.3.7.9). En Zafiro el hilo de la GPU espera ~10 ms
 * por vblank a la valla de HACE DOS FOTOGRAMAS: el SGX va saturado y es el
 * muro. Antes de adelgazar shaders hay que saber que se lleva el tiempo del
 * chip, y eso no lo mide nadie. Esto si: el hilo que dibuja apunta cada escena
 * al cerrarla (valla, hora, cuanto trabajo de vertices lleva) y un hilo propio
 * mira la valla cada ~250 us. El tiempo de chip de una escena va desde que se
 * pudo empezar (cerrada y la anterior ya hecha) hasta que se vio hecha. Las de
 * vita2d (la presentacion) no llevan valla: su tiempo cae en la escena
 * siguiente. Cada 10 s, una linea "gpu chip" en crash.txt con un ajuste lineal
 * del tiempo de las escenas de superficie frente a su trabajo de vertices
 * (indices x bytes del programa): si explica casi todo, el chip va atado a los
 * shaders de vertices. Y las ultimas 2048 escenas, a gpu_escenas.csv.
 * Se apaga con ux0:/data/azahar/sin_cronometro_gpu.txt.
 */
struct Work {
    u32 draws = 0;
    u32 vgpu_indices = 0;
    u32 vcpu_vertices = 0;
    unsigned long long vs_work = 0; ///< Suma de indices x bytes del programa de vertices.
    u32 fs_bytes = 0;               ///< Suma de los bytes de programa de fragmentos.
};
struct Submitted {
    u32 fence;
    u32 kind;
    Work work;
    unsigned long long submit_us;
};
struct Sample {
    u32 seq;
    u32 kind;
    u32 draws;
    u32 vgpu_indices;
    u32 vcpu_vertices;
    u32 vs_work_kb;
    u32 fs_bytes;
    u32 chip_us;
    u32 latency_us;
};
constexpr u32 kTimed = 1024;
constexpr u32 kSamples = 2048;
/// Por encima de los hilos de trabajo: tiene que despertar a tiempo, y en
/// cada vuelta solo lee la valla y unos contadores.
constexpr int kTimerPriority = 128;
constexpr const char* kTimerOffPath = "ux0:/data/azahar/sin_cronometro_gpu.txt";
constexpr const char* kCsvPath = "ux0:/data/azahar/gpu_escenas.csv";

bool g_timer_on = false;
Work g_work{}; ///< La escena abierta; solo la toca el hilo que dibuja.
Submitted g_timed[kTimed];
std::atomic<u32> g_timed_head{0};
/// Dos buffers de muestras: el cronometro llena uno y la caja escribe el otro.
Sample g_samples[2][kSamples];
std::atomic<int> g_samples_ready{-1}; ///< El lleno que espera a escribirse, o -1.

u32 ProgramBytes(const void* program, bool vertex) {
    if (program == nullptr) {
        return 0;
    }
    const SceGxmProgram* gxp =
        vertex ? sceGxmVertexProgramGetProgram(static_cast<const SceGxmVertexProgram*>(program))
               : sceGxmFragmentProgramGetProgram(
                     static_cast<const SceGxmFragmentProgram*>(program));
    return gxp != nullptr ? sceGxmProgramGetSize(gxp) : 0;
}

/// Por tipo de escena, en una ventana.
struct KindStats {
    u32 scenes = 0;
    unsigned long long chip_us = 0;
    u32 max_us = 0;
};
struct Window {
    KindStats kind[4];
    u32 lost = 0;
    // Superficies: lo que llevan y el ajuste lineal tiempo ~ trabajo de vertices.
    unsigned long long vgpu_indices = 0;
    unsigned long long vcpu_vertices = 0;
    unsigned long long vs_work_kb = 0;
    unsigned long long fs_bytes = 0;
    double n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0, syy = 0;
};

void NoteWindow(const Window& w, double seconds) {
    unsigned long long busy = 0;
    u32 scenes = 0;
    for (const KindStats& k : w.kind) {
        busy += k.chip_us;
        scenes += k.scenes;
    }
    const auto avg = [](const KindStats& k) {
        return k.scenes != 0 ? static_cast<double>(k.chip_us) / k.scenes : 0.0;
    };
    const KindStats& draw = w.kind[static_cast<u32>(SceneKind::Draw)];
    const KindStats& blit = w.kind[static_cast<u32>(SceneKind::Blit)];
    const KindStats& clear = w.kind[static_cast<u32>(SceneKind::Clear)];
    // Ajuste lineal de las de superficie: chip_us = a + b * kb_de_trabajo_vs.
    double slope = 0, intercept = 0, r2 = 0;
    const double den = w.n * w.sxx - w.sx * w.sx;
    if (w.n >= 3 && den > 0) {
        const double cov = w.n * w.sxy - w.sx * w.sy;
        slope = cov / den;
        intercept = (w.sy - slope * w.sx) / w.n;
        const double vy = w.n * w.syy - w.sy * w.sy;
        r2 = vy > 0 ? cov * cov / (den * vy) : 0;
    }
    const double per = draw.scenes != 0 ? static_cast<double>(draw.scenes) : 1.0;
    char line[512];
    std::snprintf(line, sizeof(line),
                  "ocupado %.0f ms/s (%.0f %%), %.0f escenas/s (%u perdidas) | superficie "
                  "%.0f/s media %.0f us max %u | copia %.0f/s media %.0f us | borrado %.0f/s "
                  "media %.0f us | por superficie: %.0f indices vs-gpu, %.0f KB de trabajo vs, "
                  "%.0f vertices vs-cpu, %.1f KB de fs | ajuste: %.0f us + %.2f us por KB vs, "
                  "r2 %.2f",
                  busy / 1000.0 / seconds, busy / 10000.0 / seconds, scenes / seconds, w.lost,
                  draw.scenes / seconds, avg(draw), draw.max_us, blit.scenes / seconds,
                  avg(blit), clear.scenes / seconds, avg(clear), w.vgpu_indices / per,
                  w.vs_work_kb / per, w.vcpu_vertices / per, w.fs_bytes / per / 1024.0,
                  intercept, slope, r2);
    Common::VitaNote("gpu chip", line);
}

void* TimerMain(void*) {
    sceKernelChangeThreadPriority(sceKernelGetThreadId(), kTimerPriority);
    u32 tail = 0;
    u32 seq = 0;
    unsigned long long last_done_us = 0;
    unsigned long long window_start = Common::VitaMicros();
    Window w{};
    int fill = 0;
    u32 filled = 0;
    while (true) {
        sceKernelDelayThread(250);
        const u32 done = g_fence != nullptr ? *g_fence : 0;
        const unsigned long long now = Common::VitaMicros();
        const u32 head = g_timed_head.load(std::memory_order_acquire);
        if (head - tail > kTimed) {
            w.lost += head - tail - kTimed / 2;
            tail = head - kTimed / 2;
        }
        while (tail != head) {
            const Submitted& sub = g_timed[tail % kTimed];
            if (static_cast<s32>(sub.fence - done) > 0) {
                break;
            }
            const unsigned long long start = std::max(sub.submit_us, last_done_us);
            const u32 chip = now > start ? static_cast<u32>(now - start) : 0;
            last_done_us = now;
            KindStats& k = w.kind[sub.kind & 3u];
            k.scenes++;
            k.chip_us += chip;
            k.max_us = std::max(k.max_us, chip);
            const u32 vs_kb = static_cast<u32>(sub.work.vs_work / 1024);
            if (sub.kind == static_cast<u32>(SceneKind::Draw)) {
                w.vgpu_indices += sub.work.vgpu_indices;
                w.vcpu_vertices += sub.work.vcpu_vertices;
                w.vs_work_kb += vs_kb;
                w.fs_bytes += sub.work.fs_bytes;
                const double x = vs_kb;
                const double y = chip;
                w.n += 1;
                w.sx += x;
                w.sy += y;
                w.sxx += x * x;
                w.sxy += x * y;
                w.syy += y * y;
            }
            Sample& out = g_samples[fill][filled];
            out.seq = seq++;
            out.kind = sub.kind;
            out.draws = sub.work.draws;
            out.vgpu_indices = sub.work.vgpu_indices;
            out.vcpu_vertices = sub.work.vcpu_vertices;
            out.vs_work_kb = vs_kb;
            out.fs_bytes = sub.work.fs_bytes;
            out.chip_us = chip;
            out.latency_us = now > sub.submit_us ? static_cast<u32>(now - sub.submit_us) : 0;
            if (++filled == kSamples) {
                // Lleno: a escribir, si el anterior ya se escribio.
                if (g_samples_ready.load(std::memory_order_acquire) < 0) {
                    g_samples_ready.store(fill, std::memory_order_release);
                    fill ^= 1;
                }
                filled = 0;
            }
            tail++;
        }
        if (now - window_start >= 10'000'000ull) {
            NoteWindow(w, static_cast<double>(now - window_start) / 1e6);
            w = Window{};
            window_start = now;
        }
    }
    return nullptr;
}

/// Desde el hilo de la caja (cada 100 ms): si hay un buffer lleno, a la tarjeta.
void WriteSamplesIfReady() {
    const int ready = g_samples_ready.load(std::memory_order_acquire);
    if (ready < 0) {
        return;
    }
    static char text[kSamples * 64 + 128];
    std::size_t n = static_cast<std::size_t>(std::snprintf(
        text, sizeof(text),
        "seq,tipo,dibujados,indices_vs_gpu,vertices_vs_cpu,kb_trabajo_vs,bytes_fs,chip_us,"
        "latencia_us\n"));
    for (u32 i = 0; i < kSamples && n + 96 < sizeof(text); i++) {
        const Sample& s = g_samples[ready][i];
        n += static_cast<std::size_t>(std::snprintf(text + n, sizeof(text) - n,
                                                    "%u,%u,%u,%u,%u,%u,%u,%u,%u\n", s.seq,
                                                    s.kind, s.draws, s.vgpu_indices,
                                                    s.vcpu_vertices, s.vs_work_kb, s.fs_bytes,
                                                    s.chip_us, s.latency_us));
    }
    const SceUID fd = sceIoOpen(kCsvPath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, text, static_cast<SceSize>(n));
        sceIoClose(fd);
    }
    g_samples_ready.store(-1, std::memory_order_release);
}

/*
 * QUE PROGRAMAS DE VERTICES SE LLEVAN EL CHIP (0.3.8.1). El cronometro de
 * 0.3.7.9 dijo que en el mapa de Zafiro el tiempo de chip de cada escena es
 * casi exactamente proporcional a indices x bytes del programa de vertices
 * (r2 0,97-0,99, casi nada fijo): el SGX va atado a nuestros shaders de
 * vertices. Para saber cuales adelgazar primero se suma, por programa, lo que
 * dibuja, y cada 30 s se escribe el ranking en vs_trabajo.txt con el hash de
 * su fuente, que es el nombre de su .gxp y de su .cg en vs_fuentes (si existe
 * volcar_vs.txt, ver el rasterizador).
 */
struct ProgramWork {
    u32 program; ///< SceGxmVertexProgram*; 0 = libre.
    u32 bytes;
    u64 hash;
    u64 indices;
    u32 draws;
};
constexpr u32 kPrograms = 512;
ProgramWork g_programs[kPrograms];
constexpr const char* kRankingPath = "ux0:/data/azahar/vs_trabajo.txt";

ProgramWork* ProgramSlot(u32 program) {
    const u32 start = (program >> 4) * 2654435761u;
    for (u32 i = 0; i < kPrograms; i++) {
        ProgramWork& slot = g_programs[(start + i) & (kPrograms - 1)];
        if (slot.program == program) {
            return &slot;
        }
        if (slot.program == 0) {
            slot.program = program;
            return &slot;
        }
    }
    return nullptr;
}

/// Desde el hilo de la caja: copia, ordena y escribe. Las cuentas se leen sin
/// cerrojo mientras el hilo que dibuja suma: para un ranking da igual.
void WriteRanking() {
    static ProgramWork copy[kPrograms];
    u32 n = 0;
    unsigned long long total = 0;
    for (const ProgramWork& slot : g_programs) {
        if (slot.program != 0 && slot.indices != 0) {
            copy[n++] = slot;
            total += slot.indices * slot.bytes;
        }
    }
    if (n == 0 || total == 0) {
        return;
    }
    std::sort(copy, copy + n, [](const ProgramWork& a, const ProgramWork& b) {
        return a.indices * a.bytes > b.indices * b.bytes;
    });
    static char text[48 * 1024];
    std::size_t len = static_cast<std::size_t>(std::snprintf(
        text, sizeof(text),
        "programas de vertices por trabajo en el chip (indices x bytes) desde el arranque: %u\n"
        "puesto,hash_fuente,bytes_gxp,indices,dibujados,trabajo_mb,porcentaje,acumulado\n",
        n));
    double acc = 0;
    for (u32 i = 0; i < n && i < 60 && len + 160 < sizeof(text); i++) {
        const ProgramWork& p = copy[i];
        const double work = static_cast<double>(p.indices) * p.bytes;
        acc += work;
        len += static_cast<std::size_t>(std::snprintf(
            text + len, sizeof(text) - len, "%u,%016llx,%u,%llu,%u,%.1f,%.1f,%.1f\n", i + 1,
            static_cast<unsigned long long>(p.hash), p.bytes,
            static_cast<unsigned long long>(p.indices), p.draws, work / 1048576.0,
            100.0 * work / total, 100.0 * acc / total));
    }
    const SceUID fd = sceIoOpen(kRankingPath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, text, static_cast<SceSize>(len));
        sceIoClose(fd);
    }
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
    u32 ticks = 0;
    while (true) {
        sceKernelDelayThread(100 * 1000);
        WriteSamplesIfReady();
        if (g_timer_on && ++ticks % 300 == 0) {
            WriteRanking();
        }
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
    SceIoStat off{};
    if (sceIoGetstat(kTimerOffPath, &off) < 0) {
        pthread_attr_t timer_attr;
        pthread_attr_init(&timer_attr);
        pthread_attr_setstacksize(&timer_attr, 64 * 1024);
        pthread_t timer;
        if (pthread_create(&timer, &timer_attr, &TimerMain, nullptr) == 0) {
            pthread_detach(timer);
            g_timer_on = true;
        }
        pthread_attr_destroy(&timer_attr);
    }
    Common::VitaNote("gpu chip", g_timer_on
                                       ? "cronometro del chip en marcha (lineas cada 10 s)"
                                       : "cronometro del chip apagado");
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
    g_work = Work{};
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
    if (g_timer_on) {
        const u32 head = g_timed_head.load(std::memory_order_relaxed);
        Submitted& sub = g_timed[head % kTimed];
        sub.fence = fence;
        sub.kind = g_scene_open ? CurrentScene(*box)->kind : 0u;
        sub.work = g_work;
        sub.submit_us = Common::VitaMicros();
        g_timed_head.store(head + 1, std::memory_order_release);
    }
    g_scene_open = false;
}

void NoteVertexProgram(const void* program, u64 source_hash, u32 bytes) {
    if (!g_timer_on || program == nullptr) {
        return;
    }
    if (ProgramWork* slot = ProgramSlot(Address(program))) {
        slot->hash = source_hash;
        slot->bytes = bytes;
    }
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
    if (g_timer_on) {
        g_work.draws++;
        if (info.kind == DrawKind::VertexGpu) {
            g_work.vgpu_indices += info.count;
            const u32 vs_bytes = ProgramBytes(info.vertex_program, true);
            g_work.vs_work += static_cast<unsigned long long>(info.count) * vs_bytes;
            if (ProgramWork* slot = ProgramSlot(Address(info.vertex_program))) {
                slot->indices += info.count;
                slot->draws++;
                slot->bytes = vs_bytes;
            }
        } else if (info.kind == DrawKind::VertexCpu) {
            g_work.vcpu_vertices += info.count;
        }
        g_work.fs_bytes += ProgramBytes(info.fragment_program, false);
    }
    g_pending = Draw{};
}

} // namespace Gxm::BlackBox
