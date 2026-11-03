// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "video_core/renderer_gxm/gxm_cg.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <malloc.h>
#include <mutex>
#include <pthread.h>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <fmt/format.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/rtc.h>
#include "common/common_types.h"
#include "common/hash.h"
#include "common/logging/log.h"
#include "common/vita_diag.h"
#include "video_core/shader/generator/cg_vs_shader_gen.h"

/// Tamano del heap de libc, definido en citra_vita/main.cpp (AZAHAR_HEAP_MB).
extern unsigned int _newlib_heap_size_user;

namespace Gxm {

/// Ver g_vs_o3 (0.4.3.7): esta fuente de vertices se compila a O1 desde ahora.
void AddO3Bad(u64 hash_city);

namespace {

constexpr const char* kShacccgPaths[] = {
    "ur0:/data/libshacccg.suprx",
    "ux0:/data/libshacccg.suprx",
};

SceUID g_module = -1;
/// Compilaciones hechas con el modulo cargado ahora (0.1.8.5, ver StoreBadSource).
u32 g_compiles_since_load = 0;
SceShaccCgSourceFile g_source{};
SceShaccCgCallbackList g_callbacks{};
char g_status[64] = "sin iniciar";
/// El compilador ha devuelto un "internal error" alguna vez. Ver CgPoisoned.
/// Atomico desde 0.1.9.6: lo escribe el hilo de compilacion.
std::atomic<bool> g_poisoned{false};
/**
 * Todo lo del compilador (el modulo, su memoria, la cache, las listas) pasa
 * por aqui (0.1.9.6): desde que hay un hilo de compilacion, CompileCg puede
 * llamarse desde dos hilos. Recursivo porque CompileCg llama a EnsureCgReady
 * y a ReleaseCgOutput.
 */
std::recursive_mutex g_cg_mutex;
unsigned long long g_last_attempt_us = 0;

/// Un intento por segundo como mucho cuando falla: sin esto, cada lote cuyo
/// shader no se pueda compilar paga una carga de modulo fallida, y eso fue una
/// caida de 450 ms a 13.734 ms de fotograma.
constexpr unsigned long long kRetryIntervalUs = 1000000;

/// Las dos fuentes del banco en paralelo (0.4.3.1): cada compilacion la suya.
SceShaccCgSourceFile g_bench_files[2]{};

SceShaccCgSourceFile* OpenSource(const char* name, const SceShaccCgSourceLocation*,
                                 const SceShaccCgCompileOptions*, const char**) {
    for (SceShaccCgSourceFile& file : g_bench_files) {
        if (file.fileName != nullptr && name != nullptr && std::strcmp(file.fileName, name) == 0) {
            return &file;
        }
    }
    return &g_source;
}

/**
 * RECUPERARSE DE UN "fatal internal error" (0.1.5.9).
 *
 * Medido en 0.1.5.8: un shader de vertices dio "fatal internal error", el
 * siguiente de fragmentos dio lo mismo (el compilador se queda roto para
 * siempre), todo el 3D paso a software (0.6 FPS) y poco despues la emulacion
 * murio con bad_alloc: el compilador se rompe a medias y deja SIN DEVOLVER la
 * memoria que habia pedido.
 *
 * Arreglo: se apunta cada bloque que el compilador pide DURANTE una
 * compilacion. Si la compilacion acaba en error interno, se descarga el modulo
 * (su estado global roto se va con el), se devuelve la memoria que dejo
 * perdida y se vuelve a cargar limpio en la compilacion siguiente. Los shaders
 * de fragmentos siguen funcionando; los de vertices traducidos no se vuelven a
 * mandar en esta sesion (CgPoisoned), porque el que lo rompio volveria a salir.
 *
 * La memoria SOLO se devuelve si la descarga sale bien: si el modulo siguiera
 * cargado, su estado podria apuntar a esos bloques.
 */
bool g_tracking = false;
std::atomic<u32> g_generation{0};

/**
 * TODA LA MEMORIA VIVA DEL COMPILADOR (0.1.8.1).
 *
 * Hasta 0.1.8.0 solo se apuntaba lo que el compilador pedia DURANTE una
 * compilacion, y se olvidaba al acabar bien. crash.txt de 0.1.8.0: una
 * compilacion lleva el heap de 171 a 189 MB, y sceShaccCgReleaseCompiler no
 * devuelve NADA (189,7 MB despues). Esos 18 MB se quedan en el estado del
 * modulo; con la siguiente compilacion grande el heap no da y el compilador
 * muere por dentro (volcado de 0.1.8.0: PC dentro de libshacccg).
 *
 * Ahora se apunta todo lo que pide mientras esta cargado, y se puede
 * DESCARGAR el modulo. Desde 0.1.8.5 solo se mide (picos y lo retenido, ver
 * NoteCompileMemory): liberar lo apuntado tras descargar resulto peligroso.
 */
std::size_t g_live_bytes = 0;
/// Lo maximo que ha tenido vivo durante la compilacion en curso.
std::size_t g_peak_bytes = 0;

/**
 * PRESUPUESTO DE UNA COMPILACION (0.1.8.0, rebajado en 0.1.8.1).
 *
 * Volcado de 0.1.7.9: el heap se acababa DENTRO de sceShaccCgCompileProgram y
 * el bad_alloc del insert de aqui no podia atravesar libshacccg (es C):
 * std::unexpected y abort(). CgAlloc no lanza nunca. El tope ya no es la via
 * normal de salir de un apuro (un NULL a mitad de compilacion probablemente lo
 * tumba igual): lo que evita el apuro es no EMPEZAR a compilar sin memoria de
 * sobra (kMinFreeToCompile). El tope solo deja kEmulatorReserve para que la
 * emulacion no muera por un bad_alloc suyo si el compilador se desboca.
 */
constexpr std::size_t kEmulatorReserve = 8u * 1024u * 1024u;
std::size_t g_compile_bytes = 0;
std::size_t g_compile_budget = static_cast<std::size_t>(-1);
bool g_compile_over_budget = false;

/**
 * MEMORIA A CERO Y LIBERACION DIFERIDA (0.2.3.7). Las caidas del compilador
 * con volcado, todas, de 0.2.0.x a 0.2.3.5 (los 17 que hay en ux0:data), son
 * en el MISMO sitio: SceShaccCg+0x65ea8, "ldr r0, [r0, #4]" de una funcion
 * que lee el tipo de un nodo, llamada justo despues de un "si el nodo no es
 * NULL" (SceShaccCg+0x7dc26). El nodo vale 0x18, 1 o 0x72646461 ("addr",
 * texto de un shader): un campo que nunca se escribio, o un bloque ya
 * liberado y reutilizado. Con el reservador de Sony en un proceso recien
 * arrancado esa memoria sale de paginas nuevas, a cero, y el campo vale NULL;
 * con el heap del emulador, lleno de restos, vale basura. Por eso cada bloque
 * sale a cero, y lo liberado espera en una cola (hasta kQuarantineBytes)
 * antes de volver al heap, para que un acceso tardio lea lo que habia.
 */
// 2 MB desde 0.3.0.1 (eran 8): en el 3D de Zafiro Alfa el heap se quedo sin
// sitio ("bad_alloc" con el compilador cayendose) y esto eran 6 MB de mas.
constexpr std::size_t kQuarantineBytes = 2u * 1024u * 1024u;
/// Reservas de la compilacion en curso: cuantas y cuanto (ver CompileCg).
u32 g_alloc_calls = 0;
u64 g_alloc_total = 0;
/// Las que salieron de g_small_free, sin pasar por newlib.
u32 g_alloc_reused = 0;

/**
 * BLOQUES PEQUENOS SIN PASAR POR NEWLIB (0.3.2.6). Un shader de vertices de
 * Pokemon Sol son ~700.000 reservas y otras tantas liberaciones (memalign, que
 * por dentro es un malloc y hasta dos free), y el malloc de newlib tiene UN
 * cerrojo para todo el proceso, sin herencia de prioridad: el compilador es el
 * hilo de menos prioridad del nucleo 2, y si le quitan la CPU con el cerrojo
 * cogido, el hilo de emulacion y el de la GPU esperan a que vuelva. Durante
 * una compilacion, lo que sale de la cuarentena va a una lista por tamano (de
 * 16 en 16 bytes, hasta kSmallBytes, como mucho kSmallFreeBytes en total) y
 * las reservas de ese tamano salen de ahi. Al acabar la compilacion se
 * devuelve todo a newlib. Todo esto, como el resto, con g_cg_mutex cogido.
 */
// 1 KB y 2 MB desde 0.3.2.9: con 512 bytes y 1 MB solo salian de la lista el
// 33-40 % de las reservas ("sin newlib" en crash.txt de 0.3.2.6).
constexpr std::size_t kSmallBytes = 1024;
constexpr std::size_t kSmallFreeBytes = 2u * 1024u * 1024u;
std::array<void*, kSmallBytes / 16> g_small_free{};
std::size_t g_small_free_bytes = 0;

bool ArenaRelease(void* pointer);

/// Un bloque que ya no usa nadie: a su lista si cabe, si no a newlib.
void ReleaseBlock(void* pointer, std::size_t usable) {
    if (ArenaRelease(pointer)) {
        return;
    }
    const std::size_t slot = usable / 16;
    if (g_tracking && slot >= 1 && slot <= g_small_free.size() &&
        g_small_free_bytes + usable <= kSmallFreeBytes) {
        // La lista va dentro de los propios bloques (al menos 16 bytes).
        *static_cast<void**>(pointer) = g_small_free[slot - 1];
        g_small_free[slot - 1] = pointer;
        g_small_free_bytes += usable;
        return;
    }
    std::free(pointer);
}

/// Devuelve a newlib todo lo de g_small_free.
void FlushSmallFree() {
    for (void*& head : g_small_free) {
        while (head != nullptr) {
            void* const next = *static_cast<void**>(head);
            std::free(head);
            head = next;
        }
    }
    g_small_free_bytes = 0;
}

/**
 * LA CUENTA SIN UN CONJUNTO DE PUNTEROS, Y LA COLA EN UN ANILLO (0.3.1.4). Cada
 * reserva del compilador entraba en un unordered_set (un malloc de nodo, y
 * rehacer la tabla al crecer) y cada liberacion en una deque: el doble de
 * llamadas a malloc que las del propio compilador, que son millones por shader
 * de vertices grande (1,7 millones y 35 s de CPU uno de 12 KB de Pokemon Sol en
 * crash.txt de 0.3.1.2). El conjunto solo servia para no descontar bloques de
 * otro modulo, y el modulo descargado no vuelve a liberar nada: la cuenta se
 * lleva con lo que dice malloc_usable_size. El anillo guarda lo mismo que la
 * deque, con tope de entradas ademas del de bytes.
 */
constexpr std::size_t kQuarantineSlots = 32768;
std::vector<std::pair<void*, std::size_t>> g_quarantine;
std::size_t g_quarantine_head = 0;
std::size_t g_quarantine_count = 0;
std::size_t g_quarantine_bytes = 0;

/**
 * EL RESERVADOR PROPIO DEL COMPILADOR (0.4.2.3). Con el cronometro de 0.4.2.2
 * (una de cada 64 reservas), en Kirby's Extra Epic Yarn con la cache vacia
 * (29-10) reservar y liberar se llevaban ~143 s de los ~268 s que paso
 * compilando: memalign y free de newlib, con el heap del emulador lleno de
 * restos y su cerrojo de todo el proceso (si otro hilo lo tiene, el
 * compilador espera). El banco, sin nadie mas, compila el mismo shader 2-4
 * veces mas rapido.
 *
 * Ahora los bloques de hasta kArenaMaxBlock salen de trozos de 2 MB pedidos a
 * newlib de una vez: listas por tamano (de 16 en 16 bytes hasta 1 KB, y cuatro
 * por cada potencia de dos hasta 256 KB) y, si la lista esta vacia, lo
 * siguiente del trozo. Sin cerrojo propio: todo lo del compilador va con
 * g_cg_mutex. Lo de siempre se mantiene: cada bloque sale a cero, alineado a
 * 16, y lo liberado pasa por la cuarentena antes de volver a su lista. Los
 * mas grandes, o si ya hay kArenaMaxChunks trozos, por newlib como antes.
 * Cuando el compilador se queda sin trabajo (CgArenaTrim), los trozos sin
 * ningun bloque vivo vuelven a newlib.
 */
constexpr std::size_t kArenaChunkBytes = 2u * 1024u * 1024u;
constexpr u32 kArenaMaxChunks = 16;
constexpr std::size_t kArenaMaxBlock = 256u * 1024u;
constexpr u32 kArenaSmallClasses = 64;
constexpr u32 kArenaClasses = kArenaSmallClasses + 4 * 8;
constexpr u32 kArenaMagic = 0x4E524341u; // "ACRN"

/// Delante de cada bloque: 16 bytes (el bloque sigue alineado a 16).
struct ArenaHeader {
    u32 magic;
    u32 klass;
    u32 bytes;
    u32 pad;
};
static_assert(sizeof(ArenaHeader) == 16);

struct ArenaChunk {
    u8* base = nullptr;
    std::size_t used = 0;
    /// Bloques que no estan en una lista (vivos o en la cuarentena).
    u32 live = 0;
};
std::array<ArenaChunk, kArenaMaxChunks> g_arena{};
u32 g_arena_chunks = 0;
std::array<void*, kArenaClasses> g_arena_free{};
/// Se apaga con ux0:/data/azahar/sin_reservador.txt (para comparar).
bool g_arena_on = true;
bool g_arena_checked = false;
/// Trozos pedidos en la compilacion en curso (para la nota).
u32 g_arena_new_chunks = 0;

u32 ArenaClass(std::size_t size) {
    if (size <= 1024) {
        return size == 0 ? 0 : static_cast<u32>((size + 15) / 16 - 1);
    }
    const u32 exponent = 31u - static_cast<u32>(__builtin_clz(static_cast<u32>(size - 1)));
    const std::size_t base = std::size_t{1} << exponent;
    const std::size_t step = base / 4;
    const u32 index = static_cast<u32>((size - base + step - 1) / step);
    return kArenaSmallClasses + (exponent - 10) * 4 + index - 1;
}

std::size_t ArenaClassBytes(u32 klass) {
    if (klass < kArenaSmallClasses) {
        return (static_cast<std::size_t>(klass) + 1) * 16;
    }
    const u32 rest = klass - kArenaSmallClasses;
    const std::size_t base = std::size_t{1024} << (rest / 4);
    return base + (base / 4) * (rest % 4 + 1);
}

/// El trozo que contiene el puntero, o -1 si no es del reservador propio.
int ArenaChunkOf(const void* pointer) {
    const u8* const at = static_cast<const u8*>(pointer);
    for (u32 i = 0; i < g_arena_chunks; i++) {
        if (at >= g_arena[i].base && at < g_arena[i].base + kArenaChunkBytes) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

/// Un bloque del reservador propio, o nullptr (entonces, newlib como antes).
void* ArenaAlloc(std::size_t size, std::size_t& bytes) {
    if (!g_arena_checked) {
        g_arena_checked = true;
        SceIoStat off{};
        g_arena_on = sceIoGetstat("ux0:/data/azahar/sin_reservador.txt", &off) < 0;
        Common::VitaNote("gxm compila", g_arena_on ? "reservador propio del compilador: si"
                                                   : "reservador propio: no (sin_reservador.txt)");
    }
    if (!g_arena_on || size > kArenaMaxBlock) {
        return nullptr;
    }
    const u32 klass = ArenaClass(size);
    bytes = ArenaClassBytes(klass);
    if (void* const head = g_arena_free[klass]) {
        g_arena_free[klass] = *static_cast<void**>(head);
        const int chunk = ArenaChunkOf(head);
        if (chunk >= 0) {
            g_arena[chunk].live++;
        }
        return head;
    }
    const std::size_t need = sizeof(ArenaHeader) + bytes;
    ArenaChunk* chunk = g_arena_chunks != 0 ? &g_arena[g_arena_chunks - 1] : nullptr;
    if (chunk == nullptr || chunk->used + need > kArenaChunkBytes) {
        if (g_arena_chunks == kArenaMaxChunks) {
            return nullptr;
        }
        // Sin quitarle a la emulacion lo que necesita (ver kEmulatorReserve).
        const std::size_t in_use = mallinfo().uordblks;
        if (in_use + kArenaChunkBytes + kEmulatorReserve > _newlib_heap_size_user) {
            return nullptr;
        }
        void* const base = memalign(64, kArenaChunkBytes);
        if (base == nullptr) {
            return nullptr;
        }
        chunk = &g_arena[g_arena_chunks++];
        chunk->base = static_cast<u8*>(base);
        chunk->used = 0;
        chunk->live = 0;
        g_arena_new_chunks++;
    }
    auto* const header = reinterpret_cast<ArenaHeader*>(chunk->base + chunk->used);
    header->magic = kArenaMagic;
    header->klass = klass;
    header->bytes = static_cast<u32>(bytes);
    header->pad = 0;
    chunk->used += need;
    chunk->live++;
    return reinterpret_cast<u8*>(header) + sizeof(ArenaHeader);
}

/// Lo que mide un bloque del reservador propio (0 si no lo es).
std::size_t ArenaBytes(const void* pointer) {
    if (ArenaChunkOf(pointer) < 0) {
        return 0;
    }
    const auto* header =
        reinterpret_cast<const ArenaHeader*>(static_cast<const u8*>(pointer) - sizeof(ArenaHeader));
    return header->magic == kArenaMagic ? header->bytes : 0;
}

/// Un bloque del reservador propio que sale de la cuarentena: a su lista.
bool ArenaRelease(void* pointer) {
    const int chunk = ArenaChunkOf(pointer);
    if (chunk < 0) {
        return false;
    }
    const auto* header =
        reinterpret_cast<const ArenaHeader*>(static_cast<u8*>(pointer) - sizeof(ArenaHeader));
    if (header->magic != kArenaMagic || header->klass >= kArenaClasses) {
        // No deberia pasar nunca: se pierde el bloque antes que estropear una lista.
        static bool noted = false;
        if (!noted) {
            noted = true;
            Common::VitaNote("gxm compila", "reservador propio: bloque con la cabecera rota");
        }
        return true;
    }
    *static_cast<void**>(pointer) = g_arena_free[header->klass];
    g_arena_free[header->klass] = pointer;
    if (g_arena[chunk].live != 0) {
        g_arena[chunk].live--;
    }
    return true;
}

void* CgAllocInner(unsigned int size) {
    /**
     * Sin NULL por presupuesto (0.2.0.8). libshacccg no comprueba lo que le
     * devuelve el reservador: un NULL a mitad de compilacion lo tumbaba con un
     * acceso a una direccion casi nula (volcados de 0.2.0.5 a 0.2.0.7, Pokemon
     * Sol al pasar al 3D). Pasado el presupuesto solo se apunta; si la memoria
     * se acaba de verdad, el NULL es inevitable.
     */
    if (g_tracking && g_compile_bytes + size > g_compile_budget) {
        g_compile_over_budget = true;
    }
    /**
     * Alineado y redondeado a 16 (0.2.0.7). malloc de newlib da 8, y el
     * compilador de Sony se caia SIEMPRE en el mismo punto, justo despues de
     * pedir memoria aqui (volcados de 0.2.0.1, 0.2.0.5 y 0.2.0.6: acceso a
     * memoria invalido dentro de libshacccg con la pila pasando por CgAlloc),
     * lo que encaja con accesos NEON de 16 bytes que exigen esa alineacion.
     */
    const std::size_t rounded = (size + 15u) & ~15u;
    // Un bloque de la lista 'slot' tiene al menos slot * 16 bytes utiles.
    const std::size_t slot = rounded / 16;
    void* pointer = nullptr;
    std::size_t arena_bytes = 0;
    // Ver EL RESERVADOR PROPIO DEL COMPILADOR.
    if (void* const own = ArenaAlloc(rounded, arena_bytes)) {
        std::memset(own, 0, rounded);
        if (g_tracking) {
            g_alloc_calls++;
            g_alloc_total += rounded;
            g_alloc_reused++;
            g_compile_bytes += arena_bytes;
        }
        g_live_bytes += arena_bytes;
        if (g_live_bytes > g_peak_bytes) {
            g_peak_bytes = g_live_bytes;
        }
        return own;
    }
    if (slot >= 1 && slot <= g_small_free.size() && g_small_free[slot - 1] != nullptr) {
        pointer = g_small_free[slot - 1];
        g_small_free[slot - 1] = *static_cast<void**>(pointer);
        g_small_free_bytes -= malloc_usable_size(pointer);
        g_alloc_reused++;
    } else {
        pointer = memalign(16, rounded);
        if (pointer == nullptr) {
            return nullptr;
        }
    }
    std::memset(pointer, 0, rounded);
    if (g_tracking) {
        g_alloc_calls++;
        g_alloc_total += rounded;
    }
    const std::size_t usable = malloc_usable_size(pointer);
    g_live_bytes += usable;
    if (g_live_bytes > g_peak_bytes) {
        g_peak_bytes = g_live_bytes;
    }
    if (g_tracking) {
        g_compile_bytes += usable;
    }
    return pointer;
}

void CgFreeInner(void* pointer) {
    if (pointer == nullptr) {
        return;
    }
    const std::size_t own_bytes = ArenaBytes(pointer);
    const std::size_t size = own_bytes != 0 ? own_bytes : malloc_usable_size(pointer);
    g_live_bytes = g_live_bytes > size ? g_live_bytes - size : 0;
    if (g_tracking) {
        g_compile_bytes = g_compile_bytes > size ? g_compile_bytes - size : 0;
    }
    if (g_quarantine.empty()) {
        try {
            g_quarantine.resize(kQuarantineSlots);
        } catch (...) {
            ReleaseBlock(pointer, size);
            return;
        }
    }
    const auto free_oldest = [] {
        auto& oldest = g_quarantine[g_quarantine_head];
        ReleaseBlock(oldest.first, oldest.second);
        g_quarantine_bytes -= oldest.second;
        oldest = {};
        g_quarantine_head = (g_quarantine_head + 1) % kQuarantineSlots;
        g_quarantine_count--;
    };
    if (g_quarantine_count == kQuarantineSlots) {
        free_oldest();
    }
    g_quarantine[(g_quarantine_head + g_quarantine_count) % kQuarantineSlots] = {pointer, size};
    g_quarantine_count++;
    g_quarantine_bytes += size;
    while (g_quarantine_bytes > kQuarantineBytes && g_quarantine_count > 1) {
        free_oldest();
    }
}

/**
 * CUANTO SE LLEVA EL RESERVADOR (0.4.2.2). El banco del compilador da, para el
 * MISMO shader, entre 2 y 4 veces menos CPU que dentro del juego (un shader de
 * fragmentos de 8 KB: 1,2 s en el banco, 2-4 s jugando a Luigi's Mansion). Una
 * sospecha es el malloc de newlib con el heap del emulador lleno de restos y su
 * cerrojo compartido. Para saberlo sin pagar dos lecturas del reloj por reserva
 * (son cientos de miles por shader), se cronometra una de cada 64 y se
 * multiplica: sale como "reservar" y "liberar" en la nota de "compilado en".
 */
u32 g_free_calls = 0;
unsigned long long g_alloc_sample_us = 0;
unsigned long long g_free_sample_us = 0;

/**
 * Los trozos del reservador propio sin ningun bloque vivo, de vuelta a newlib
 * (0.4.2.3). Con g_cg_mutex cogido y el compilador sin compilar: primero se
 * vacia la cuarentena (sus bloques pasan a las listas), despues se quitan de
 * las listas los bloques de los trozos que se devuelven.
 */
void ArenaTrimLocked() {
    while (g_quarantine_count != 0) {
        auto& oldest = g_quarantine[g_quarantine_head];
        ReleaseBlock(oldest.first, oldest.second);
        g_quarantine_bytes -= oldest.second;
        oldest = {};
        g_quarantine_head = (g_quarantine_head + 1) % kQuarantineSlots;
        g_quarantine_count--;
    }
    FlushSmallFree();
    std::array<bool, kArenaMaxChunks> dead{};
    u32 dead_count = 0;
    for (u32 i = 0; i < g_arena_chunks; i++) {
        dead[i] = g_arena[i].live == 0;
        dead_count += dead[i] ? 1u : 0u;
    }
    if (dead_count == 0) {
        return;
    }
    for (void*& head : g_arena_free) {
        void** link = &head;
        while (*link != nullptr) {
            void* const block = *link;
            const int chunk = ArenaChunkOf(block);
            if (chunk >= 0 && dead[chunk]) {
                *link = *static_cast<void**>(block);
            } else {
                link = static_cast<void**>(block);
            }
        }
    }
    u32 kept = 0;
    for (u32 i = 0; i < g_arena_chunks; i++) {
        if (dead[i]) {
            std::free(g_arena[i].base);
        } else {
            g_arena[kept++] = g_arena[i];
        }
    }
    for (u32 i = kept; i < g_arena_chunks; i++) {
        g_arena[i] = ArenaChunk{};
    }
    g_arena_chunks = kept;
}

/// El banco en paralelo (0.4.3.1): el reservador propio no tiene cerrojo.
std::atomic<bool> g_bench_parallel{false};

int ArenaChunkOf(const void* pointer);

void* CgAlloc(unsigned int size) {
    // Banco en paralelo (0.4.3.1): directo a newlib, sin el reservador propio ni
    // la cuarentena (no tienen cerrojo) y sin cerrojo nuestro que lo serialice.
    if (g_bench_parallel.load(std::memory_order_relaxed)) {
        const std::size_t rounded = size == 0 ? 16u : (size + 15u) & ~15u;
        void* const pointer = memalign(16, rounded);
        if (pointer != nullptr) {
            std::memset(pointer, 0, rounded);
        }
        return pointer;
    }
    if (!g_tracking || (g_alloc_calls & 63u) != 0) {
        return CgAllocInner(size);
    }
    const unsigned long long begin = Common::VitaMicros();
    void* const pointer = CgAllocInner(size);
    g_alloc_sample_us += Common::VitaMicros() - begin;
    return pointer;
}

void CgFree(void* pointer) {
    if (g_bench_parallel.load(std::memory_order_relaxed)) {
        // Los del reservador propio de antes del banco se quedan (es el banco).
        if (pointer != nullptr && ArenaChunkOf(pointer) < 0) {
            std::free(pointer);
        }
        return;
    }
    if (!g_tracking || (g_free_calls++ & 63u) != 0) {
        CgFreeInner(pointer);
        return;
    }
    const unsigned long long begin = Common::VitaMicros();
    CgFreeInner(pointer);
    g_free_sample_us += Common::VitaMicros() - begin;
}

/**
 * CACHE DE SHADERS COMPILADOS EN LA TARJETA (0.1.5.8).
 *
 * POR QUE. libshacccg compila en la propia consola y es lento: cada estado
 * nuevo de la GPU del 3DS (combinacion de TEV, luces, formato de vertices...)
 * es un shader nuevo, y al pasar de una pantalla 2D a una escena 3D aparecen
 * muchos de golpe. El usuario lo vio como una carga "realmente lenta" entre el
 * menu 2D y el 3D de Pokemon Zafiro Alfa.
 *
 * COMO. El binario GXP que devuelve el compilador se guarda en
 * ux0:/data/azahar/shadercache/<hash>_<perfil>.gxp, y la proxima vez que se
 * pida el MISMO codigo fuente se lee de ahi en vez de compilar. La primera vez
 * sigue costando lo mismo; las siguientes (esa escena otra vez, o la partida
 * de manana) casi nada.
 *
 * SEGURIDAD. La clave es el codigo fuente ENTERO: la cabecera guarda dos
 * hashes distintos de 64 bits (CityHash y FNV-1a), su longitud, el perfil y
 * una version del formato. Un fichero que no cuadre en todo se ignora y se
 * compila como siempre (y se reescribe). Si el fichero no se puede leer o
 * escribir, tambien se compila como siempre: la cache nunca es necesaria.
 */
/**
 * LA CARPETA DE LA CACHE, CAMBIABLE PARA LAS PRUEBAS (0.4.2.3). Con
 * ux0:/data/azahar/cache_prueba.txt todo lo de la cache (binarios, listas,
 * marcas, paquetes) va a shadercache_prueba: las pruebas con la cache vacia no
 * tienen que apartar la del usuario (la tanda del 29-10 la dejo apartada dos
 * dias al cortarse la sesion a medias).
 */
std::string CacheDirChoice() {
    SceIoStat flag{};
    return sceIoGetstat("ux0:/data/azahar/cache_prueba.txt", &flag) >= 0
               ? "ux0:/data/azahar/shadercache_prueba"
               : "ux0:/data/azahar/shadercache";
}
const std::string g_cache_dir = CacheDirChoice();
const char* const kCacheDir = g_cache_dir.c_str();

/// Un fichero de la carpeta de la cache (los nombres de siempre, dentro de kCacheDir).
const char* CacheFile(std::string& storage, const char* name) {
    storage = g_cache_dir + "/" + name;
    return storage.c_str();
}
constexpr u32 kCacheMagic = 0x58475A41u; // "AZGX"
constexpr u32 kCacheVersion = 1;

struct CacheHeader {
    u32 magic;
    u32 version;
    u32 profile;
    u32 source_size;
    u64 hash_city;
    u64 hash_fnv;
    u32 program_size;
    u32 reserved;
};
static_assert(sizeof(CacheHeader) == 40, "cabecera de la cache de shaders");

/// Salidas que vienen de la cache: se liberan con free, no con shacccg. Con el
/// hash de su fuente, que es el nombre de su .gxp (0.3.8.1, ver CgOutputHash).
std::unordered_map<const SceShaccCgCompileOutput*, u64> g_cached_outputs;
/// Protege g_cached_outputs (0.1.9.6): leer de la cache de la tarjeta no
/// necesita el compilador, asi que no espera a la compilacion en curso.
std::mutex g_outputs_mutex;
/// El hilo de compilacion: su etapa no se apunta para el vigilante, que mira
/// solo el hilo de emulacion (0.1.9.8).
pthread_t g_worker_thread{};
bool g_worker_running = false;
bool g_cache_dir_ready = false;
u32 g_cache_notes = 0;
/**
 * Topes de las notas de compilacion, por juego (0.3.1.4): eran de toda la
 * sesion, y en crash.txt de 0.3.1.3 Kirby gasto las sesenta de tiempos antes
 * de que Pokemon Sol, cuarto juego de la sesion, compilara nada. Los pone a
 * cero PreloadCgCache, al arrancar cada juego.
 */
std::atomic<u32> g_start_notes{0};
std::atomic<u32> g_time_notes{0};
/// Cuando empezo la partida (PreloadCgCache), ver kHeavyGraceUs (0.3.9.7).
std::atomic<unsigned long long> g_session_start_us{0};

u64 Fnv1a64(const char* data, std::size_t size) {
    u64 hash = 0xCBF29CE484222325ull;
    for (std::size_t i = 0; i < size; i++) {
        hash ^= static_cast<u8>(data[i]);
        hash *= 0x100000001B3ull;
    }
    return hash;
}

std::string CachePath(u64 hash_city, SceShaccCgTargetProfile profile) {
    return fmt::format("{}/{:016x}_{}.gxp", kCacheDir, hash_city, static_cast<u32>(profile));
}

/// Ficheros de la cache precargados en memoria (PreloadCgCache), por ruta, y
/// la lista de los que ha usado el juego actual. g_preload_mutex protege los
/// tres: los usan el hilo de la GPU y el de compilacion.
std::mutex g_preload_mutex;
std::unordered_map<std::string, std::vector<u8>> g_preloaded;
std::unordered_set<std::string> g_game_list;
std::string g_game_list_path;
/**
 * LO QUE HAY EN LA CARPETA DE LA CACHE, EN MEMORIA (0.3.1.5). Lo que no esta
 * precargado se buscaba con sceIoOpen, y casi siempre no esta (es un shader
 * nuevo): en una carpeta de mas de mil ficheros y con esta tarjeta, decenas de
 * ms por intento, y cada programa nuevo prueba dos. crash.txt de 0.3.1.4 en
 * Pokemon Sol: "vs nuevos 72 (8692 ms, traducir 2719 ms)", en el hilo de la
 * GPU. PreloadCgCache lista la carpeta una vez por juego (tambien con
 * g_preload_mutex) y lo que no aparece ahi no se abre; StoreCached apunta lo
 * que escribe.
 */
std::unordered_set<std::string> g_card_files;
bool g_card_index_ready = false;

/// Ver AddVsIndex. Tambien con g_preload_mutex.
constexpr u32 kVsIndexMagic = 0x49565A41u; // "AZVI"
struct VsIndexHeader {
    u32 magic;
    u32 version;
    char tag[40];
};
struct VsIndexRecord {
    u64 key;
    u64 hash_city;
    u64 hash_fnv;
    u32 source_size;
    u32 variant;
};
static_assert(sizeof(VsIndexRecord) == 32, "registro del indice de vertices");
std::unordered_map<u64, VsIndexRecord> g_vs_index;
std::string g_vs_index_path;
/// El fichero ya tiene la cabecera de esta compilacion del traductor.
bool g_vs_index_valid = false;
/// Abierto para anadir al final desde el primer registro de la partida.
SceUID g_vs_index_fd = -1;

VsIndexHeader CurrentVsIndexHeader() {
    VsIndexHeader header{};
    header.magic = kVsIndexMagic;
    header.version = 1;
    std::strncpy(header.tag, Pica::Shader::Generator::GXM::VsGeneratorTag(),
                 sizeof(header.tag) - 1);
    return header;
}

/// El indice del juego que arranca (desde PreloadCgCache).
void LoadVsIndex(u64 program_id) {
    const std::string path = fmt::format("{}/vs_{:016x}.idx", kCacheDir, program_id);
    std::unordered_map<u64, VsIndexRecord> records;
    bool valid = false;
    const SceUID fd = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);
    if (fd >= 0) {
        const VsIndexHeader want = CurrentVsIndexHeader();
        VsIndexHeader have{};
        if (sceIoRead(fd, &have, sizeof(have)) == static_cast<int>(sizeof(have)) &&
            std::memcmp(&have, &want, sizeof(have)) == 0) {
            valid = true;
            VsIndexRecord record{};
            // Uno posterior con la misma clave sustituye al anterior.
            while (sceIoRead(fd, &record, sizeof(record)) == static_cast<int>(sizeof(record))) {
                records[record.key] = record;
            }
        }
        sceIoClose(fd);
    }
    const std::size_t count = records.size();
    {
        const std::lock_guard lock{g_preload_mutex};
        if (g_vs_index_fd >= 0) {
            sceIoClose(g_vs_index_fd);
            g_vs_index_fd = -1;
        }
        g_vs_index = std::move(records);
        g_vs_index_path = path;
        g_vs_index_valid = valid;
    }
    Common::VitaNote("gxm cache",
                     fmt::format("indice de vertices: {} programas{}", count,
                                 valid ? "" : " (de otra compilacion del traductor: de cero)")
                         .c_str());
}

/// Apunta el fichero en la lista del juego, una vez.
void RememberForGame(const std::string& path) {
    const std::lock_guard lock{g_preload_mutex};
    if (g_game_list_path.empty() || !g_game_list.insert(path).second) {
        return;
    }
    const SceUID fd =
        sceIoOpen(g_game_list_path.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd < 0) {
        return;
    }
    const std::string line = path + "\n";
    sceIoWrite(fd, line.data(), static_cast<SceSize>(line.size()));
    sceIoClose(fd);
}

void NoteCache(const std::string& text) {
    // Solo las primeras: crash.txt no es sitio para una linea por shader.
    if (g_cache_notes < 6) {
        g_cache_notes++;
        Common::VitaNote("gxm cache", text.c_str());
    }
}

/// Lo mismo que LoadCached, desde una copia del fichero en memoria.
const SceShaccCgCompileOutput* FromPreloaded(const CacheHeader& want,
                                             const std::vector<u8>& file) {
    CacheHeader have{};
    if (file.size() < sizeof(have)) {
        return nullptr;
    }
    std::memcpy(&have, file.data(), sizeof(have));
    if (have.magic != want.magic || have.version != want.version ||
        have.profile != want.profile || have.source_size != want.source_size ||
        have.hash_city != want.hash_city || have.hash_fnv != want.hash_fnv ||
        have.program_size == 0 || file.size() != sizeof(have) + have.program_size) {
        return nullptr;
    }
    void* memory = std::malloc(sizeof(SceShaccCgCompileOutput) + have.program_size);
    if (memory == nullptr) {
        return nullptr;
    }
    u8* program = static_cast<u8*>(memory) + sizeof(SceShaccCgCompileOutput);
    std::memcpy(program, file.data() + sizeof(have), have.program_size);
    auto* output = static_cast<SceShaccCgCompileOutput*>(memory);
    output->programData = program;
    output->programSize = have.program_size;
    output->diagnosticCount = 0;
    output->diagnostics = nullptr;
    {
        const std::lock_guard lock{g_outputs_mutex};
        g_cached_outputs.emplace(output, have.hash_city);
    }
    return output;
}

const SceShaccCgCompileOutput* LoadCachedFromCard(const CacheHeader& want,
                                                  const std::string& path);

const SceShaccCgCompileOutput* LoadCached(const CacheHeader& want, const std::string& path) {
    {
        const std::lock_guard lock{g_preload_mutex};
        const auto it = g_preloaded.find(path);
        if (it != g_preloaded.end()) {
            if (const SceShaccCgCompileOutput* output = FromPreloaded(want, it->second)) {
                return output;
            }
        }
        if (g_card_index_ready && g_card_files.count(path) == 0) {
            return nullptr;
        }
    }
    const SceShaccCgCompileOutput* output = LoadCachedFromCard(want, path);
    if (output != nullptr) {
        RememberForGame(path);
    }
    return output;
}

const SceShaccCgCompileOutput* LoadCachedFromCard(const CacheHeader& want,
                                                  const std::string& path) {
    const SceUID fd = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);
    if (fd < 0) {
        return nullptr;
    }
    CacheHeader have{};
    bool ok = sceIoRead(fd, &have, sizeof(have)) == static_cast<int>(sizeof(have)) &&
              have.magic == want.magic && have.version == want.version &&
              have.profile == want.profile && have.source_size == want.source_size &&
              have.hash_city == want.hash_city && have.hash_fnv == want.hash_fnv &&
              have.program_size > 0 && have.program_size < 4u * 1024u * 1024u;
    void* memory = nullptr;
    if (ok) {
        // La salida y el programa en el mismo bloque: el programa detras de la
        // estructura (16 bytes), asi que queda alineado igual que malloc.
        memory = std::malloc(sizeof(SceShaccCgCompileOutput) + have.program_size);
        ok = memory != nullptr;
    }
    if (ok) {
        u8* program = static_cast<u8*>(memory) + sizeof(SceShaccCgCompileOutput);
        ok = sceIoRead(fd, program, have.program_size) == static_cast<int>(have.program_size);
    }
    sceIoClose(fd);
    if (!ok) {
        std::free(memory);
        return nullptr;
    }
    auto* output = static_cast<SceShaccCgCompileOutput*>(memory);
    output->programData = static_cast<u8*>(memory) + sizeof(SceShaccCgCompileOutput);
    output->programSize = have.program_size;
    output->diagnosticCount = 0;
    output->diagnostics = nullptr;
    {
        const std::lock_guard lock{g_outputs_mutex};
        g_cached_outputs.emplace(output, have.hash_city);
    }
    return output;
}

/**
 * Copia en un bloque nuestro, con el mismo formato que LoadCached, de una
 * salida del compilador (0.1.7.9). Asi la salida de libshacccg se puede
 * destruir enseguida sin que
 * el programa registrado en el parcheador apunte a memoria suya.
 */
const SceShaccCgCompileOutput* CopyOutput(const SceShaccCgCompileOutput& source, u64 hash) {
    void* memory = std::malloc(sizeof(SceShaccCgCompileOutput) + source.programSize);
    if (memory == nullptr) {
        return nullptr;
    }
    auto* output = static_cast<SceShaccCgCompileOutput*>(memory);
    u8* program = static_cast<u8*>(memory) + sizeof(SceShaccCgCompileOutput);
    std::memcpy(program, source.programData, source.programSize);
    output->programData = program;
    output->programSize = source.programSize;
    output->diagnosticCount = 0;
    output->diagnostics = nullptr;
    {
        const std::lock_guard lock{g_outputs_mutex};
        g_cached_outputs.emplace(output, hash);
    }
    return output;
}

std::size_t HeapInUse() {
    return mallinfo().uordblks;
}

std::size_t FreeHeap() {
    const std::size_t total = _newlib_heap_size_user;
    const std::size_t used = HeapInUse();
    return total > used ? total - used : 0;
}

/**
 * Libre minimo para EMPEZAR una compilacion (0.1.8.5). Medido en 0.1.8.4 con
 * el shader de piel de Zafiro Alfa especializado: picos de 1,6 a 10,8 MB por
 * compilacion, y el compilador retiene menos de 0,6 MB. Los umbrales de
 * 0.1.8.3 (85 MB para los de vertices) dejaban sin compilar shaders que cabian
 * de sobra en cuanto el heap pasaba de 178 MB en uso.
 */
constexpr std::size_t kMinFreeToCompile = 24u * 1024u * 1024u;
constexpr std::size_t kMinFreeToCompileVs = 32u * 1024u * 1024u;

/**
 * LO QUE GASTA CADA COMPILACION, A crash.txt (0.1.7.9). Las ocho primeras.
 *
 * 0.1.8.5: SIN sceShaccCgReleaseCompiler ni descargar el compilador despues de
 * compilar. Las dos cosas se metieron en 0.1.7.9 y 0.1.8.3 contra un supuesto
 * agotamiento de memoria, y las dos estaban en la secuencia de los dos crashes
 * siguientes: 0.1.8.0 murio DENTRO del compilador justo despues de un
 * ReleaseCompiler, y 0.1.8.4 murio dentro de malloc, con el heap corrupto,
 * tras cuatro descargas en las que se liberaba toda la memoria que el modulo
 * habia pedido (si algo suyo la seguia usando, la corrompia). Y lo medido en
 * 0.1.8.4 dice que no hacian falta: picos de 1,6-10,8 MB y menos de 0,6 MB
 * retenidos. Se vuelve a como compilaba de 0.1.5.9 a 0.1.7.8.
 */
u32 g_mem_notes = 0;

void NoteCompileMemory(const char* name, std::size_t before, std::size_t live_before) {
    if (g_mem_notes >= 8) {
        return;
    }
    g_mem_notes++;
    const std::size_t after = HeapInUse();
    const std::size_t peak = g_peak_bytes > live_before ? g_peak_bytes - live_before : 0;
    Common::VitaNote("gxm mem",
                     fmt::format("{}: heap en uso {} KB, despues {} KB; pico de la compilacion "
                                 "{} KB, el compilador retiene {} KB",
                                 name, before / 1024, after / 1024, peak / 1024,
                                 g_live_bytes / 1024)
                         .c_str());
}

void StoreCached(CacheHeader header, const std::string& path,
                 const SceShaccCgCompileOutput& output) {
    if (!g_cache_dir_ready) {
        sceIoMkdir(kCacheDir, 0777);
        g_cache_dir_ready = true;
    }
    header.program_size = output.programSize;
    const SceUID fd = sceIoOpen(path.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) {
        return;
    }
    const bool ok =
        sceIoWrite(fd, &header, sizeof(header)) == static_cast<int>(sizeof(header)) &&
        sceIoWrite(fd, output.programData, output.programSize) ==
            static_cast<int>(output.programSize);
    sceIoClose(fd);
    if (!ok) {
        // A medias no sirve: que la proxima vez se compile.
        sceIoRemove(path.c_str());
        return;
    }
    {
        const std::lock_guard lock{g_preload_mutex};
        g_card_files.insert(path);
    }
    RememberForGame(path);
}

/**
 * EL SHADER QUE ROMPE EL COMPILADOR, A LA TARJETA (0.1.5.9).
 *
 * En 0.1.5.8 un shader de vertices traducido dejo el compilador de Cg sin
 * compilar nada mas (todo a software, 0.6 FPS), y en crash.txt solo cabe el
 * primer mensaje, no el shader. Sin el codigo fuente no hay forma de saber
 * que construccion le sienta mal. Se guardan el primer fallo y el primer
 * "internal error" (pueden ser distintos) enteros, con sus mensajes.
 */
/// Cuantos se guardan por sesion (0.1.7.3): con las variantes, un mismo shader
/// puede fallar de varias formas, y cada una dice algo distinto.
u32 g_dumped_failure = 0;
u32 g_dumped_internal = 0;

void DumpFailedSource(const char* name, const char* source, const SceShaccCgCompileOutput* output,
                      bool internal) {
    if ((internal ? g_dumped_internal : g_dumped_failure) >= 4) {
        return;
    }
    (internal ? g_dumped_internal : g_dumped_failure)++;
    const SceUID fd = sceIoOpen("ux0:/data/azahar/cg_error.txt",
                                SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd < 0) {
        return;
    }
    std::string text = fmt::format("===== {} ({}) =====\n", name,
                                   internal ? "ERROR INTERNO" : "fallo de compilacion");
    if (output != nullptr) {
        for (int i = 0; i < output->diagnosticCount && i < 8; i++) {
            const auto& diag = output->diagnostics[i];
            text += fmt::format("linea {}: {}\n",
                                diag.location != nullptr ? diag.location->lineNumber : 0,
                                diag.message != nullptr ? diag.message : "(sin mensaje)");
        }
    }
    text += "----- codigo fuente -----\n";
    text += source;
    text += "\n\n";
    sceIoWrite(fd, text.data(), static_cast<SceSize>(text.size()));
    sceIoClose(fd);
    Common::VitaNote("gxm shader", "codigo del shader que fallo en ux0:/data/azahar/cg_error.txt");
}

/**
 * LISTA NEGRA DE CODIGOS QUE ROMPEN EL COMPILADOR (0.1.7.3).
 *
 * Antes de 0.1.7.3 el primer "internal error" dejaba los shaders de vertices
 * traducidos apagados TODA la sesion (g_poisoned), y el que lo rompio salia
 * otra vez en la siguiente partida: en la cinematica de Rubi Omega eso eran
 * 134 de 142 lotes sombreados en la CPU, ~84 ms de ~190 por vblank. Ahora el
 * generador tiene variantes (rasterizer_gxm.cpp prueba otra si una falla), asi
 * que lo que se aparta es solo ESE codigo fuente: sus dos hashes van a la
 * tarjeta y no se le vuelve a dar al compilador, ni en esta sesion ni en las
 * siguientes. El resto de shaders sigue compilando con normalidad.
 */
/**
 * 0.1.8.5: fichero NUEVO (cg_roto2.bin). En el viejo habia 24 codigos, y la nota
 * de 0.1.5.8 de aqui arriba dice por que muchos podian ser inocentes: tras un
 * error interno el compilador se queda roto y los siguientes fallan igual. Un
 * shader de fragmentos apuntado aqui se salta para siempre y TODOS sus lotes
 * se rasterizan por software (0.1.8.4: "gxm skip: compilar shader" y despues
 * 465 ms por vblank en los vertices). Ahora solo se apunta lo que falla con el
 * compilador recien cargado; lo demas solo se salta en esta sesion.
 */
std::string g_kBadListPath_storage;
const char* const kBadListPath = CacheFile(g_kBadListPath_storage, "cg_roto2.bin");
/**
 * EL SHADER QUE SE ESTA COMPILANDO, EN LA TARJETA (0.2.0.6). libshacccg no solo
 * devuelve "internal error": con algun shader se cae entero (acceso a memoria
 * invalido dentro del modulo de Sony, volcados de 0.2.0.1 y 0.2.0.5 al pasar
 * al 3D de Pokemon Sol), y eso se lleva el proceso sin que se pueda apuntar
 * nada despues. Se escribe aqui antes de compilar y se borra al volver: si al
 * arrancar sigue ahi, la sesion anterior murio compilandolo, y va a la lista
 * negra. Ese shader se dibuja por el otro camino, pero el juego ya no se cae.
 */
std::string g_kCompilingPath_storage;
const char* const kCompilingPath = CacheFile(g_kCompilingPath_storage, "cg_compilando.bin");
/// El codigo del que se compila, para estudiar el que lo tumbe (0.2.0.8).
std::string g_kCompilingSourcePath_storage;
const char* const kCompilingSourcePath = CacheFile(g_kCompilingSourcePath_storage, "cg_compilando.cg");
/**
 * Los que ya tumbaron el compilador una vez con optimizacion (0.2.0.8): se
 * reintentan sin ella (menos memoria y otro camino dentro del compilador). Si
 * tambien asi se caen, a la lista negra.
 */
std::string g_kNoOptListPath_storage;
const char* const kNoOptListPath = CacheFile(g_kNoOptListPath_storage, "cg_sin_optimizar.bin");
struct BadSource {
    u64 hash_city;
    u64 hash_fnv;
    bool operator==(const BadSource& other) const {
        return hash_city == other.hash_city && hash_fnv == other.hash_fnv;
    }
};
struct BadSourceHash {
    std::size_t operator()(const BadSource& bad) const {
        return static_cast<std::size_t>(bad.hash_city ^ (bad.hash_fnv >> 7));
    }
};
std::unordered_set<BadSource, BadSourceHash> g_bad_sources;
bool g_bad_loaded = false;
/**
 * UNA SEGUNDA OPORTUNIDAD A LOS DE FRAGMENTOS (0.3.2.1). crash.txt de 0.3.2.0,
 * Pokemon X: "fs: compilar shader" en los 1000 rechazos de cada ventana y
 * ningun "gxm compila" de fragmentos en toda la sesion: su shader estaba en la
 * lista negra y CompileCg lo devolvia nulo sin decir nada. El 44 % de sus
 * triangulos, por el rasterizador de software, y el juego a 4,5 fps esperando
 * a la GPU el 85 % del tiempo. Un error interno con el compilador recien
 * cargado (lo que llevaba ahi a los de fragmentos) se da en un nivel de
 * optimizacion: sin optimizar suele compilar. Cada uno de fragmentos de la
 * lista se reintenta UNA vez en O0, apuntado en kRetriedPath ANTES de compilar
 * para que, si colgara el compilador, no se repita en otra sesion. No se
 * reintenta el que consta que tumbo el proceso (kDiedPath, o su cg_murio_*.cg).
 *
 * RETIRADO EN 0.3.7.8: el binario O0 de uno de estos colgaba la GPU en la
 * intro de Zafiro Alfa. La lista se sigue leyendo solo para borrar de la
 * cache esos binarios (PurgeUnoptimizedPrograms).
 */
std::string g_kRetriedPath_storage;
const char* const kRetriedPath = CacheFile(g_kRetriedPath_storage, "cg_reintentado.bin");
std::string g_kDiedPath_storage;
const char* const kDiedPath = CacheFile(g_kDiedPath_storage, "cg_murio.bin");
std::unordered_set<BadSource, BadSourceHash> g_died_sources;
/**
 * SOSPECHOSO (0.2.3.5). Sin volcado no se culpa al shader que se estaba
 * compilando (0.2.1.6), pero entonces uno que tumba el compilador sin dejar
 * volcado -- Pokemon Sol, al entrar en combate en 0.2.3.3: la sesion acaba
 * compilando uno de 12.699 bytes y no hay psp2core -- tumbaba el juego en
 * cada combate. Ahora queda apuntado aqui; si la siguiente sesion vuelve a
 * acabar compilando ESE MISMO, a la lista negra (a la CPU). Si se llega a
 * compilar bien, deja de ser sospechoso.
 */
std::string g_kSuspectPath_storage;
const char* const kSuspectPath = CacheFile(g_kSuspectPath_storage, "cg_sospechoso.bin");
/**
 * CUANTAS VECES ANTES DE CONDENARLO (0.3.5.1). La regla de arriba ("si la
 * siguiente sesion vuelve a acabar compilando ESE MISMO, a la lista negra")
 * daba por muerto a cualquiera que el usuario pillara compilando dos veces. Y
 * los grandes TARDAN: en el log del 23-10, uno de 26.736 bytes de Zafiro Alfa
 * compilo bien en 170.136 ms (99.710 ms de CPU), y el siguiente, de 26.868, no
 * llego a acabar en dos partidas seguidas de unos minutos. Sin volcado de la
 * consola no hay ninguna prueba de que se haya caido: lo unico que consta es
 * que la sesion termino antes que el. Asi que a los grandes se les dan cuatro
 * oportunidades en vez de dos; a los pequenos, que compilan en segundos, las
 * dos de siempre, porque ahi pillarlo dos veces si significa algo.
 */
constexpr u32 kBigSourceBytes = 20000;
/**
 * LOS AVISOS SIN PRUEBAS SE ACABAN (0.4.2.2). Contar "la sesion acabo
 * compilandolo" como aviso castigaba al usuario que cierra el juego mientras
 * compila (los de 0,7-25 s lo hacen a menudo con la cache vacia). Ahora la
 * marca lleva cuanta CPU llevaba el compilador con el (ver KeepMarkerAlive): sin
 * volcado, solo se le culpa si se habia comido esto sin acabar, que es colgarse.
 * El mas lento medido en el banco: 66 s de CPU uno de vertices de 24 KB.
 */
constexpr u32 kHangCpuSmallS = 120;
constexpr u32 kHangCpuBigS = 900;

/// Lo que se apunta en kCompilingPath antes de compilar.
struct CompilingMarker {
    BadSource source;
    u32 profile;
    u32 optimization;
    u32 source_size;
    u32 free_kb;
    /**
     * CUANDO EMPEZO, HASTA CUANDO SIGUIO Y CUANTA CPU LLEVABA (0.4.2.2), en
     * segundos del reloj de la tarjeta (ver FsNowSeconds). Al arrancar solo
     * cuenta como prueba un volcado escrito MIENTRAS se compilaba este shader, y
     * sin volcado solo una compilacion que se comio kHangCpu*S de CPU.
     */
    u64 start_s;
    u64 alive_s;
    u32 magic;
    u32 flags;
    u32 cpu_s;
    u32 reserved;
};
constexpr u32 kMarkerMagic = 0x324D5A41u; // "AZM2"
/// En esa sesion se colgo la GPU (gpu_colgada.txt): lo que muriera despues lo explica eso.
constexpr u32 kMarkerGpuHung = 1u;
/// El tamano de la marca de antes de 0.4.2.2, sin tiempos.
constexpr u32 kOldMarkerBytes = 32;

/// Una fecha de fichero o del RTC, en segundos.
u64 TickSeconds(const SceDateTime& t) {
    SceRtcTick tick{};
    if (sceRtcGetTick(&t, &tick) < 0) {
        return 0;
    }
    return tick.tick / 1000000ull;
}

/**
 * EL RELOJ DE LA TARJETA (0.4.2.2). Las fechas de los ficheros (las de los
 * volcados psp2core) no tienen por que ir en la misma zona horaria que el RTC.
 * La primera vez se escribe un fichero, se mira su fecha y se guarda la
 * diferencia; despues la hora "de la tarjeta" es el RTC mas esa diferencia.
 */
long long g_fs_offset_s = 0;
bool g_fs_offset_ready = false;

u64 FsNowSeconds() {
    const auto rtc_now = [] {
        SceDateTime now{};
        return sceRtcGetCurrentClock(&now, 0) < 0 ? 0ull : TickSeconds(now);
    };
    if (!g_fs_offset_ready) {
        g_fs_offset_ready = true;
        const std::string probe = std::string(kCacheDir) + "/cg_reloj.bin";
        const SceUID fd = sceIoOpen(probe.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        if (fd >= 0) {
            const u32 one = 1;
            sceIoWrite(fd, &one, sizeof(one));
            sceIoClose(fd);
            SceIoStat stat{};
            const u64 rtc = rtc_now();
            if (sceIoGetstat(probe.c_str(), &stat) >= 0 && rtc != 0) {
                g_fs_offset_s = static_cast<long long>(TickSeconds(stat.st_mtime)) -
                                static_cast<long long>(rtc);
            }
        }
        Common::VitaNote("gxm shader",
                         fmt::format("reloj de la tarjeta: {} s respecto al RTC", g_fs_offset_s)
                             .c_str());
    }
    const u64 rtc = rtc_now();
    return rtc == 0 ? 0 : static_cast<u64>(static_cast<long long>(rtc) + g_fs_offset_s);
}

/**
 * LA MARCA CON EL FICHERO ABIERTO (0.3.1.8). Antes de cada compilacion se
 * abria, truncaba y cerraba la marca (y la fuente), y al acabar se borraba:
 * operaciones de metadatos en ux0 que con la tarjeta ocupada costaban 0,5-2 s
 * POR SHADER ("tarjeta antes" en crash.txt; un 12-33 % de cada compilacion en
 * Super Mario 3D Land). Ahora el fichero se queda abierto y se reescriben sus
 * bytes en su sitio; "borrar" es escribir una marca vacia, que LoadBadSources
 * ignora. Desde 0.4.2.2 con sceIoPwrite: la mantiene viva otro hilo
 * (KeepMarkerAlive) y un lseek compartido entre dos hilos no es seguro.
 */
SceUID g_marker_fd = -1;
/// Hay una compilacion apuntada en la marca ahora mismo.
std::atomic<bool> g_marker_live{false};
/// runClocks del hilo de compilacion al empezar (0 si no compila ese hilo).
std::atomic<unsigned long long> g_marker_start_clocks{0};
/// La GPU se ha colgado en esta sesion (ver CgNoteGpuHang).
std::atomic<bool> g_gpu_hung{false};
/**
 * EL CODIGO, EN UN FICHERO QUE TAMBIEN SE QUEDA ABIERTO (0.4.2.2). Se abria,
 * truncaba y cerraba antes de cada compilacion de vertices o de mas de 4 KB:
 * 1,7 s de "tarjeta antes" en cada uno de vertices de Luigi's Mansion. Ahora se
 * reescribe en su sitio; lo que sobre detras de un codigo mas corto no se lee
 * (la marca dice cuantos bytes valen).
 */
SceUID g_source_fd = -1;

void WriteCompilingMarker(const CompilingMarker& marker) {
    if (g_marker_fd < 0) {
        g_marker_fd = sceIoOpen(kCompilingPath, SCE_O_WRONLY | SCE_O_CREAT, 0777);
        if (g_marker_fd < 0) {
            return;
        }
    }
    sceIoPwrite(g_marker_fd, &marker, sizeof(marker), 0);
    g_marker_live.store(true, std::memory_order_release);
}

void ClearCompilingMarker() {
    g_marker_live.store(false, std::memory_order_release);
    g_marker_start_clocks.store(0, std::memory_order_relaxed);
    if (g_marker_fd < 0) {
        return;
    }
    const CompilingMarker empty{};
    sceIoPwrite(g_marker_fd, &empty, sizeof(empty), 0);
}

/// Ver CompilingMarker::alive_s: lo llama el hilo del balanceador cada pocos segundos.
void KeepMarkerAlive(SceUID worker) {
    if (!g_marker_live.load(std::memory_order_acquire) || g_marker_fd < 0) {
        return;
    }
    const u64 now = FsNowSeconds();
    sceIoPwrite(g_marker_fd, &now, sizeof(now), offsetof(CompilingMarker, alive_s));
    const unsigned long long start = g_marker_start_clocks.load(std::memory_order_relaxed);
    if (start == 0 || worker < 0) {
        return;
    }
    SceKernelThreadInfo info{};
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(worker, &info) >= 0 && info.runClocks >= start) {
        const u32 cpu = static_cast<u32>((info.runClocks - start) / 1000000ull);
        sceIoPwrite(g_marker_fd, &cpu, sizeof(cpu), offsetof(CompilingMarker, cpu_s));
    }
}

void AppendRecord(const char* path, const BadSource& record) {
    const SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd < 0) {
        return;
    }
    sceIoWrite(fd, &record, sizeof(record));
    sceIoClose(fd);
}

void ReadRecords(const char* path, std::unordered_set<BadSource, BadSourceHash>& out) {
    const SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        return;
    }
    BadSource record{};
    // Tope por si el fichero estuviera mal: 4096 entradas son 64 KB.
    for (u32 i = 0; i < 4096; i++) {
        if (sceIoRead(fd, &record, sizeof(record)) != static_cast<int>(sizeof(record))) {
            break;
        }
        out.insert(record);
    }
    sceIoClose(fd);
}

