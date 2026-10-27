// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <psp2/shacccg.h>
#include "common/common_types.h"

namespace Gxm {

/**
 * El compilador de shaders Cg (libshacccg.suprx), COMPARTIDO por todo el
 * backend.
 *
 * El modulo es estado global del proceso: el presentador y el rasterizador lo
 * necesitan a la vez, y cargarlo por separado hacia que el segundo intento
 * fallara (y que reintentarlo en cada lote convirtiera la caida de rendimiento
 * en una tormenta de llamadas al sistema: 13,7 s de fotograma medidos en
 * consola). Aqui se carga una vez, se reutiliza el handle y, si el primer
 * intento falla, se reintenta como mucho una vez por segundo.
 */
bool EnsureCgReady();

/**
 * BANCO DEL COMPILADOR (0.4.0.0). Si existe ux0:/data/azahar/banco_cg/hacer.txt,
 * compila lo que pide cada linea ("fichero.cg vp|fp O=n fastmath=n
 * fastprecision=n fastint=n nostdlib=n"), apunta tiempo de reloj y de CPU en
 * banco_cg/resultados.txt, borra hacer.txt y cierra Azahar. Para probar desde
 * el PC que forma del Cg compila mas deprisa sin tener que jugar.
 */
void RunCgBenchIfAny();

/// Los shaders de vertices a O2 (0.4.1.1, ver gxm_cg.cpp). Ajuste "vs_o2=".
extern std::atomic<bool> g_vs_o2;
/// Fragmentos compilados con useFastprecision (0.4.1.9, ajuste fs_rapidos=).
extern std::atomic<bool> g_fs_fast;

/// El banco de memoria (0.4.0.9, gxm_banco_memoria.cpp): solo con banco_mem/hacer.txt.
void RunMemBenchIfAny();

/// Estado corto para el overlay y crash.txt.
const char* CgStatus();

/**
 * True si el compilador de Cg ha devuelto un "internal error" en esta sesion.
 *
 * En 0.1.4.7 un shader de vertices traducido hizo que sceShaccCg respondiera
 * "fatal internal error", y a partir de ahi fallaron TODAS las compilaciones,
 * tambien las de fragmentos: la partida entera se fue a software. Si vuelve a
 * pasar, la ruta del shader de vertices en la GPU deja de pedirle shaders al
 * compilador (HwShaderCache), para no gastar lo que le quede en algo que ya
 * se sabe que lo rompe.
 */
bool CgPoisoned();

/**
 * Queda poco heap (0.1.7.9): menos de kCgHeapReserveMB libres. Lo opcional
 * que cuesta compilar -- los programas especializados por booleanos -- se deja
 * de pedir, para que una escena con muchos shaders nuevos no acabe la partida
 * con bad_alloc. El lote se queda en la CPU, que es correcto.
 */
constexpr unsigned int kCgHeapReserveMB = 32;
bool CgHeapLow();

/**
 * Cuantas veces se ha descargado y recargado el compilador tras un error
 * interno (0.1.5.9). Un shader que fallo por el compilador roto se puede
 * reintentar cuando esto cambia: el fallo no era del shader.
 */
u32 CgGeneration();

/**
 * Solo la cache de la tarjeta, sin el compilador (0.1.9.9): nullptr si no esta
 * o no vale. Es lo unico que pide el hilo de emulacion en partida: CompileCg,
 * si el fichero de la cache no sirve, va al compilador, y con el hilo de
 * compilacion ocupado eso era esperarle (Yo-kai Watch, 0.1.9.8).
 */
const SceShaccCgCompileOutput* LoadCgCache(SceShaccCgTargetProfile profile, const char* name,
                                           const char* source, bool from_card = true);

/**
 * EL INDICE DE PROGRAMAS DE VERTICES (0.3.2.8). Por juego
 * (shadercache/vs_<id>.idx), la clave de cada programa de vertices de
 * HwShaderCache y la fuente de Cg con la que esta en la cache de la tarjeta.
 * Con eso se lee de la cache sin traducirlo otra vez a Cg, que son 4-36 ms por
 * programa en el hilo de la GPU ("traducir" en crash.txt: ~600 ms en cada
 * cambio de zona de Pokemon Sol). Solo vale para la misma compilacion del
 * traductor (VsGeneratorTag); con otra se empieza de cero. 'variant' dice que
 * variante de traduccion era.
 */
const SceShaccCgCompileOutput* LoadCgCacheIndexed(u64 key, u32* variant);
void AddVsIndex(u64 key, const std::string& source, u32 variant);

/// Compila un shader. El resultado vive hasta ReleaseCgOutput.
const SceShaccCgCompileOutput* CompileCg(SceShaccCgTargetProfile profile, const char* name,
                                         const char* source);

void ReleaseCgOutput(const SceShaccCgCompileOutput* output);

/// El hash del codigo fuente de una salida (el nombre de su .gxp en la cache),
/// o 0 si no es de las que se guardan (0.3.8.1, para el ranking de la caja negra).
u64 CgOutputHash(const SceShaccCgCompileOutput* output);

/// Olvida la compilacion en curso para el siguiente arranque: el proceso va a
/// salir a proposito, no se muere compilando (0.3.8.3, piloto automatico).
void CgForgetInterruptedCompile();

/**
 * PRECARGA DE LA CACHE DE SHADERS (0.2.0.3). Cada shader que se lee o se
 * guarda en la cache de la tarjeta queda apuntado en una lista por juego
 * (shadercache/juego_<id>.lst). Al arrancar ese juego otra vez, esto lee a
 * memoria todos los de su lista, y durante la partida se sirven de ahi en vez
 * de abrir un fichero en la tarjeta por shader: en una transicion al 3D eran
 * decenas de lecturas de varios ms cada una en mitad del juego. 'progress'
 * recibe (leidos, total). Devuelve cuantos se precargaron.
 */
u32 PreloadCgCache(u64 program_id, const std::function<void(u32, u32)>& progress);

/// El prefijo de los ficheros de la cache del juego que corre
/// (".../shadercache/juego_<id>"), vacio si no se ha precargado ninguno (0.3.9.4).
std::string CurrentGameCachePrefix();

/// El programa de esa fuente ya esta en la cache (precargado o en la carpeta),
/// sin leerlo (0.3.9.4).
bool CgCacheContains(SceShaccCgTargetProfile profile, const std::string& source);

/**
 * COMPILACION EN SEGUNDO PLANO (0.1.9.6).
 *
 * Un shader de vertices de 11-16 KB de Cg tarda de varios segundos a mas de
 * medio minuto en el compilador de la consola, y hasta ahora eso era el juego
 * congelado (Zafiro Alfa al entrar al 3D, Pokemon Sol al presentarse Kukui).
 * Ahora un hilo aparte compila y el lote sigue por la CPU mientras tanto; el
 * resultado se recoge en el hilo de emulacion (registrar el programa en el
 * parcheador no es seguro desde otro hilo). Las fuentes son las variantes a
 * probar, en orden: la primera que compile gana. El compilador sigue siendo
 * uno solo: CompileCg lo protege con un cerrojo, asi que un shader de
 * fragmentos que haga falta en el hilo de emulacion espera, como mucho, a que
 * acabe la compilacion en curso.
 */
struct CgJob {
    /// Vertices o fragmentos (0.1.9.8: los de fragmentos tambien van aqui).
    SceShaccCgTargetProfile profile = SCE_SHACCCG_PROFILE_VP;
    const char* name = "azahar_gxm_vs.cg";
    std::vector<std::string> sources;
    std::vector<u32> variants;
    /// El nombre de cada fuente para crash.txt, si no es 'name' (0.3.1.4).
    std::vector<const char*> names;
    /// Lo escribe el hilo de compilacion ANTES de poner done.
    const SceShaccCgCompileOutput* output = nullptr;
    u32 used_variant = 0;
    std::atomic<bool> done{false};
    /// Cuando se encolo (VitaMicros), para saber cuanto lleva esperando.
    unsigned long long submitted_us = 0;
    /// Cuando lo cogio el hilo de compilacion (0 = aun en la cola), 0.3.1.3.
    std::atomic<unsigned long long> started_us{0};
    /// Minutos de compilador (0.2.2.3): se coge cuando no queda otro en la cola.
    bool heavy = false;
};

/// 'urgent' pone el trabajo delante de la cola (los de fragmentos: sin ellos
/// el lote no se dibuja).
void CgSubmit(std::shared_ptr<CgJob> job, bool urgent = false);

/// Hay lotes por la CPU esperando a un trabajo de la cola: el hilo de
/// compilacion sube por encima de los ayudantes hasta acabar el que tiene entre
/// manos (0.2.1.3). Barato si ya esta subido.
void CgMarkStarved();

/// Lo que esta haciendo el hilo de compilacion, para el aviso en pantalla
/// (0.2.1.6): desde cuando compila el trabajo actual (VitaMicros, 0 si nada) y
/// cuantos esperan detras.
struct CgActivity {
    unsigned long long busy_since_us = 0;
    u32 queued = 0;
};
CgActivity GetCgActivity();

/**
 * El hilo de compilacion esta con un trabajo grande (CgJob::heavy), de los que
 * tardan minutos (0.3.4.3). Mientras tanto, lo que espera en la cola no esta
 * "atascado": espera su turno detras de un compilador que avanza. Ver los
 * motivos "fs atascado" y "vs atascado" en rasterizer_gxm.cpp.
 */
bool CgWorkerOnHeavyJob();

/**
 * EL COMPILADOR AVANZA (0.4.0.3): ha terminado un shader en los ultimos 15 s, o
 * lleva menos de 60 con el que tiene entre manos. Mientras sea asi, un lote
 * cuyo shader espera mucho NO se da por atascado (todos sus lotes a la CPU):
 * sigue el reparto de "compilando largo" (unos pocos por la CPU, el resto se
 * salta un momento).
 */
bool CgWorkerProgressing();

} // namespace Gxm
