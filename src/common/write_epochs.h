// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include <cstddef>
#include "common/common_types.h"

/**
 * LAS EPOCAS DE ESCRITURA DE LA FCRAM (0.4.0.8).
 *
 * POR QUE. La cache de subidas de vertices (0.4.0.4) compara en cada lote los
 * bytes del invitado con su copia para saber si han cambiado: en el 3D de
 * Zafiro Alfa son 12 KB por lote y la comparacion se lleva mas tiempo del hilo
 * de la GPU que ninguna otra cosa. Y casi todos son mallas que no cambian nunca.
 *
 * DE DONDE SE SABE QUE ALGO HA CAMBIADO. En una 3DS la GPU NO ve la cache de
 * la CPU: un juego que escribe vertices con la CPU TIENE que vaciar esa parte
 * de la cache antes de dibujar (GSPGPU::FlushDataCache o StoreDataCache, la
 * orden GX de vaciado, svcFlushProcessDataCache/StoreProcessDataCache), o la
 * GPU leeria lo viejo. Y lo que escribe el propio sistema (lecturas de
 * ficheros, IPC, DMA, rellenos y transferencias de la GPU) pasa por aqui. Cada
 * uno de esos avisos pone una epoca nueva en sus paginas de 4 KB; una pagina
 * cuya epoca no ha subido desde la ultima comparacion tiene los mismos bytes
 * que entonces vio la GPU de la 3DS.
 *
 * Fuera de la FCRAM (VRAM y demas) no se apunta nada y CleanSince dice que no:
 * eso se sigue comparando como siempre. Quien lo use pone ademas un tope de
 * tiempo, por si algun juego se fiara de que la cache se vacie sola.
 */
namespace Common::WriteEpochs {

/// Ajuste "vertices_por_vaciados=": apagado, CleanSince siempre dice que no.
extern std::atomic<bool> enabled;

/// La FCRAM del invitado, en memoria nuestra. Rehace la tabla (todo limpio).
void SetRange(const u8* base, std::size_t size);

/// La VRAM del invitado (0.4.1.1): se sigue igual que la FCRAM.
void SetVramRange(const u8* base, std::size_t size);

/**
 * No fiarse mas de la zona de este puntero en esta partida (0.4.1.1): quien
 * descubra que alli se escribio sin aviso (la CPU puede escribir la VRAM sin
 * pasar por la cache, sin vaciar nada).
 */
void DistrustRegionOf(const void* pointer);

/// El puntero es de la VRAM del invitado.
[[nodiscard]] bool InVram(const void* pointer);

/// De donde viene cada aviso (0.4.0.9, para crash.txt).
enum Source : u32 {
    kSourceWrite,      ///< WriteBlock/ZeroBlock: lo escribe el sistema.
    kSourceGspFlush,   ///< GSPGPU::FlushDataCache.
    kSourceGspStore,   ///< GSPGPU::StoreDataCache.
    kSourceGxFlush,    ///< La orden GX de vaciado de cache.
    kSourceSvc,        ///< svcFlush/StoreProcessDataCache.
    kSourceGpuFill,    ///< Relleno de memoria de la GPU.
    kSourceGpuCopy,    ///< Transferencia o copia de textura de la GPU.
    kSources,
};
/// Lo que dice CleanSince en 'culprit' cuando no hay pagina culpable (0.4.1.4).
constexpr u32 kCulpritOutside = kSources;        ///< Fuera de la FCRAM y la VRAM.
constexpr u32 kCulpritFloor = kSources + 1;      ///< Todo dado por cambiado (MarkAll).
constexpr u32 kCulpritDistrusted = kSources + 2; ///< Zona sin confianza (el vigia).
constexpr u32 kCulprits = kSources + 3;

/// Estos bytes (puntero nuestro) pueden haber cambiado: epoca nueva.
void MarkHost(const void* pointer, std::size_t size, u32 source = kSourceWrite);

/// Avisos y KB por fuente desde la ultima llamada (y los pone a cero).
void TakeSourceStats(u32* calls, u32* kib);

/// Todo puede haber cambiado (un aviso que no se sabe traducir).
void MarkAll();

/// La epoca de ahora: se toma ANTES de leer los bytes que se comparan.
[[nodiscard]] u32 Now();

/// Ninguna pagina del tramo ha recibido un aviso despues de 'epoch'. Si no,
/// y 'culprit' no es nulo, la fuente del aviso de la primera pagina que lo
/// tiene, o uno de los kCulprit* si no hay pagina culpable.
[[nodiscard]] bool CleanSince(const void* pointer, std::size_t size, u32 epoch,
                              u32* culprit = nullptr);

/// Veces que se dio todo por cambiado (MarkAll), para crash.txt (0.4.1.2).
[[nodiscard]] u32 MarkAlls();

/// Avisos recibidos (para crash.txt).
[[nodiscard]] u32 Marks();

} // namespace Common::WriteEpochs