void StoreBadSource(const BadSource& bad);

/**
 * QUE VOLCADOS HAY ENTRE DOS HORAS (0.4.2.2). Hasta 0.4.2.1 contaba cualquier
 * psp2core-* posterior a la fecha de la marca, y eso culpaba a inocentes: la
 * fecha de la marca era la de abrirla (se queda abierta desde 0.3.1.8), los
 * volcados de OTRAS aplicaciones se llaman igual (en la tarjeta del usuario,
 * uno de YouTubers Life del 28-10 a las 22:35) y un cuelgue de la GPU tambien
 * acaba en volcado. Los seis shaders que entraron en la lista negra el 27, 28 y
 * 29-10 compilaban bien en el banco (de 0,25 a 66 s, sin un error). Ahora solo
 * cuenta un volcado escrito mientras se compilaba ESE shader o poco despues.
 *
 * EL GPUCRASH EXCULPA (0.3.7.5): que la GPU se cuelgue no dice nada del
 * compilador; al contrario, EXPLICA la muerte.
 */
enum class DumpSince {
    None,     ///< Ningun volcado en esa ventana.
    Gpu,      ///< Un psp2core-*-GPUCRASH: murio la consola, no el compilador.
    Compiler, ///< Un psp2core-* normal: se cayo el proceso compilando.
};

