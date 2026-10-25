// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "common/common_types.h"

struct SceGxmTexture;

/**
 * LA CAJA NEGRA DE LA GPU (0.3.7.5).
 *
 * Zafiro Alfa cuelga la GPU en la cinematica de la bici (GPUCRASH) y nada de
 * lo probado lo quita: ni esperar a la GPU tras cada escena, ni el video_core
 * en el hilo de emulacion, ni dejar los shaders de 26 KB en la CPU, ni el
 * recorte del LOOP. El volcado de la consola solo dice que el hilo de la GPU
 * estaba en sceGxmFinish esperando a un chip que ya no iba a contestar; QUE se
 * le habia mandado no lo dice nadie.
 *
 * Esto lo apunta. Las ultimas escenas que se mandan, cada una con sus ultimos
 * dibujados: programas, indices, flujos de vertices y texturas, con sus
 * direcciones. Vive en la PILA de un hilo propio que nunca vuelve, porque el
 * psp2dmp de un GPUCRASH guarda las pilas de todos los hilos y no el resto de
 * la memoria del proceso: asi esta en el volcado aunque la consola mate el
 * proceso antes de que nadie escriba nada. Empieza con kMagic para encontrarla.
 *
 * Ademas, ese hilo mira la valla cada 100 ms: si hay escenas mandadas y no
 * avanza en 1,5 s, escribe lo que hay en ux0:data/azahar/gpu_colgada.txt, una
 * sola vez, con escrituras directas (por si da tiempo).
 *
 * Solo escribe el hilo que dibuja (el de la GPU, o el de emulacion con
 * gpu_hilo=0: nunca los dos a la vez), y lo que escribe son unos pocos
 * enteros por dibujado.
 */
namespace Gxm::BlackBox {

enum class SceneKind : u8 {
    Unknown = 0, ///< Dibujado sin escena apuntada: algun camino que falta enganchar.
    Draw = 1,    ///< La escena de una superficie (SetupDrawState).
    Blit = 2,    ///< Copia de pantalla (BlitToCopy).
    Clear = 3,   ///< Borrado aparte (FlushClear).
};

enum class DrawKind : u8 {
    VertexGpu = 1, ///< Shader de vertices de la PICA traducido, en la GPU.
    VertexCpu = 2, ///< Vertices ya transformados por la CPU (DrawBatchOnGpu).
    Blit = 3,      ///< El quad de la copia de pantalla.
    Clear = 4,     ///< El quad del borrado.
};

/// Lo que el que dibuja sabe de su dibujado; las texturas se apuntan aparte.
struct DrawInfo {
    DrawKind kind{};
    u8 primitive = 0; ///< SceGxmPrimitiveType.
    u8 streams = 0;   ///< Flujos de vertices puestos.
    u32 count = 0;    ///< Indices que se le dan a sceGxmDraw.
    u32 range = 0;    ///< Vertices copiados por flujo (0 si no aplica).
    const void* indices = nullptr;
    const void* vertex_program = nullptr;
    const void* fragment_program = nullptr;
    const void* stream[3]{};
    u32 stream_bytes[3]{};
    /// Triangulos aplastados por posiciones no finitas (DrawBatchOnGpu).
    u32 bad = 0;
};

/// Arranca el hilo de la caja (una vez). 'fence' es la valla que escribe la GPU
/// al acabar cada escena.
void Start(const volatile unsigned int* fence);

/// El anillo de vertices, para ver si algun flujo o indice se sale de el.
void NoteVertexRing(const void* base, u32 size);

/// Justo despues de un sceGxmBeginScene que ha ido bien. 'color' es la
/// direccion del invitado (la de "gxm dib" en crash.txt).
void OpenScene(SceneKind kind, u32 color);

/// Justo despues de su sceGxmEndScene, con el valor de valla que escribira.
void CloseScene(u32 fence);

/// La textura de un sampler (0..2) del dibujado que viene.
void NoteTexture(u32 slot, const SceGxmTexture* texture);

/// Justo ANTES de sceGxmDraw: si el que cuelga la GPU es ese, tiene que estar.
void Record(const DrawInfo& info);

} // namespace Gxm::BlackBox