DumpSince CrashDumpBetween(u64 from_s, u64 until_s) {
    const SceUID dir = sceIoDopen("ux0:data");
    if (dir < 0) {
        return DumpSince::None;
    }
    DumpSince found = DumpSince::None;
    SceIoDirent entry{};
    // Sin cortar al primero: uno del proceso manda sobre uno de la GPU.
    while (sceIoDread(dir, &entry) > 0) {
        if (std::strncmp(entry.d_name, "psp2core-", 9) != 0) {
            continue;
        }
        const u64 at = TickSeconds(entry.d_stat.st_mtime);
        if (at < from_s || at > until_s) {
            continue;
        }
        if (std::strstr(entry.d_name, "GPUCRASH") == nullptr) {
            found = DumpSince::Compiler;
            break;
        }
        found = DumpSince::Gpu;
    }
    sceIoDclose(dir);
    return found;
}

/// El codigo del que tumbo el proceso, a cg_murio_<hash>.cg para estudiarlo desde el PC.
void SaveDiedSource(const BadSource& source, u32 size) {
    if (size == 0 || size > 1024u * 1024u) {
        return;
    }
    std::vector<char> text(size);
    const SceUID in = sceIoOpen(kCompilingSourcePath, SCE_O_RDONLY, 0);
    if (in < 0) {
        return;
    }
    const int got = sceIoRead(in, text.data(), size);
    sceIoClose(in);
    if (got != static_cast<int>(size)) {
        return;
    }
    const std::string path = fmt::format("{}/cg_murio_{:016x}.cg", kCacheDir, source.hash_city);
    const SceUID out = sceIoOpen(path.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (out >= 0) {
        sceIoWrite(out, text.data(), size);
        sceIoClose(out);
    }
}

/// La sesion anterior acabo con este shader a medio compilar: se le culpa o no.
void EvaluateInterruptedCompile(const CompilingMarker& marker, bool timed) {
    const bool vertex = marker.profile == static_cast<u32>(SCE_SHACCCG_PROFILE_VP);
    const bool big = marker.source_size >= kBigSourceBytes;
    bool blame = false;
    std::string verdict;
    if (!timed) {
        verdict = "marca sin tiempos (de antes de 0.4.2.2): no se le culpa";
    } else if ((marker.flags & kMarkerGpuHung) != 0) {
        verdict = "en esa sesion se colgo la GPU: no se le culpa";
    } else {
        const u64 last_s = std::max(marker.alive_s, marker.start_s);
        const u64 wall_s = last_s - marker.start_s;
        // Escribir un volcado lleva su rato: hasta cuatro minutos despues.
        const DumpSince dump =
            CrashDumpBetween(marker.start_s > 5 ? marker.start_s - 5 : 0, last_s + 240);
        if (dump == DumpSince::Compiler) {
            blame = true;
            verdict = fmt::format("volcado del proceso a los {} s de empezar ({} s de CPU): a "
                                  "la lista negra",
                                  wall_s, marker.cpu_s);
        } else if (dump == DumpSince::Gpu) {
            verdict = "volcado GPUCRASH: murio la GPU, no se le culpa";
        } else if (marker.cpu_s >= (big ? kHangCpuBigS : kHangCpuSmallS)) {
            blame = true;
            verdict = fmt::format("{} s de CPU sin acabar ({} s de reloj): colgado, a la lista "
                                  "negra",
                                  marker.cpu_s, wall_s);
        } else {
            verdict = fmt::format("cortado a los {} s ({} s de CPU) y sin volcado: se cerro el "
                                  "juego, no se le culpa",
                                  wall_s, marker.cpu_s);
        }
    }
    if (blame && vertex && marker.optimization == 3) {
        /**
         * Murio a O3 (ver g_vs_o3, 0.4.3.7): primero se le da la oportunidad
         * a O1, que es como compilaban todos hasta 0.4.3.6. Si a O1 tambien
         * tumba el proceso, la marca siguiente dira O1 y entonces si va a la
         * lista negra.
         */
        AddO3Bad(marker.source.hash_city);
        SaveDiedSource(marker.source, marker.source_size);
        verdict += " (a O3: se compilara a O1, sin lista negra)";
    } else if (blame) {
        StoreBadSource(marker.source);
        if (g_died_sources.insert(marker.source).second) {
            AppendRecord(kDiedPath, marker.source);
        }
        SaveDiedSource(marker.source, marker.source_size);
    }
    Common::VitaNote("gxm shader",
                     fmt::format("la sesion anterior acabo compilando un shader de {} ({} bytes, "
                                 "O{}): {}",
                                 vertex ? "vertices" : "fragmentos", marker.source_size,
                                 marker.optimization, verdict)
                         .c_str());
}

/**
 * LA AMNISTIA (0.4.2.2). Lo que entro en la lista negra por "murio compilando"
 * con las reglas de antes (cg_murio.bin) vuelve a compilarse una vez: con las
 * de ahora (EvaluateInterruptedCompile) el que de verdad tumbe el compilador
 * vuelve a la lista con pruebas. Los de "error interno" (el compilador recien
 * cargado lo rechaza, siempre igual) se quedan. Una sola vez por tarjeta.
 */
std::string g_kAmnestyPath_storage;
const char* const kAmnestyPath = CacheFile(g_kAmnestyPath_storage, "cg_amnistia_0422.txt");
std::string g_kDiedBeforeAmnestyPath_storage;
const char* const kDiedBeforeAmnestyPath = CacheFile(g_kDiedBeforeAmnestyPath_storage, "cg_murio_antes_0422.bin");

void Amnesty() {
    SceIoStat done{};
    if (sceIoGetstat(kAmnestyPath, &done) >= 0) {
        return;
    }
    u32 freed = 0;
    for (const BadSource& died : g_died_sources) {
        freed += static_cast<u32>(g_bad_sources.erase(died));
    }
    const std::size_t died = g_died_sources.size();
    g_died_sources.clear();
    if (!g_cache_dir_ready) {
        sceIoMkdir(kCacheDir, 0777);
        g_cache_dir_ready = true;
    }
    if (freed != 0) {
        const SceUID fd = sceIoOpen(kBadListPath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        if (fd >= 0) {
            for (const BadSource& bad : g_bad_sources) {
                sceIoWrite(fd, &bad, sizeof(bad));
            }
            sceIoClose(fd);
        }
    }
    if (died != 0) {
        sceIoRemove(kDiedBeforeAmnestyPath);
        sceIoRename(kDiedPath, kDiedBeforeAmnestyPath);
        sceIoRemove(kDiedPath);
    }
    const SceUID flag = sceIoOpen(kAmnestyPath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (flag >= 0) {
        const std::string text = fmt::format("{} de {} muertes sin pruebas fuera de la lista\n",
                                             freed, died);
        sceIoWrite(flag, text.data(), static_cast<SceSize>(text.size()));
        sceIoClose(flag);
    }
    Common::VitaNote("gxm shader", fmt::format("amnistia: {} shaders de la lista negra por "
                                               "muertes sin pruebas vuelven a compilarse",
                                               freed)
                                       .c_str());
}

void LoadBadSources() {
    if (g_bad_loaded) {
        return;
    }
    g_bad_loaded = true;
    ReadRecords(kBadListPath, g_bad_sources);
    ReadRecords(kDiedPath, g_died_sources);
    Amnesty();
    // El sospechoso de 0.2.3.5-0.4.2.1 ya no se usa (ver kHangCpuSmallS).
    sceIoRemove(kSuspectPath);
    CompilingMarker interrupted{};
    int got = -1;
    {
        const SceUID fd = sceIoOpen(kCompilingPath, SCE_O_RDONLY, 0);
        if (fd >= 0) {
            got = sceIoRead(fd, &interrupted, sizeof(interrupted));
            sceIoClose(fd);
        }
    }
    // Marca vacia (ver ClearCompilingMarker): no murio compilando.
    if (got >= static_cast<int>(kOldMarkerBytes) &&
        !(interrupted.source == BadSource{} && interrupted.source_size == 0)) {
        EvaluateInterruptedCompile(interrupted, got == static_cast<int>(sizeof(interrupted)) &&
                                                    interrupted.magic == kMarkerMagic);
    }
    sceIoRemove(kCompilingPath);
    if (!g_bad_sources.empty()) {
        Common::VitaNote("gxm shader", fmt::format("{} shaders que rompen el compilador se "
                                                   "saltan (lista de la tarjeta)",
                                                   g_bad_sources.size())
                                           .c_str());
    }
}

void StoreBadSource(const BadSource& bad) {
    if (!g_bad_sources.insert(bad).second) {
        return;
    }
    if (!g_cache_dir_ready) {
        sceIoMkdir(kCacheDir, 0777);
        g_cache_dir_ready = true;
    }
    const SceUID fd =
        sceIoOpen(kBadListPath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd < 0) {
        return;
    }
    sceIoWrite(fd, &bad, sizeof(bad));
    sceIoClose(fd);
}

/// Ver g_tracking: descarga el compilador roto y devuelve lo que perdio.
void RecoverFromInternalError(bool counts = true) {
    if (g_module < 0) {
        return;
    }
    /**
     * Dos topes (0.1.8.8). Con 0.1.8.7 se llego al unico que habia (8) en el 3D
     * de Zafiro Alfa y el compilador se quedaba cargado y ROTO: tampoco
     * compilaban los shaders de fragmentos y casi todo se rasterizaba por
     * software ("vs compilador roto", ts 5436 contra tg 66). Ahora, a partir de
     * kVsRecoveries recargas, se dejan de traducir shaders de vertices (son
     * los que lo rompen), pero se sigue recargando para que los de fragmentos
     * compilen. kMaxRecoveries acota la memoria que no se devuelve (ver abajo):
     * de ahi en adelante si se queda roto.
     */
    constexpr u32 kVsRecoveries = 6;
    constexpr u32 kMaxRecoveries = 16;
    // Un error a O2 que se reintenta a O1 no cuenta (0.4.1.1).
    if (counts && g_generation + 1 >= kVsRecoveries && !g_poisoned) {
        g_poisoned = true;
        Common::VitaNote("gxm shader", "demasiados errores internos: no se traducen mas "
                                       "shaders de vertices en esta sesion");
    }
    if (g_generation >= kMaxRecoveries) {
        g_live_bytes = 0;
        return;
    }
    int status = 0;
    const int rc = sceKernelStopUnloadModule(g_module, 0, nullptr, 0, nullptr, &status);
    if (rc < 0) {
        Common::VitaNote("gxm shader", fmt::format("no se pudo descargar el compilador roto "
                                                   "({:#x}): sigue sin compilar",
                                                   static_cast<u32>(rc))
                                           .c_str());
        g_live_bytes = 0; // no se puede devolver: el modulo sigue vivo
        g_poisoned = true;
        return;
    }
    /**
     * 0.1.8.5: la memoria del modulo NO se libera, solo se olvida. Liberarla
     * (0.1.5.9-0.1.8.4) da por hecho que tras la descarga nada la usa, y el
     * crash de 0.1.8.4 (malloc con el heap corrupto despues de varias
     * descargas) dice que no es seguro. Se pierde el pico de una compilacion
     * fallida (unos MB), y esto solo pasa con errores internos.
     */
    const std::size_t lost = g_live_bytes;
    g_live_bytes = 0;
    g_module = -1;
    g_last_attempt_us = 0; // recargar ya en la siguiente compilacion
    g_compiles_since_load = 0;
    g_generation++;
    std::strcpy(g_status, "recargando");
    Common::VitaNote("gxm shader",
                     fmt::format("compilador descargado tras el error interno ({} KB sin "
                                 "devolver); se recarga limpio",
                                 lost / 1024)
                         .c_str());
}

} // Anonymous namespace

u32 CgGeneration() {
    return g_generation;
}

bool EnsureCgReady() {
    const std::lock_guard lock{g_cg_mutex};
    if (g_module >= 0) {
        return true;
    }
    const unsigned long long now_us = Common::VitaMicros();
    if (g_last_attempt_us != 0 && now_us - g_last_attempt_us < kRetryIntervalUs) {
        return false;
    }
    g_last_attempt_us = now_us;
    for (const char* path : kShacccgPaths) {
        g_module = sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, nullptr);
        if (g_module >= 0) {
            LOG_INFO(Render, "GXM: libshacccg cargado desde {}", path);
            break;
        }
        LOG_ERROR(Render, "GXM: cargar {} fallo con {:#x}", path, static_cast<u32>(g_module));
    }
    if (g_module < 0) {
        const auto written = fmt::format_to_n(g_status, sizeof(g_status) - 1,
                                              "sin libshacccg ({:#x})",
                                              static_cast<u32>(g_module));
        *written.out = 0;
        return false;
    }
    sceShaccCgSetDefaultAllocator(&CgAlloc, &CgFree);
    g_compiles_since_load = 0;
    sceShaccCgInitializeCallbackList(&g_callbacks, SCE_SHACCCG_TRIVIAL);
    g_callbacks.openFile = &OpenSource;
    std::strcpy(g_status, "listo");
    return true;
}

const char* CgStatus() {
    return g_status;
}

bool CgPoisoned() {
    return g_poisoned;
}

bool CgHeapLow() {
    const std::size_t total = _newlib_heap_size_user;
    const std::size_t reserve = static_cast<std::size_t>(kCgHeapReserveMB) * 1024u * 1024u;
    return HeapInUse() + reserve > total;
}

/**
 * Los de vertices compilados en O0 por 0.2.0.8-0.2.1.3 (los de la lista de sin
 * optimizar) se quedaron en la cache de la tarjeta, y uno de ellos cuelga la
 * GPU: se borran y van a la lista negra antes de precargar nada (0.2.1.4).
 * Despues de la primera vez no queda fichero que borrar y no hace nada.
 */
/*
 * Y LOS DE FRAGMENTOS TAMBIEN (0.3.7.8). Un shader de fragmentos que daba error
 * interno al optimizar se recompilaba en O0 (por la lista de sin optimizar o
 * por el reintento de la lista negra, kRetriedPath) y el binario se guardaba
 * en la cache con el nombre de siempre. En la intro de Zafiro Alfa (la bici)
 * uno de esos colgaba la GPU en cada partida: la caja negra (gxm_caja_negra.h)
 * dio la escena y el programa de fragmentos, que salia de la cache, y al quitar
 * a mano los tres binarios O0 de fragmentos de la tarjeta (25-10) la escena
 * paso. Ya no se compila ninguno en O0, y aqui se borran los que queden de
 * cualquier juego; sus lotes van por la CPU, como antes del reintento.
 */
void PurgeUnoptimizedPrograms() {
    std::unordered_set<BadSource, BadSourceHash> no_opt;
    ReadRecords(kNoOptListPath, no_opt);
    std::unordered_set<BadSource, BadSourceHash> retried;
    ReadRecords(kRetriedPath, retried);
    u32 purged_vs = 0;
    u32 purged_fs = 0;
    const std::lock_guard lock{g_cg_mutex};
    for (const BadSource& source : no_opt) {
        if (sceIoRemove(CachePath(source.hash_city, SCE_SHACCCG_PROFILE_VP).c_str()) >= 0) {
            StoreBadSource(source);
            purged_vs++;
        }
    }
    no_opt.insert(retried.begin(), retried.end());
    for (const BadSource& source : no_opt) {
        if (sceIoRemove(CachePath(source.hash_city, SCE_SHACCCG_PROFILE_FP).c_str()) >= 0) {
            StoreBadSource(source);
            purged_fs++;
        }
    }
    if (purged_vs + purged_fs != 0) {
        Common::VitaNote("gxm cache",
                         fmt::format("{} shaders de vertices y {} de fragmentos sin optimizar "
                                     "borrados de la cache (colgaban la GPU): a la CPU",
                                     purged_vs, purged_fs)
                             .c_str());
    }
}

/**
 * EL PAQUETE DEL JUEGO (0.3.9.4). La precarga leia cada programa de su propio
 * fichero en una carpeta de ~2.700: unos 90 ms por sceIoOpen en esta tarjeta, y
 * con 300-450 programas por juego, 30-40 s de "Loading shaders..." en cada
 * arranque (Super Mario 3D Land, 26-10). Ahora todos los programas del juego
 * van tambien en un solo fichero, juego_<id>.pak, que se lee de una vez. Lo que
 * falte en el paquete se lee suelto, como antes, y el paquete se reescribe.
 * Cada entrada se valida igual que un fichero suelto (FromPreloaded compara la
 * cabecera con la fuente), y las que ya no estan en la carpeta (programas
 * borrados a proposito, ver PurgeUnoptimizedPrograms) se descartan.
 */
constexpr u32 kPackMagic = 0x4B505A41u; // "AZPK"
constexpr u32 kPackVersion = 1;

std::string PackPath(u64 program_id) {
    return fmt::format("{}/juego_{:016x}.pak", kCacheDir, program_id);
}

void ReadPack(const std::string& path, std::unordered_map<std::string, std::vector<u8>>& out) {
    const SceUID fd = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);
    if (fd < 0) {
        return;
    }
    const SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    std::vector<u8> data;
    if (size > 12 && size < 64 * 1024 * 1024) {
        data.resize(static_cast<std::size_t>(size));
        if (sceIoRead(fd, data.data(), static_cast<SceSize>(data.size())) !=
            static_cast<int>(data.size())) {
            data.clear();
        }
    }
    sceIoClose(fd);
    if (data.size() < 12) {
        return;
    }
    u32 header[3];
    std::memcpy(header, data.data(), sizeof(header));
    if (header[0] != kPackMagic || header[1] != kPackVersion) {
        return;
    }
    std::size_t at = sizeof(header);
    for (u32 i = 0; i < header[2]; i++) {
        u16 path_len = 0;
        u32 file_len = 0;
        if (at + sizeof(path_len) > data.size()) {
            break;
        }
        std::memcpy(&path_len, data.data() + at, sizeof(path_len));
        at += sizeof(path_len);
        if (at + path_len + sizeof(file_len) > data.size()) {
            break;
        }
        std::string entry(reinterpret_cast<const char*>(data.data() + at), path_len);
        at += path_len;
        std::memcpy(&file_len, data.data() + at, sizeof(file_len));
        at += sizeof(file_len);
        if (at + file_len > data.size()) {
            break;
        }
        out.emplace(std::move(entry),
                    std::vector<u8>(data.begin() + static_cast<std::ptrdiff_t>(at),
                                    data.begin() + static_cast<std::ptrdiff_t>(at + file_len)));
        at += file_len;
    }
}

/// Con g_preload_mutex cogido: todo lo precargado, a un fichero temporal y
/// despues en su sitio (si se corta a medias, el paquete viejo sigue valiendo).
void WritePackLocked(const std::string& path) {
    std::vector<u8> data(12);
    u32 count = 0;
    for (const auto& [entry, file] : g_preloaded) {
        if (entry.size() > 0xFFFF) {
            continue;
        }
        const u16 path_len = static_cast<u16>(entry.size());
        const u32 file_len = static_cast<u32>(file.size());
        const std::size_t at = data.size();
        data.resize(at + sizeof(path_len) + path_len + sizeof(file_len) + file_len);
        std::memcpy(data.data() + at, &path_len, sizeof(path_len));
        std::memcpy(data.data() + at + sizeof(path_len), entry.data(), path_len);
        std::memcpy(data.data() + at + sizeof(path_len) + path_len, &file_len, sizeof(file_len));
        std::memcpy(data.data() + at + sizeof(path_len) + path_len + sizeof(file_len),
                    file.data(), file_len);
        count++;
    }
    const u32 header[3] = {kPackMagic, kPackVersion, count};
    std::memcpy(data.data(), header, sizeof(header));
    const std::string temp = path + ".tmp";
    const SceUID fd = sceIoOpen(temp.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) {
        return;
    }
    const bool ok = sceIoWrite(fd, data.data(), static_cast<SceSize>(data.size())) ==
                    static_cast<int>(data.size());
    sceIoClose(fd);
    if (!ok) {
        sceIoRemove(temp.c_str());
        return;
    }
    sceIoRemove(path.c_str());
    sceIoRename(temp.c_str(), path.c_str());
}

std::string g_game_prefix;

std::string CurrentGameCachePrefix() {
    const std::lock_guard lock{g_preload_mutex};
    return g_game_prefix;
}


/**
 * LOS DE VERTICES EN O2 (0.4.1.1). El banco del compilador (27-10) compilo a O2
 * los siete shaders de vertices que tenemos guardados de Zafiro Alfa y Super
 * Mario 3D Land, incluido el que da "fatal internal error" a O3 (e1): todos
 * bien, con binarios un 23-35 % mas pequenos que a O1 y algo mas rapidos de
 * compilar. Y el chip de la Vita tarda en proporcion al tamano del programa
 * de vertices (Mario 3D Land: 3,1 ms + 0,07 us por KB de trabajo, r2 1,00).
 *
 * Su binario lleva OTRA identidad en la cache (las dos huellas con kVsO2Salt):
 * los O1 de antes no se usan y cada uno se recompila una vez a O2. Si alguno
 * diera error interno a O2, va a la lista de cg_o2_malos.bin y desde entonces
 * se compila a O1 (en la misma compilacion, tras recargar el compilador), en
 * vez de acabar en la lista negra y por la CPU como en 0.4.0.1 con O3.
 *
 * APAGADO POR DEFECTO (0.4.1.2). En Super Mario 3D Land, jugando, los
 * programas reales salieron MAS grandes a O2 ("KB de trabajo vs" por
 * superficie 162.825 -> 219.738) y el chip tardo un 6 % mas por superficie:
 * los del banco eran de otra version del traductor. Los binarios O1 siguen en
 * la cache con su identidad, asi que apagarlo no recompila nada.
 */
std::atomic<bool> g_vs_o2{false};
constexpr u64 kVsO2Salt = 0x4F32A5C3D1E07B69ull;
std::string g_kO2BadPath_storage;
const char* const kO2BadPath = CacheFile(g_kO2BadPath_storage, "cg_o2_malos.bin");
std::mutex g_o2_bad_mutex;
std::unordered_set<u64> g_o2_bad;
bool g_o2_bad_loaded = false;

bool IsO2Bad(u64 hash_city) {
    const std::lock_guard lock{g_o2_bad_mutex};
    if (!g_o2_bad_loaded) {
        g_o2_bad_loaded = true;
        if (std::FILE* in = std::fopen(kO2BadPath, "rb")) {
            u64 hash = 0;
            while (std::fread(&hash, sizeof(hash), 1, in) == 1) {
                g_o2_bad.insert(hash);
            }
            std::fclose(in);
        }
    }
    return g_o2_bad.count(hash_city) != 0;
}

void AddO2Bad(u64 hash_city) {
    (void)IsO2Bad(hash_city);
    const std::lock_guard lock{g_o2_bad_mutex};
    if (!g_o2_bad.insert(hash_city).second) {
        return;
    }
    if (std::FILE* out = std::fopen(kO2BadPath, "ab")) {
        std::fwrite(&hash_city, sizeof(hash_city), 1, out);
        std::fclose(out);
    }
}

/// Este fuente de vertices se compila (y se busca en la cache) a O2.
bool VsUsesO2(SceShaccCgTargetProfile profile, u64 hash_city) {
    return profile == SCE_SHACCCG_PROFILE_VP && g_vs_o2.load(std::memory_order_relaxed) &&
           !IsO2Bad(hash_city);
}

/// La identidad en la cache de un binario a O2 (ver kVsO2Salt).
void SaltForO2(u64& hash_city, u64& hash_fnv) {
    hash_city ^= kVsO2Salt;
    hash_fnv ^= kVsO2Salt;
}

/**
 * LOS DE VERTICES NUEVOS, A O3 (0.4.3.7, ajuste vs_o3=, encendido). Banco del
 * 03-11 con las 41 fuentes de vertices de Kirby TD, Luigi's Mansion y Zafiro
 * Alfa tal como las genera 0.4.3.6: las 41 compilan a O3 sin un error interno,
 * en un 15 % menos de tiempo en total (los grandes, que son los que hacen
 * esperar, un 20-26 % menos; algunos pequenos tardan hasta medio segundo mas)
 * y con binarios un 46 % mas pequenos (-52 % de instrucciones). El chip tarda
 * en proporcion al binario de vertices (ver kVsO2Salt), asi que compensa de
 * sobra lo que crecen las fuentes unificadas (kCgUnifiedInputs).
 *
 * A diferencia de O2, el binario se guarda con la MISMA identidad que a O1: lo
 * que ya hay en la cache de la tarjeta sigue valiendo tal cual (nadie recompila
 * sus juegos) y solo lo nuevo sale a O3. Si una fuente da error interno a O3 (en
 * 0.4.0.1 cuatro de Zafiro lo daban, con otro traductor) o el proceso muere
 * compilandola a O3, va a cg_o3_malos.bin y desde entonces se compila a O1, en
 * la misma compilacion si fue un error interno. O2 (vs_o2=1) manda sobre esto.
 */
std::atomic<bool> g_vs_o3{true};
std::string g_kO3BadPath_storage;
const char* const kO3BadPath = CacheFile(g_kO3BadPath_storage, "cg_o3_malos.bin");
std::mutex g_o3_bad_mutex;
std::unordered_set<u64> g_o3_bad;
bool g_o3_bad_loaded = false;

bool IsO3Bad(u64 hash_city) {
    const std::lock_guard lock{g_o3_bad_mutex};
    if (!g_o3_bad_loaded) {
        g_o3_bad_loaded = true;
        if (std::FILE* in = std::fopen(kO3BadPath, "rb")) {
            u64 hash = 0;
            while (std::fread(&hash, sizeof(hash), 1, in) == 1) {
                g_o3_bad.insert(hash);
            }
            std::fclose(in);
        }
    }
    return g_o3_bad.count(hash_city) != 0;
}

void AddO3Bad(u64 hash_city) {
    (void)IsO3Bad(hash_city);
    const std::lock_guard lock{g_o3_bad_mutex};
    if (!g_o3_bad.insert(hash_city).second) {
        return;
    }
    if (std::FILE* out = std::fopen(kO3BadPath, "ab")) {
        std::fwrite(&hash_city, sizeof(hash_city), 1, out);
        std::fclose(out);
    }
}

/// Este fuente de vertices se compila a O3 (si no esta ya en la cache).
bool VsUsesO3(SceShaccCgTargetProfile profile, u64 hash_city) {
    return profile == SCE_SHACCCG_PROFILE_VP && g_vs_o3.load(std::memory_order_relaxed) &&
           !VsUsesO2(profile, hash_city) && !IsO3Bad(hash_city);
}

/**
 * FRAGMENTOS CON fastprecision (0.4.1.9, ajuste fs_rapidos=). El 80-90 % del
 * chip en Mario 3D Land y Zafiro es el shader de fragmentos (experimento
 * fs_plano de 0.4.1.5). Con esto el compilador puede usar media precision
 * donde lo vea seguro. Su binario lleva otra identidad en la cache, como los
 * de O2: se recompila cada uno una vez.
 */
std::atomic<bool> g_fs_fast{false};
constexpr u64 kFsFastSalt = 0x7A11F0E5C3B2D194ull;

bool FsUsesFast(SceShaccCgTargetProfile profile) {
    return profile == SCE_SHACCCG_PROFILE_FP && g_fs_fast.load(std::memory_order_relaxed);
}

void SaltForFast(u64& hash_city, u64& hash_fnv) {
    hash_city ^= kFsFastSalt;
    hash_fnv ^= kFsFastSalt;
}

bool CgCacheContains(SceShaccCgTargetProfile profile, const std::string& source) {
    u64 hash = Common::ComputeHash64(source.data(), source.size());
    if (VsUsesO2(profile, hash)) {
        u64 unused = 0;
        SaltForO2(hash, unused);
    }
    if (FsUsesFast(profile)) {
        u64 unused = 0;
        SaltForFast(hash, unused);
    }
    const std::string path = CachePath(hash, profile);
    const std::lock_guard lock{g_preload_mutex};
    return g_preloaded.count(path) != 0 || g_card_files.count(path) != 0;
}

u32 PreloadCgCache(u64 program_id, const std::function<void(u32, u32)>& progress) {
    PurgeUnoptimizedPrograms();
    g_start_notes.store(0, std::memory_order_relaxed);
    g_time_notes.store(0, std::memory_order_relaxed);
    g_session_start_us.store(Common::VitaMicros(), std::memory_order_relaxed);
    g_cache_notes = 0;
    g_mem_notes = 0;
    // Nada de lo anterior sirve para otro juego.
    std::vector<std::string> paths;
    {
        const std::lock_guard lock{g_preload_mutex};
        g_preloaded.clear();
        g_game_list.clear();
        g_game_list_path = fmt::format("{}/juego_{:016x}.lst", kCacheDir, program_id);
        g_game_prefix = fmt::format("{}/juego_{:016x}", kCacheDir, program_id);
    }
    sceIoMkdir(kCacheDir, 0777);
    const SceUID list_fd = sceIoOpen(g_game_list_path.c_str(), SCE_O_RDONLY, 0);
    if (list_fd >= 0) {
        std::string text;
        char chunk[4096];
        int read = 0;
        while ((read = sceIoRead(list_fd, chunk, sizeof(chunk))) > 0) {
            text.append(chunk, static_cast<std::size_t>(read));
        }
        sceIoClose(list_fd);
        std::size_t start = 0;
        while (start < text.size()) {
            std::size_t end = text.find('\n', start);
            if (end == std::string::npos) {
                end = text.size();
            }
            if (end > start) {
                paths.emplace_back(text.substr(start, end - start));
            }
            start = end + 1;
        }
    }
    /**
     * Tope de memoria: los programas de un juego pesan de unos KB a decenas de
     * KB cada uno, pero la lista crece con cada partida. Lo que no quepa se
     * lee de la tarjeta como antes.
     */
    constexpr std::size_t kMaxPreloadBytes = 24u * 1024u * 1024u;
    std::size_t total_bytes = 0;
    u32 loaded = 0;
    const u32 total = static_cast<u32>(paths.size());
    const unsigned long long preload_begin = Common::VitaMicros();
    const std::string pack_path = PackPath(program_id);
    std::unordered_map<std::string, std::vector<u8>> pack;
    ReadPack(pack_path, pack);
    u32 from_pack = 0;
    u32 loose = 0;
    for (u32 i = 0; i < total; i++) {
        if (progress && (i % 8) == 0) {
            progress(i, total);
        }
        const std::string& path = paths[i];
        if (auto packed = pack.find(path); packed != pack.end()) {
            if (total_bytes + packed->second.size() > kMaxPreloadBytes) {
                continue;
            }
            total_bytes += packed->second.size();
            loaded++;
            from_pack++;
            const std::lock_guard lock{g_preload_mutex};
            g_game_list.insert(path);
            g_preloaded.emplace(path, std::move(packed->second));
            continue;
        }
        loose++;
        const SceUID fd = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);
        if (fd < 0) {
            continue;
        }
        const SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
        sceIoLseek(fd, 0, SCE_SEEK_SET);
        bool ok = size > static_cast<SceOff>(sizeof(CacheHeader)) &&
                  size < 4 * 1024 * 1024 &&
                  total_bytes + static_cast<std::size_t>(size) <= kMaxPreloadBytes;
        std::vector<u8> file;
        if (ok) {
            file.resize(static_cast<std::size_t>(size));
            ok = sceIoRead(fd, file.data(), static_cast<SceSize>(file.size())) ==
                 static_cast<int>(file.size());
        }
        sceIoClose(fd);
        if (!ok) {
            continue;
        }
        total_bytes += file.size();
        loaded++;
        const std::lock_guard lock{g_preload_mutex};
        g_game_list.insert(path);
        g_preloaded.emplace(path, std::move(file));
    }
    if (progress) {
        progress(total, total);
    }
    Common::VitaNote("gxm cache",
                     fmt::format("precarga: {} de {} shaders, {} KB en {} ms ({} del paquete, "
                                 "{} sueltos)",
                                 loaded, total, total_bytes / 1024,
                                 (Common::VitaMicros() - preload_begin) / 1000, from_pack, loose)
                         .c_str());
    LoadVsIndex(program_id);
    {
        const unsigned long long list_begin = Common::VitaMicros();
        std::unordered_set<std::string> files;
        const SceUID dir = sceIoDopen(kCacheDir);
        if (dir >= 0) {
            SceIoDirent entry{};
            while (sceIoDread(dir, &entry) > 0) {
                files.insert(fmt::format("{}/{}", kCacheDir, entry.d_name));
            }
            sceIoDclose(dir);
        }
        const std::size_t count = files.size();
        u32 dropped = 0;
        {
            const std::lock_guard lock{g_preload_mutex};
            g_card_files = std::move(files);
            g_card_index_ready = dir >= 0;
            if (g_card_index_ready) {
                for (auto it = g_preloaded.begin(); it != g_preloaded.end();) {
                    if (g_card_files.count(it->first) == 0) {
                        it = g_preloaded.erase(it);
                        dropped++;
                    } else {
                        ++it;
                    }
                }
            }
            // El paquete, al dia: si algo se leyo suelto o se ha descartado.
            if (loose != 0 || dropped != 0 || from_pack != pack.size()) {
                WritePackLocked(pack_path);
            }
        }
        if (dropped != 0) {
            Common::VitaNote("gxm cache", fmt::format("paquete: {} programas que ya no estan "
                                                      "en la carpeta, fuera",
                                                      dropped)
                                              .c_str());
        }
        Common::VitaNote("gxm cache", fmt::format("carpeta: {} ficheros en {} ms", count,
                                                  (Common::VitaMicros() - list_begin) / 1000)
                                          .c_str());
    }
    return loaded;
}

const SceShaccCgCompileOutput* LoadCgCache(SceShaccCgTargetProfile profile, const char* name,
                                           const char* source, bool from_card) {
    const std::size_t source_size = std::strlen(source);
    CacheHeader key{};
    key.magic = kCacheMagic;
    key.version = kCacheVersion;
    key.profile = static_cast<u32>(profile);
    key.source_size = static_cast<u32>(source_size);
    key.hash_city = Common::ComputeHash64(source, source_size);
    key.hash_fnv = Fnv1a64(source, source_size);
    if (VsUsesO2(profile, key.hash_city)) {
        SaltForO2(key.hash_city, key.hash_fnv);
    }
    if (FsUsesFast(profile)) {
        SaltForFast(key.hash_city, key.hash_fnv);
    }
    const unsigned long long begin_us = Common::VitaMicros();
    const std::string path = CachePath(key.hash_city, profile);
    const SceShaccCgCompileOutput* cached = nullptr;
    if (from_card) {
        cached = LoadCached(key, path);
    } else {
        const std::lock_guard lock{g_preload_mutex};
        const auto it = g_preloaded.find(path);
        if (it != g_preloaded.end()) {
            cached = FromPreloaded(key, it->second);
        }
    }
    if (cached != nullptr) {
        NoteCache(fmt::format("{} leido de la cache en {} us", name,
                              Common::VitaMicros() - begin_us));
    }
    return cached;
}

const SceShaccCgCompileOutput* LoadCgCacheIndexed(u64 key, u32* variant) {
    VsIndexRecord record{};
    {
        const std::lock_guard lock{g_preload_mutex};
        const auto it = g_vs_index.find(key);
        if (it == g_vs_index.end()) {
            return nullptr;
        }
        record = it->second;
    }
    CacheHeader want{};
    want.magic = kCacheMagic;
    want.version = kCacheVersion;
    want.profile = static_cast<u32>(SCE_SHACCCG_PROFILE_VP);
    want.source_size = record.source_size;
    want.hash_city = record.hash_city;
    want.hash_fnv = record.hash_fnv;
    if (VsUsesO2(SCE_SHACCCG_PROFILE_VP, record.hash_city)) {
        SaltForO2(want.hash_city, want.hash_fnv);
    }
    const SceShaccCgCompileOutput* output =
        LoadCached(want, CachePath(want.hash_city, SCE_SHACCCG_PROFILE_VP));
    if (output != nullptr) {
        *variant = record.variant;
    }
    return output;
}

void AddVsIndex(u64 key, const std::string& source, u32 variant) {
    VsIndexRecord record{};
    record.key = key;
    record.hash_city = Common::ComputeHash64(source.data(), source.size());
    record.hash_fnv = Fnv1a64(source.data(), source.size());
    record.source_size = static_cast<u32>(source.size());
    record.variant = variant;
    const std::lock_guard lock{g_preload_mutex};
    if (g_vs_index_path.empty()) {
        return;
    }
    const auto it = g_vs_index.find(key);
    if (it != g_vs_index.end() && std::memcmp(&it->second, &record, sizeof(record)) == 0) {
        return;
    }
    g_vs_index[key] = record;
    if (g_vs_index_fd < 0) {
        // Con otra cabecera (u otra compilacion del traductor) se empieza de cero.
        g_vs_index_fd = sceIoOpen(g_vs_index_path.c_str(),
                                  SCE_O_WRONLY | SCE_O_CREAT |
                                      (g_vs_index_valid ? SCE_O_APPEND : SCE_O_TRUNC),
                                  0777);
        if (g_vs_index_fd < 0) {
            return;
        }
        if (!g_vs_index_valid) {
            const VsIndexHeader header = CurrentVsIndexHeader();
            if (sceIoWrite(g_vs_index_fd, &header, sizeof(header)) !=
                static_cast<int>(sizeof(header))) {
                sceIoClose(g_vs_index_fd);
                g_vs_index_fd = -1;
                return;
            }
            g_vs_index_valid = true;
        }
    }
    sceIoWrite(g_vs_index_fd, &record, sizeof(record));
}

namespace {
const SceShaccCgCompileOutput* CompileCgOnce(SceShaccCgTargetProfile profile, const char* name,
                                             const char* source, bool& retry_at_o1);
} // Anonymous namespace

/**
 * Compila (o lee de la cache) y, si un shader de vertices dio error interno a
 * O2, lo vuelve a intentar a O1 en el acto (0.4.1.1, ver g_vs_o2): para
 * entonces ya esta en la lista de O2 malos y el compilador recargado.
 */
const SceShaccCgCompileOutput* CompileCg(SceShaccCgTargetProfile profile, const char* name,
                                         const char* source) {
    bool retry_at_o1 = false;
    const SceShaccCgCompileOutput* output = CompileCgOnce(profile, name, source, retry_at_o1);
    if (output == nullptr && retry_at_o1) {
        bool again = false;
        output = CompileCgOnce(profile, name, source, again);
    }
    return output;
}

namespace {
const SceShaccCgCompileOutput* CompileCgOnce(SceShaccCgTargetProfile profile, const char* name,
                                             const char* source, bool& retry_at_o1) {
    retry_at_o1 = false;
    // Cache en la tarjeta (0.1.5.8): si este codigo fuente ya se compilo
    // alguna vez, el binario se lee de disco. Ver CacheHeader. Sin el cerrojo
    // del compilador (0.1.9.6): no hay que esperar a que acabe otra compilacion.
    const std::size_t source_size = std::strlen(source);
    CacheHeader key{};
    key.magic = kCacheMagic;
    key.version = kCacheVersion;
    key.profile = static_cast<u32>(profile);
    key.source_size = static_cast<u32>(source_size);
    key.hash_city = Common::ComputeHash64(source, source_size);
    key.hash_fnv = Fnv1a64(source, source_size);
    const u64 plain_hash_city = key.hash_city;
    const bool o2 = VsUsesO2(profile, key.hash_city);
    if (o2) {
        SaltForO2(key.hash_city, key.hash_fnv);
    }
    // Ver g_vs_o3 (0.4.3.7): sin sal, con la misma identidad que a O1.
    const bool o3 = !o2 && VsUsesO3(profile, plain_hash_city);
    const bool fast = FsUsesFast(profile);
    if (fast) {
        SaltForFast(key.hash_city, key.hash_fnv);
    }
    const std::string cache_path = CachePath(key.hash_city, profile);
    {
        const unsigned long long begin_us = Common::VitaMicros();
        if (const SceShaccCgCompileOutput* cached = LoadCached(key, cache_path)) {
            NoteCache(fmt::format("{} leido de la cache en {} us", name,
                                  Common::VitaMicros() - begin_us));
            return cached;
        }
    }
    const std::lock_guard lock{g_cg_mutex};
    LoadBadSources();
    const BadSource this_source{key.hash_city, key.hash_fnv};
    if (g_bad_sources.count(this_source) != 0) {
        static u32 blacklisted_notes = 0;
        if (blacklisted_notes < 8) {
            blacklisted_notes++;
            Common::VitaNote("gxm shader",
                             fmt::format("{}: en la lista negra, no se compila ({} bytes)", name,
                                         source_size)
                                 .c_str());
        }
        return nullptr;
    }
    /**
     * NO EMPEZAR SIN MEMORIA DE SOBRA (0.1.8.1). Una compilacion puede pedir
     * decenas de MB por el camino, y quedarse sin ellos a mitad tumba el
     * compilador por dentro. Si no hay kMinFreeToCompile libres, primero se
     * recupera lo que retenga el compilador; si ni asi, este shader no se
     * compila ahora (el lote se dibuja por el otro camino) y se reintentara
     * cuando lo pida otro lote.
     */
    const bool vertex = profile == SCE_SHACCCG_PROFILE_VP;
    const std::size_t min_free = vertex ? kMinFreeToCompileVs : kMinFreeToCompile;
    /**
     * UNA MEDIDA DEL HEAP POR COMPILACION (0.4.2.5). mallinfo() de newlib recorre
     * todos los bloques libres del heap, y con el del emulador (180 MB en uso,
     * lleno de restos) eso no es gratis; aqui se llamaba cinco veces antes de
     * cada compilacion (el minimo, el presupuesto, la nota, la marca) y eso caia
     * en "tarjeta antes": 0,47 s por shader en Luigi's Mansion con la cache
     * vacia (31-10). Una sola, cronometrada ("medir el heap" en la nota).
     */
    const unsigned long long heap_probe_begin_us = Common::VitaMicros();
    const std::size_t heap_before = HeapInUse();
    const std::size_t free_before =
        _newlib_heap_size_user > heap_before ? _newlib_heap_size_user - heap_before : 0;
    const unsigned long long heap_probe_us = Common::VitaMicros() - heap_probe_begin_us;
    if (free_before < min_free) {
        static u32 skip_notes = 0;
        if (skip_notes < 4) {
            skip_notes++;
            Common::VitaNote("gxm mem", fmt::format("{}: sin compilar, solo quedan {} KB de heap",
                                                    name, free_before / 1024)
                                            .c_str());
        }
        return nullptr;
    }
    if (!EnsureCgReady()) {
        return nullptr;
    }
    const unsigned long long compile_begin_us = Common::VitaMicros();
    /**
     * CPU DE VERDAD FRENTE A RELOJ (0.3.0.1). Los de vertices grandes tardan
     * 70-120 s en Zafiro Alfa con el compilador ya solo en su nucleo
     * (0.2.3.8): o le siguen quitando la CPU, o el compilador es asi de lento
     * con ellos. runClocks del hilo dice cuanto ha corrido de verdad, y las
     * reservas, si se le va el tiempo en pedir memoria.
     */
    SceKernelThreadInfo info_before{};
    info_before.size = sizeof(info_before);
    const bool have_info = sceKernelGetThreadInfo(sceKernelGetThreadId(), &info_before) >= 0;
    g_alloc_calls = 0;
    g_alloc_total = 0;
    g_alloc_reused = 0;
    g_free_calls = 0;
    g_alloc_sample_us = 0;
    g_free_sample_us = 0;
    g_arena_new_chunks = 0;
    g_source.fileName = name;
    g_source.text = source;
    g_source.size = static_cast<SceUInt32>(std::strlen(source));

    SceShaccCgCompileOptions options{};
    sceShaccCgInitializeCompileOptions(&options);
    options.mainSourceFile = name;
    options.targetProfile = profile;
    options.entryFunctionName = "main";
    /**
     * Los de vertices, con poca optimizacion (0.1.9.6). Son los programas de
     * la PICA traducidos: miles de lineas que el compilador tardaba de varios
     * segundos a mas de medio minuto en optimizar, con picos de hasta 27 MB y
     * hasta 20 MB retenidos despues. En la GPU de la Vita los vertices de un
     * juego de 3DS no cuestan casi nada; lo caro era compilarlos. Los de
     * fragmentos (se ejecutan por pixel) siguen con la optimizacion de serie.
     */
    /**
     * O1 EN LOS DE VERTICES (0.1.9.6). En 0.4.0.1 se probo O3, que con la
     * lectura de uniforms corta compila antes y deja binarios la mitad de
     * grandes (banco: 9 s y 7,5 KB frente a 12 s y 15,7 KB), pero en Zafiro Alfa
     * cuatro shaders dieron "fatal internal error" en O3 y no en O1 (banco_cg4), y
     * entonces un error interno dejaba el compilador roto el resto de la sesion.
     * Desde 0.4.3.7, O3 otra vez (ver g_vs_o3): el compilador ya se recarga tras
     * un error interno y el que falle a O3 se repite a O1 en el acto.
     */
    if (profile == SCE_SHACCCG_PROFILE_VP) {
        // Ver g_vs_o2 (0.4.1.1) y g_vs_o3 (0.4.3.7).
        options.optimizationLevel = o2 ? 2 : (o3 ? 3 : 1);
    }
    if (fast) {
        // Ver g_fs_fast (0.4.1.9).
        options.useFastprecision = 1;
    }
    // Nada en O0: los de vertices desde 0.2.1.6 (uno colgo la GPU en 0.2.1.3),
    // los de fragmentos desde 0.3.7.8 (ver PurgeUnoptimizedPrograms).

    g_compile_budget = free_before - kEmulatorReserve;
    g_compile_bytes = 0;
    g_compile_over_budget = false;
    g_peak_bytes = g_live_bytes;
    const std::size_t live_before = g_live_bytes;
    g_compiles_since_load++;
    const bool fresh_compiler = g_compiles_since_load == 1;
    g_tracking = true;
    // Que se compila y de que tamano, a crash.txt (0.1.9.1): en 0.1.9.0 el
    // juego se quedaba congelado dentro de una compilacion sin saber cual.
    // Hasta 200 (eran 24): con menos, el que tumba el compilador no salia.
    if (g_start_notes.fetch_add(1, std::memory_order_relaxed) < 200) {
        Common::VitaNote("gxm compila", fmt::format("{}: {} bytes de codigo, heap libre {} KB, O{}",
                                                    name, g_source.size, free_before / 1024,
                                                    options.optimizationLevel)
                                            .c_str());
    }
    const SceShaccCgCompileOutput* output = nullptr;
    // Lo que tarda en la tarjeta lo de antes de compilar (0.3.1.4): la nota y
    // las dos marcas, en el tiempo de reloj de "compilado en".
    const unsigned long long marker_us = Common::VitaMicros() - compile_begin_us;
    const unsigned long long marker_begin_us = Common::VitaMicros();
    {
        CompilingMarker compiling{};
        compiling.source = BadSource{key.hash_city, key.hash_fnv};
        compiling.profile = static_cast<u32>(profile);
        compiling.optimization = static_cast<u32>(options.optimizationLevel);
        compiling.source_size = static_cast<u32>(source_size);
        compiling.free_kb = static_cast<u32>(free_before / 1024);
        compiling.start_s = FsNowSeconds();
        compiling.alive_s = compiling.start_s;
        compiling.magic = kMarkerMagic;
        compiling.flags = g_gpu_hung.load(std::memory_order_relaxed) ? kMarkerGpuHung : 0u;
        // Ver g_source_fd: en su sitio, sin abrir ni truncar.
        if (g_source_fd < 0) {
            g_source_fd = sceIoOpen(kCompilingSourcePath, SCE_O_WRONLY | SCE_O_CREAT, 0777);
        }
        if (g_source_fd >= 0) {
            sceIoPwrite(g_source_fd, source, static_cast<SceSize>(source_size), 0);
        }
        // La CPU de ESTA compilacion la mide el balanceador (ver KeepMarkerAlive).
        const bool on_worker = g_worker_running && pthread_equal(pthread_self(), g_worker_thread);
        g_marker_start_clocks.store(have_info && on_worker ? info_before.runClocks : 0ull,
                                    std::memory_order_relaxed);
        WriteCompilingMarker(compiling);
    }
    const unsigned long long card_us = marker_us + (Common::VitaMicros() - marker_begin_us);
    if (g_worker_running && pthread_equal(pthread_self(), g_worker_thread)) {
        output = sceShaccCgCompileProgram(&options, &g_callbacks, 0);
    } else {
        const Common::ScopedVitaStage stage{"compilando shader"};
        output = sceShaccCgCompileProgram(&options, &g_callbacks, 0);
    }
    g_tracking = false;
    FlushSmallFree();
    g_compile_budget = static_cast<std::size_t>(-1);
    if (g_compile_over_budget) {
        static u32 budget_notes = 0;
        if (budget_notes < 4) {
            budget_notes++;
            Common::VitaNote("gxm mem", fmt::format("{}: el compilador agoto su presupuesto "
                                                    "({} KB); la compilacion se corta",
                                                    name, g_compile_bytes / 1024)
                                            .c_str());
        }
    }
    if (output == nullptr || output->programData == nullptr) {
        /**
         * EL MENSAJE DEL COMPILADOR VA A crash.txt, no solo al registro.
         *
         * Antes aqui solo se anotaba el NOMBRE del fichero ("azahar_gxm_f.cg")
         * y los diagnosticos iban a LOG_ERROR, que desde la consola no se ve.
         * O sea que un shader que no compilaba dejaba escrito que no compilaba
         * y ni una pista de por que, y sin esa pista no hay forma de arreglarlo
         * sin un depurador. Se anota el primero, que es el que dice donde esta
         * el error; los demas suelen ser consecuencia suya.
         */
        char note[192];
        int noted = 0;
        bool internal = false;
        if (output != nullptr) {
            for (int i = 0; i < output->diagnosticCount; i++) {
                if (output->diagnostics[i].message != nullptr &&
                    std::strstr(output->diagnostics[i].message, "internal error") != nullptr) {
                    internal = true;
                }
            }
        }
        DumpFailedSource(name, source, output, internal);
        if (output != nullptr) {
            for (int i = 0; i < output->diagnosticCount && i < 4; i++) {
                const char* text = output->diagnostics[i].message != nullptr
                                       ? output->diagnostics[i].message
                                       : "(sin mensaje)";
                LOG_ERROR(Render, "GXM {}: {}", name, text);
                /**
                 * Los DOS primeros van a crash.txt, no solo el primero.
                 *
                 * Cada arreglo de un shader generado cuesta una prueba en
                 * consola, y si hay dos errores independientes verlos de uno en
                 * uno cuesta dos vueltas. Del tercero en adelante ya suelen ser
                 * consecuencia de los anteriores y solo ensucian el fichero.
                 */
                if (noted < 2 && output->diagnostics[i].message != nullptr) {
                    const auto written =
                        fmt::format_to_n(note, sizeof(note) - 1, "{}: {}", name, text);
                    *written.out = 0;
                    Common::VitaNote("gxm shader", note);
                    noted++;
                }
            }
        } else {
            LOG_ERROR(Render, "GXM: el compilador no devolvio nada para {}", name);
        }
        if (noted == 0) {
            const auto written =
                fmt::format_to_n(note, sizeof(note) - 1, "{}: sin diagnostico", name);
            *written.out = 0;
            Common::VitaNote("gxm shader", note);
        }
        if (output != nullptr) {
            sceShaccCgDestroyCompileOutput(output);
        }
        ClearCompilingMarker();
        if (internal && (o2 || o3)) {
            // Ver g_vs_o2 (0.4.1.1) y g_vs_o3 (0.4.3.7): no a la lista negra, sino a O1.
            if (o2) {
                AddO2Bad(plain_hash_city);
            } else {
                AddO3Bad(plain_hash_city);
            }
            Common::VitaNote("gxm shader", fmt::format("{}: error interno a O{}, se "
                                                     "compila a O1",
                                                     name, o2 ? 2 : 3)
                                             .c_str());
            RecoverFromInternalError(false);
            retry_at_o1 = true;
        } else if (internal) {
            /**
             * Ese codigo no se vuelve a mandar (ni en otra sesion) y el
             * compilador se recarga limpio para el siguiente. De 0.3.2.1 a
             * 0.3.7.7 uno de fragmentos se reintentaba sin optimizar, y ese
             * binario O0 colgaba la GPU (ver PurgeUnoptimizedPrograms): ahora va
             * a la lista negra como los demas.
             */
            if (fresh_compiler) {
                StoreBadSource(this_source);
            } else {
                g_bad_sources.insert(this_source);
            }
            RecoverFromInternalError();
        } else if (g_compile_over_budget) {
            // Sin memoria a mitad de compilacion su estado puede quedar a
            // medias: se descarga y se recarga limpio, como tras un error
            // interno, pero el codigo NO va a la lista negra (no lo rompio el
            // shader, sino la falta de heap en ese momento).
            RecoverFromInternalError();
        } else {
            // Un error normal (de sintaxis) deja el compilador sano: solo se
            // suelta su memoria. Con el error interno no se le llama: el
            // modulo esta roto y RecoverFromInternalError ya lo ha descargado.
            NoteCompileMemory(name, heap_before, live_before);
        }
        return nullptr;
    }
    // Lo que tarda compilar (para saber cuanto ahorra la cache) y a la cache.
    const unsigned long long compile_ms = (Common::VitaMicros() - compile_begin_us) / 1000;
    NoteCache(fmt::format("{} compilado en {} ms", name, compile_ms));
    {
        // Aparte de las notas de la cache, que se gastan al arrancar (0.2.1.1):
        // es lo que tarda en aparecer un modelo con los shaders asincronos.
        if (g_time_notes.fetch_add(1, std::memory_order_relaxed) < 60) {
            SceKernelThreadInfo info_after{};
            info_after.size = sizeof(info_after);
            std::string cpu = "-";
            if (have_info && sceKernelGetThreadInfo(sceKernelGetThreadId(), &info_after) >= 0) {
                cpu = fmt::format("{} ms, nucleo {}, {} expulsiones",
                                  (info_after.runClocks - info_before.runClocks) / 1000,
                                  info_after.currentCpuId,
                                  info_after.threadPreemptCount - info_before.threadPreemptCount);
            }
            Common::VitaNote("gxm compila",
                             fmt::format("{}: compilado en {} ms (CPU {}; {} reservas, {} sin "
                                         "newlib, {} MB pedidos, reservar ~{} ms, liberar ~{} "
                                         "ms, {} trozos propios ({} nuevos); medir el heap {} ms, "
                                         "tarjeta antes {} ms)",
                                         name, compile_ms, cpu, g_alloc_calls, g_alloc_reused,
                                         g_alloc_total >> 20, g_alloc_sample_us * 64 / 1000,
                                         g_free_sample_us * 64 / 1000, g_arena_chunks,
                                         g_arena_new_chunks, heap_probe_us / 1000, card_us / 1000)
                                 .c_str());
        }
    }
    StoreCached(key, cache_path, *output);
    // 0.1.7.9: la salida pasa a un bloque nuestro y el compilador suelta todo
    // lo suyo. Si no hay memoria ni para la copia, se devuelve la original
    // (como antes de 0.1.7.9) y el compilador se queda como estaba.
    const SceShaccCgCompileOutput* copy = CopyOutput(*output, key.hash_city);
    if (copy == nullptr) {
        ClearCompilingMarker();
        return output;
    }
    sceShaccCgDestroyCompileOutput(output);
    ClearCompilingMarker();
    NoteCompileMemory(name, heap_before, live_before);
    /**
     * TOPE DE LO QUE RETIENE EL COMPILADOR (0.1.9.1). Desde 0.1.8.5 no se le
     * pide que suelte nada (ver NoteCompileMemory), y con los shaders de piel
     * de Zafiro Alfa retenia 8 MB mas por compilacion: 8, 16, 24... y bad_alloc.
     * Pasado el tope no se traducen mas shaders de vertices (son los grandes);
     * los de fragmentos, que son los que hacen falta para dibujar, si.
     */
    constexpr std::size_t kMaxRetained = 12u * 1024u * 1024u;
    if (g_live_bytes > kMaxRetained && !g_poisoned) {
        g_poisoned = true;
        Common::VitaNote("gxm mem", fmt::format("el compilador retiene {} KB: no se traducen mas "
                                                "shaders de vertices en esta sesion",
                                                g_live_bytes / 1024)
                                        .c_str());
    }
    return copy;
}
} // Anonymous namespace

void CgForgetInterruptedCompile() {
    ClearCompilingMarker();
}

const SceShaccCgCompileOutput* LoadProgramFile(const char* path) {
    const SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        return nullptr;
    }
    const SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    if (size < 16 || size > 1024 * 1024) {
        sceIoClose(fd);
        return nullptr;
    }
    void* memory = std::malloc(sizeof(SceShaccCgCompileOutput) + static_cast<std::size_t>(size));
    if (memory == nullptr) {
        sceIoClose(fd);
        return nullptr;
    }
    u8* program = static_cast<u8*>(memory) + sizeof(SceShaccCgCompileOutput);
    const int got = sceIoRead(fd, program, static_cast<SceSize>(size));
    sceIoClose(fd);
    if (got != static_cast<int>(size) || std::memcmp(program, "GXP", 4) != 0) {
        std::free(memory);
        return nullptr;
    }
    auto* output = static_cast<SceShaccCgCompileOutput*>(memory);
    output->programData = program;
    output->programSize = static_cast<SceInt32>(size);
    output->diagnosticCount = 0;
    output->diagnostics = nullptr;
    {
        // Como los de la cache: ReleaseCgOutput lo libera con free.
        const std::lock_guard lock{g_outputs_mutex};
        g_cached_outputs.emplace(output, 0);
    }
    return output;
}

void CgNoteGpuHang() {
    g_gpu_hung.store(true, std::memory_order_relaxed);
    if (g_marker_live.load(std::memory_order_acquire) && g_marker_fd >= 0) {
        const u32 flags = kMarkerGpuHung;
        sceIoPwrite(g_marker_fd, &flags, sizeof(flags), offsetof(CompilingMarker, flags));
    }
}

u64 CgOutputHash(const SceShaccCgCompileOutput* output) {
    const std::lock_guard lock{g_outputs_mutex};
    const auto it = g_cached_outputs.find(output);
    return it != g_cached_outputs.end() ? it->second : 0;
}

void ReleaseCgOutput(const SceShaccCgCompileOutput* output) {
    if (output == nullptr) {
        return;
    }
    // Las que vinieron de la cache son un malloc nuestro (0.1.5.8).
    {
        const std::lock_guard lock{g_outputs_mutex};
        const auto it = g_cached_outputs.find(output);
        if (it != g_cached_outputs.end()) {
            g_cached_outputs.erase(it);
            std::free(const_cast<SceShaccCgCompileOutput*>(output));
            return;
        }
    }
    const std::lock_guard lock{g_cg_mutex};
    sceShaccCgDestroyCompileOutput(output);
}

namespace {

std::mutex g_jobs_mutex;
std::condition_variable g_jobs_ready;
std::deque<std::shared_ptr<CgJob>> g_jobs;
bool g_worker_started = false;
std::atomic<SceUID> g_worker_uid{-1};
std::atomic<unsigned long long> g_worker_busy_since{0};
/// Cuando termino el ultimo trabajo el hilo de compilacion (0.4.0.3).
std::atomic<unsigned long long> g_worker_last_done_us{0};
std::atomic<u32> g_jobs_queued{0};
std::atomic<bool> g_worker_boosted{false};
/// El trabajo en marcha es grande (CgJob::heavy): ese no se sube (0.2.2.7).
std::atomic<bool> g_worker_heavy{false};
/// Atado al nucleo 2 (0.2.3.8): no se sube nunca.
bool g_worker_pinned = false;
/// Cuando se encolo el ultimo trabajo que no es grande (0.3.4.3, ver CgWorkerMain).
unsigned long long g_last_light_submit_us = 0;
/**
 * Calma antes de empezar uno grande (0.3.4.3). Un grande que ya ha empezado no
 * se puede parar: en Kirby's Extra Epic Yarn uno de 21 KB tardo 104 s, y los de
 * fragmentos que llegaron justo despues (1-2 s cada uno) esperaron esos 104 s
 * con sus lotes por software a 0,5 FPS. Al entrar en una zona nueva los
 * pequenos llegan a rafagas: se deja pasar la rafaga entera antes.
 */
constexpr unsigned long long kHeavyCalmUs = 10000000;
/**
 * Y NINGUNO GRANDE AL EMPEZAR LA PARTIDA (0.3.9.7). En el titulo de Zelda Ocarina of
 * Time 3D uno de vertices de 24 KB entro en el primer hueco de calma y tuvo el
 * compilador mas de dos minutos; los de fragmentos que llegaron detras esperaron
 * y la pantalla de arriba salio en negro todo ese rato. En los primeros
 * kHeavyGraceUs de cada partida solo se compilan los pequenos, salvo que el
 * grande lleve kHeavyMaxWaitUs en la cola (que no se quede sin compilar nunca).
 * La calma de antes de uno grande pasa de 3 a 10 s por lo mismo.
 */
constexpr unsigned long long kHeavyGraceUs = 60000000;
constexpr unsigned long long kHeavyMaxWaitUs = 90000000;
/// Justo por encima de los ayudantes y del hilo de la GPU (159).
constexpr int kCgBoostedPriority = Common::kVitaPriorityHelper - 1;

/**
 * EL COMPILADOR, AL NUCLEO QUE ESTE LIBRE (0.3.9.4). Atado al 2 desde 0.2.3.8
 * porque suelto la consola no lo movia al que quedaba libre; pero el 2 es
 * tambien el del sonido, el DSP y la lectura de ficheros, y con la prioridad
 * mas baja le quedaba poco: en New Super Mario Bros. 2 un shader de fragmentos
 * de 2,3 s de reloj tuvo 0,37 s de CPU, con los nucleos 0 y 1 al 50-60 %. Este
 * hilo mira cada 250 ms cuanto ha estado libre cada nucleo y, mientras haya
 * algo compilandose, lleva el compilador al que mas tiempo libre ha tenido si
 * le saca al actual un 15 % o mas. La prioridad no se toca: en cualquier
 * nucleo sigue usando solo lo que le dejan los demas.
 */
std::atomic<int> g_worker_core{2};

void* CgBalancerMain(void*) {
    sceKernelChangeThreadPriority(sceKernelGetThreadId(), 64);
    unsigned long long last_idle[3] = {};
    unsigned long long last_wall = 0;
    u32 moves = 0;
    u32 ticks = 0;
    while (true) {
        sceKernelDelayThread(250 * 1000);
        // Ver CompilingMarker::alive_s: cada 4 s, cuanto lleva y cuanta CPU.
        if ((++ticks & 15u) == 0) {
            KeepMarkerAlive(g_worker_uid.load(std::memory_order_acquire));
        }
        SceKernelSystemInfo system{};
        system.size = sizeof(system);
        if (sceKernelGetSystemInfo(&system) < 0) {
            continue;
        }
        const unsigned long long now = Common::VitaMicros();
        double free_share[3] = {};
        if (last_wall != 0) {
            const double wall = static_cast<double>(now - last_wall);
            for (int core = 0; core < 3; core++) {
                free_share[core] =
                    static_cast<double>(system.cpuInfo[core].idleClock - last_idle[core]) / wall;
            }
        }
        for (int core = 0; core < 3; core++) {
            last_idle[core] = system.cpuInfo[core].idleClock;
        }
        const bool first = last_wall == 0;
        last_wall = now;
        const SceUID worker = g_worker_uid.load(std::memory_order_acquire);
        if (first || worker < 0 || g_worker_busy_since.load(std::memory_order_relaxed) == 0) {
            continue;
        }
        const int current = g_worker_core.load(std::memory_order_relaxed);
        int best = current;
        for (int core = 0; core < 3; core++) {
            if (free_share[core] > free_share[best]) {
                best = core;
            }
        }
        // El tiempo libre del nucleo actual cuenta tambien el que ya usa el
        // compilador como ocupado: un 15 % de diferencia evita ir y venir.
        if (best == current || free_share[best] < free_share[current] + 0.15) {
            continue;
        }
        static const int kMasks[3] = {SCE_KERNEL_CPU_MASK_USER_0, SCE_KERNEL_CPU_MASK_USER_1,
                                      SCE_KERNEL_CPU_MASK_USER_2};
        if (sceKernelChangeThreadCpuAffinityMask(worker, kMasks[best]) >= 0) {
            g_worker_core.store(best, std::memory_order_relaxed);
            if (++moves <= 6) {
                Common::VitaNote("gxm compila",
                                 fmt::format("compilador: nucleo {} -> {} (libres {:.0f}% / "
                                             "{:.0f}% / {:.0f}%)",
                                             current, best, free_share[0] * 100,
                                             free_share[1] * 100, free_share[2] * 100)
                                     .c_str());
            }
        }
    }
    return nullptr;
}

void* CgWorkerMain(void*) {
    /**
     * EL NUCLEO 2 PARA EL (0.2.3.8). Suelto en cualquier nucleo (0.2.0.5) la
     * consola no lo movia al que quedaba libre: en un combate de Pokemon Sol
     * (crash.txt de 0.2.3.7) un shader de vertices de 18 KB tardo 45 s y otros
     * hasta 2 minutos, con el nucleo 0 parado el 77 % del tiempo, y mientras
     * tanto sus lotes por la CPU a 1-3 FPS. Los ayudantes del sombreado de
     * vertices ya no usan el 2 (ver PicaCore::ParallelShading); el hilo de
     * emulacion esta en el 0 y el de la GPU en el 1.
     */
    {
        const int rc = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(),
                                                            SCE_KERNEL_CPU_MASK_USER_2);
        Common::VitaNote("afinidad", rc >= 0 ? "compilador de shaders: nucleo 2"
                                             : "compilador de shaders: no se pudo atar");
        g_worker_pinned = rc >= 0;
    }
    // La mas baja (0.2.0.2): compilar un shader son segundos de CPU sin soltar
    // el nucleo, y comparte nucleo con ayudantes a los que espera el hilo de
    // la GPU en cada lote. Ver kVitaPriorityBackground.
    Common::VitaSetThreadPriority(Common::kVitaPriorityBackground, "compilador de shaders");
    g_worker_uid.store(sceKernelGetThreadId(), std::memory_order_release);
    {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 16 * 1024);
        pthread_t balancer;
        if (pthread_create(&balancer, &attr, &CgBalancerMain, nullptr) == 0) {
            pthread_detach(balancer);
        }
        pthread_attr_destroy(&attr);
    }
    while (true) {
        std::shared_ptr<CgJob> job;
        {
            std::unique_lock lock{g_jobs_mutex};
            /**
             * LOS GRANDES, LOS ULTIMOS (0.2.2.3). Uno de 16-23 KB pasa minutos
             * aqui y el hilo es uno solo: por orden de llegada, todo lo que
             * viniera detras esperaba esos minutos, los de fragmentos tambien
             * (sin ellos el lote no se dibuja). Primero cualquiera que no sea
             * grande; los grandes, cuando no queda otra cosa Y lleva
             * kHeavyCalmUs sin llegar ninguno pequeno (0.3.4.3).
             */
            std::deque<std::shared_ptr<CgJob>>::iterator pick;
            u32 picked_demand = 0;
            while (true) {
                g_jobs_ready.wait(lock, [] { return !g_jobs.empty(); });
                /**
                 * LO QUE MAS DIBUJOS TAPA, PRIMERO (0.4.2.2). En Luigi's Mansion con
                 * la cache vacia (28-10) el shader de vertices de 19 KB que usaba
                 * casi toda la escena espero minutos en la cola: era "grande" y los
                 * de fragmentos (uno por objeto, urgentes) no dejaban de llegar, asi
                 * que no habia la calma que pide un grande. Mientras, sus lotes se
                 * saltaban: 284.365 en la partida. Cada lote que espera a un trabajo
                 * le suma a CgJob::demand; se coge el que mas lleva, sea grande o
                 * no. Sin demanda (los de calentar, o nadie los pide ya), el orden
                 * de siempre.
                 */
                pick = g_jobs.end();
                for (auto it = g_jobs.begin(); it != g_jobs.end(); ++it) {
                    const u32 wanted = (*it)->demand.load(std::memory_order_relaxed);
                    if (wanted > picked_demand) {
                        picked_demand = wanted;
                        pick = it;
                    }
                }
                if (pick != g_jobs.end()) {
                    static u32 demand_notes = 0;
                    if (pick != g_jobs.begin() && demand_notes < 12) {
                        demand_notes++;
                        Common::VitaNote(
                            "gxm compila",
                            fmt::format("por demanda: {} de {} ({}) con {} lotes esperandolo, "
                                        "{} en la cola",
                                        (*pick)->profile == SCE_SHACCCG_PROFILE_VP ? "vertices"
                                                                                   : "fragmentos",
                                        (*pick)->sources.empty() ? 0 : (*pick)->sources.front().size(),
                                        (*pick)->heavy ? "grande" : "normal", picked_demand,
                                        g_jobs.size())
                                .c_str());
                    }
                    break;
                }
                pick = std::find_if(g_jobs.begin(), g_jobs.end(),
                                    [](const std::shared_ptr<CgJob>& queued) {
                                        return !queued->heavy;
                                    });
                if (pick != g_jobs.end()) {
                    break;
                }
                const unsigned long long now = Common::VitaMicros();
                const unsigned long long since = now - g_last_light_submit_us;
                const unsigned long long session =
                    now - g_session_start_us.load(std::memory_order_relaxed);
                const unsigned long long oldest = now - g_jobs.front()->submitted_us;
                if ((since >= kHeavyCalmUs && session >= kHeavyGraceUs) ||
                    oldest >= kHeavyMaxWaitUs) {
                    pick = g_jobs.begin();
                    break;
                }
                g_jobs_ready.wait_for(lock, std::chrono::microseconds(1000000));
            }
            job = std::move(*pick);
            g_jobs.erase(pick);
            g_jobs_queued.store(static_cast<u32>(g_jobs.size()), std::memory_order_relaxed);
        }
        g_worker_heavy.store(job->heavy, std::memory_order_relaxed);
        g_worker_busy_since.store(Common::VitaMicros(), std::memory_order_relaxed);
        job->started_us.store(Common::VitaMicros(), std::memory_order_relaxed);
        for (std::size_t i = 0; i < job->sources.size(); i++) {
            // Los de vertices dejan de pedirse con el compilador roto; los de
            // fragmentos no (ver RecoverFromInternalError).
            if (job->profile == SCE_SHACCCG_PROFILE_VP && CgPoisoned()) {
                break;
            }
            const char* name = i < job->names.size() ? job->names[i] : job->name;
            const SceShaccCgCompileOutput* output =
                CompileCg(job->profile, name, job->sources[i].c_str());
            if (output != nullptr) {
                job->output = output;
                job->used_variant = job->variants[i];
                break;
            }
        }
        g_worker_busy_since.store(0, std::memory_order_relaxed);
        g_worker_last_done_us.store(Common::VitaMicros(), std::memory_order_relaxed);
        job->done.store(true, std::memory_order_release);
        // Sin mas trabajo: los trozos vacios del reservador propio, a newlib.
        bool idle = false;
        {
            const std::lock_guard jobs_lock{g_jobs_mutex};
            idle = g_jobs.empty();
        }
        if (idle) {
            const std::lock_guard cg_lock{g_cg_mutex};
            ArenaTrimLocked();
        }
        if (g_worker_boosted.exchange(false, std::memory_order_acq_rel)) {
            sceKernelChangeThreadPriority(sceKernelGetThreadId(),
                                          Common::kVitaPriorityBackground);
        }
    }
    return nullptr;
}

} // Anonymous namespace

/**
 * EL COMPILADOR SE QUEDABA SIN CPU (0.2.1.3). crash.txt de Pokemon Sol en
 * 0.2.1.2: los shaders de vertices del 3D tardaban 30-60 s cada uno (uno que
 * pillo la CPU libre, 2,6 s), porque mientras no estan, sus lotes van por la
 * CPU y los ayudantes que los sombrean (159) ocupan los nucleos al 97 %, por
 * encima del compilador (191). Cuanto mas tardaba, mas tiempo a 1 FPS. Solo
 * cuando un lote lleva esperando de mas se le sube: la consola se para un
 * momento en vez de ir a 1 FPS minutos, y el resultado queda en la cache.
 */
void CgMarkStarved() {
    /**
     * Uno grande no se sube (0.2.2.7): son minutos, y subido se llevaba un
     * nucleo entero por delante de la GPU y de los ayudantes (New Super Mario
     * Bros. 2 en 0.2.2.6: 600 ms por fotograma mientras compilaba). Sus lotes
     * van por la CPU como antes de 0.2.2.3; compila con lo que sobre.
     *
     * Y desde 0.2.3.8 no se sube ninguno: subido por encima del hilo de
     * emulacion (160) lo dejaba parado mientras compilaba ("congelado: 6 s sin
     * avanzar, en 'esperando a la gpu'" en un combate de Pokemon Sol). Con el
     * nucleo 2 para el solo (ver CgWorkerMain) no le hace falta.
     */
    if (g_worker_boosted.load(std::memory_order_relaxed) ||
        g_worker_heavy.load(std::memory_order_relaxed) || g_worker_pinned) {
        return;
    }
    const SceUID thread = g_worker_uid.load(std::memory_order_acquire);
    if (thread < 0 || g_worker_boosted.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    sceKernelChangeThreadPriority(thread, kCgBoostedPriority);
    static bool noted = false;
    if (!noted) {
        noted = true;
        Common::VitaNote("gxm compila", "lotes esperando: compilador por encima de los ayudantes");
    }
}

void CgSubmit(std::shared_ptr<CgJob> job, bool urgent) {
    std::lock_guard lock{g_jobs_mutex};
    if (!g_worker_started) {
        g_worker_started = true;
        /**
         * Pila de 1 MB: el compilador corria hasta ahora en el hilo principal
         * y no se sabe cuanta pila necesita con un shader grande. Mejor que
         * sobre (sale del presupuesto de la aplicacion, que tiene ~80 MB libres
         * fuera del heap).
         */
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 1024 * 1024);
        if (pthread_create(&g_worker_thread, &attr, &CgWorkerMain, nullptr) == 0) {
            g_worker_running = true;
            pthread_detach(g_worker_thread);
        } else {
            g_worker_started = false;
        }
        pthread_attr_destroy(&attr);
        if (!g_worker_started) {
            // Sin hilo: se compila aqui mismo, como antes.
            job->done.store(false, std::memory_order_relaxed);
            for (std::size_t i = 0; i < job->sources.size(); i++) {
                const SceShaccCgCompileOutput* output =
                    CompileCg(job->profile, job->name, job->sources[i].c_str());
                if (output != nullptr) {
                    job->output = output;
                    job->used_variant = job->variants[i];
                    break;
                }
            }
            job->done.store(true, std::memory_order_release);
            return;
        }
        Common::VitaNote("gxm compila", "hilo de compilacion en segundo plano listo");
    }
    job->submitted_us = Common::VitaMicros();
    if (!job->heavy) {
        g_last_light_submit_us = job->submitted_us;
    }
    if (urgent) {
        g_jobs.push_front(std::move(job));
    } else {
        g_jobs.push_back(std::move(job));
    }
    g_jobs_queued.store(static_cast<u32>(g_jobs.size()), std::memory_order_relaxed);
    g_jobs_ready.notify_one();
}

bool CgWorkerProgressing() {
    const unsigned long long now = Common::VitaMicros();
    const unsigned long long last_done = g_worker_last_done_us.load(std::memory_order_relaxed);
    const unsigned long long busy_since = g_worker_busy_since.load(std::memory_order_relaxed);
    return (last_done != 0 && now - last_done < 15000000ull) ||
           (busy_since != 0 && now - busy_since < 60000000ull);
}

bool CgWorkerOnHeavyJob() {
    return g_worker_heavy.load(std::memory_order_relaxed) &&
           g_worker_busy_since.load(std::memory_order_relaxed) != 0;
}

CgActivity GetCgActivity() {
    return CgActivity{g_worker_busy_since.load(std::memory_order_relaxed),
                      g_jobs_queued.load(std::memory_order_relaxed)};
}

namespace {
/// Una compilacion del banco: sin cache, sin marcas, sin lista negra.
std::string BenchCompile(bool vertex, const std::string& source, int opt, int fastmath,
                         int fastprecision, int fastint, int nostdlib, int slot = -1) {
    if (!EnsureCgReady()) {
        return "sin compilador";
    }
    // En paralelo (slot 0 o 1) sin el cerrojo: es justo lo que se quiere medir.
    std::unique_lock<std::recursive_mutex> lock{g_cg_mutex, std::defer_lock};
    if (slot < 0) {
        lock.lock();
    }
    g_alloc_calls = 0;
    g_alloc_total = 0;
    g_alloc_reused = 0;
    static constexpr const char* kSlotNames[2] = {"banco_a.cg", "banco_b.cg"};
    SceShaccCgSourceFile& file = slot >= 0 ? g_bench_files[slot] : g_source;
    const char* const file_name = slot >= 0 ? kSlotNames[slot] : "banco.cg";
    file.fileName = file_name;
    file.text = source.c_str();
    file.size = static_cast<SceUInt32>(source.size());
    SceShaccCgCompileOptions options{};
    sceShaccCgInitializeCompileOptions(&options);
    options.mainSourceFile = file_name;
    options.targetProfile = vertex ? SCE_SHACCCG_PROFILE_VP : SCE_SHACCCG_PROFILE_FP;
    options.entryFunctionName = "main";
    if (opt >= 0) {
        options.optimizationLevel = opt;
    }
    options.useFastmath = fastmath;
    options.useFastprecision = fastprecision;
    options.useFastint = fastint;
    options.noStdlib = nostdlib;
    SceKernelThreadInfo before{};
    before.size = sizeof(before);
    sceKernelGetThreadInfo(sceKernelGetThreadId(), &before);
    g_compile_budget = FreeHeap() - kEmulatorReserve;
    g_compile_bytes = 0;
    g_compile_over_budget = false;
    const unsigned long long begin = Common::VitaMicros();
    g_tracking = true;
    const SceShaccCgCompileOutput* output = sceShaccCgCompileProgram(&options, &g_callbacks, 0);
    g_tracking = false;
    const unsigned long long wall_ms = (Common::VitaMicros() - begin) / 1000;
    FlushSmallFree();
    g_compile_budget = static_cast<std::size_t>(-1);
    SceKernelThreadInfo after{};
    after.size = sizeof(after);
    sceKernelGetThreadInfo(sceKernelGetThreadId(), &after);
    std::string diagnostic = "-";
    bool internal = false;
    if (output != nullptr) {
        for (int i = 0; i < output->diagnosticCount; i++) {
            const char* message = output->diagnostics[i].message;
            if (message == nullptr) {
                continue;
            }
            internal = internal || std::strstr(message, "internal error") != nullptr;
            if (diagnostic == "-") {
                diagnostic = message;
            }
        }
    }
    const bool ok = output != nullptr && output->programData != nullptr;
    /**
     * LO QUE IMPORTA DEL BINARIO (0.4.2.0). Los bytes del programa no son lo
     * que cuesta: el banco del 27-10 vio fastprecision y half dar binarios
     * mas GRANDES. Lo que mide el chip es cuantas instrucciones tiene el
     * programa principal y cuantos registros temporales usa (con mas, caben
     * menos hilos a la vez y se tapan peor las lecturas de textura). Los
     * campos de la cabecera SceGxmProgram, en sus desplazamientos (los de
     * Vita3K, gxm/types.h).
     */
    std::string header = "-";
    if (ok && output->programSize >= 72) {
        const u8* const gxp = reinterpret_cast<const u8*>(output->programData);
        const auto u16_at = [gxp](u32 at) {
            u16 v;
            std::memcpy(&v, gxp + at, sizeof(v));
            return v;
        };
        const auto u32_at = [gxp](u32 at) {
            u32 v;
            std::memcpy(&v, gxp + at, sizeof(v));
            return v;
        };
        // Y las banderas del programa (+20, 0.4.2.4): ver EL REGISTRO PUEDE FALLAR
        // en rasterizer_gxm.cpp.
        header = fmt::format("instrucciones {} (secundario {}), temporales {}, PA {}, SA {}, "
                             "banderas {:08x}",
                             u32_at(60), u32_at(68), u32_at(52), u16_at(48), u16_at(50),
                             u32_at(20));
    }
    const std::string text = fmt::format(
        "{} | reloj {} ms, CPU {} ms | {} bytes de binario | {} | {} reservas, {} MB pedidos | {}",
        ok ? "OK" : "FALLO", wall_ms, (after.runClocks - before.runClocks) / 1000,
        ok ? output->programSize : 0, header, g_alloc_calls, g_alloc_total >> 20, diagnostic);
    if (output != nullptr) {
        sceShaccCgDestroyCompileOutput(output);
    }
    if (internal) {
        RecoverFromInternalError();
    }
    return text;
}
} // Anonymous namespace

void RunCgBenchIfAny() {
    static constexpr char kDir[] = "ux0:/data/azahar/banco_cg/";
    const std::string order_path = std::string(kDir) + "hacer.txt";
    std::FILE* order = std::fopen(order_path.c_str(), "r");
    if (order == nullptr) {
        return;
    }
    std::vector<std::string> lines;
    char line[512];
    while (std::fgets(line, sizeof(line), order) != nullptr) {
        std::string text = line;
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
        if (!text.empty() && text[0] != '#') {
            lines.push_back(text);
        }
    }
    std::fclose(order);
    std::remove(order_path.c_str());
    Common::VitaNote("banco cg", fmt::format("{} compilaciones", lines.size()).c_str());
    std::FILE* out = std::fopen((std::string(kDir) + "resultados.txt").c_str(), "a");
    // Que no la pare nadie: el banco es lo unico que corre.
    sceKernelChangeThreadPriority(sceKernelGetThreadId(), 64);
    for (const std::string& spec : lines) {
        char file[256] = {};
        char profile[8] = {};
        int opt = -1, fastmath = 0, fastprecision = 0, fastint = 0, nostdlib = 0;
        std::sscanf(spec.c_str(), "%255s %7s O=%d fastmath=%d fastprecision=%d fastint=%d "
                                  "nostdlib=%d",
                    file, profile, &opt, &fastmath, &fastprecision, &fastint, &nostdlib);
        const auto read_source = [&](const char* name) {
            std::string text;
            if (std::FILE* in = std::fopen((std::string(kDir) + name).c_str(), "rb")) {
                char chunk[4096];
                std::size_t got = 0;
                while ((got = std::fread(chunk, 1, sizeof(chunk), in)) > 0) {
                    text.append(chunk, got);
                }
                std::fclose(in);
            }
            return text;
        };
        /**
         * DOS A LA VEZ (0.4.3.1): "par a.cg b.cg vp|fp O=n". Cada una en su hilo
         * (1 MB de pila, como el de compilacion), sin el cerrojo del compilador:
         * para saber si libshacccg aguanta dos compilaciones en paralelo y si
         * de verdad van a la vez (reloj total frente a la suma).
         */
        if (std::strcmp(file, "par") == 0) {
            char first[128] = {};
            char second[128] = {};
            char par_profile[8] = {};
            int par_opt = -1;
            std::sscanf(spec.c_str(), "par %127s %127s %7s O=%d", first, second, par_profile,
                        &par_opt);
            struct Work {
                std::string source;
                bool vertex;
                int opt;
                int slot;
                std::string result;
            };
            Work work[2] = {{read_source(first), std::strcmp(par_profile, "fp") != 0, par_opt, 0, {}},
                            {read_source(second), std::strcmp(par_profile, "fp") != 0, par_opt, 1, {}}};
            EnsureCgReady();
            g_bench_parallel.store(true, std::memory_order_relaxed);
            const unsigned long long par_begin = Common::VitaMicros();
            pthread_t threads[2]{};
            bool started[2] = {false, false};
            for (int i = 0; i < 2; i++) {
                pthread_attr_t attr;
                pthread_attr_init(&attr);
                pthread_attr_setstacksize(&attr, 1024 * 1024);
                started[i] = pthread_create(&threads[i], &attr, [](void* arg) -> void* {
                    auto* w = static_cast<Work*>(arg);
                    // Cada uno en un nucleo distinto (1 y 2).
                    sceKernelChangeThreadCpuAffinityMask(
                        sceKernelGetThreadId(),
                        w->slot == 0 ? SCE_KERNEL_CPU_MASK_USER_1 : SCE_KERNEL_CPU_MASK_USER_2);
                    w->result = w->source.empty()
                                    ? std::string("no se pudo leer")
                                    : BenchCompile(w->vertex, w->source, w->opt, 0, 0, 0, 0, w->slot);
                    return nullptr;
                }, &work[i]) == 0;
                pthread_attr_destroy(&attr);
            }
            for (int i = 0; i < 2; i++) {
                if (started[i]) {
                    pthread_join(threads[i], nullptr);
                }
            }
            g_bench_parallel.store(false, std::memory_order_relaxed);
            const std::string result =
                fmt::format("los dos en {} ms | A: {} | B: {}",
                            (Common::VitaMicros() - par_begin) / 1000, work[0].result,
                            work[1].result);
            Common::VitaNote("banco cg", fmt::format("{} -> {}", spec, result).c_str());
            if (out != nullptr) {
                std::fprintf(out, "%s -> %s\n", spec.c_str(), result.c_str());
                std::fflush(out);
            }
            continue;
        }
        const std::string source = read_source(file);
        const std::string result =
            source.empty() ? std::string("no se pudo leer")
                           : BenchCompile(std::strcmp(profile, "fp") != 0, source, opt,
                                          fastmath, fastprecision, fastint, nostdlib);
        Common::VitaNote("banco cg", fmt::format("{} -> {}", spec, result).c_str());
        if (out != nullptr) {
            std::fprintf(out, "%s -> %s\n", spec.c_str(), result.c_str());
            std::fflush(out);
        }
    }
    if (out != nullptr) {
        std::fprintf(out, "== fin\n");
        std::fclose(out);
    }
    sceKernelDelayThread(1000 * 1000);
    sceKernelExitProcess(0);
}

} // namespace Gxm
