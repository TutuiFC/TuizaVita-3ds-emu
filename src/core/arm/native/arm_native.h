// Copyright 2026 Azahar Vita port
// Licensed under GPLv2 or any later version

#pragma once

#include <memory>
#include "common/common_types.h"

namespace Memory {
struct PageTable;
class MemorySystem;
}
struct ARMul_State;

/**
 * EJECUCION NATIVA DEL CODIGO DEL 3DS (fase 3, lado emulador).
 *
 * El modulo de kernel `aznative.skprx` deja correr el codigo ARM11 del juego
 * directamente en el Cortex-A9 de la Vita, en modo usuario, y lo devuelve por
 * cada excepcion suya (SVC, instruccion indefinida, fallo de memoria) o por
 * cada interrupcion de la consola. Lo que esta medido en hardware a dia de
 * 24-10-2026:
 *   - 883 MIPS nativos contra ~25 M instr/s del JIT a 18 ciclos por
 *     instruccion, o sea unas 35 veces mas en bucles de registros;
 *   - 4,9 us por salida del mundo nativo, y el juego hace 109 SVC por
 *     fotograma: 0,53 ms de los 36, o sea que la puerta no se come la ganancia;
 *   - el invitado puede ejecutar en 0x00100000, donde el 3DS pone el codigo,
 *     aunque la Vita tenga objetos vivos del kernel ahi, porque corre con una
 *     tabla TTBR0 nuestra que solo existe durante esos microsegundos;
 *   - y puede usar la memoria que el emulador YA tiene reservada, mapeada
 *     pagina a pagina en las direcciones del 3DS.
 *
 * DE MOMENTO ESTO NO EJECUTA NADA. Esta primera pieza solo refleja la tabla de
 * paginas del emulador en la MMU y cuenta lo que cuesta, con el JIT corriendo
 * encima como siempre. Es a proposito: de ese numero depende si la tabla se
 * puede rehacer cada vez que el 3DS cambia de proceso o si hay que llevarla
 * incremental, y es mejor saberlo antes de escribir el motor que despues.
 *
 * COMO SE CARGA EL MODULO. No lo carga Azahar: un modulo de kernel se queda
 * cargado hasta que la consola se apaga de verdad, asi que basta con abrir una
 * vez la app de prueba (`AZNT00001`) y Azahar se lo encuentra puesto. Las
 * importaciones son DEBILES: si no esta, los stubs quedan sin resolver y aqui
 * no se llama a nada -- el emulador funciona exactamente como antes. Nunca se
 * llama a un stub sin comprobarlo primero: llamarlo sin resolver salta a la
 * direccion 0 y mata el proceso.
 */
namespace Core::Native {

/// El modulo esta cargado y su version es la que espera este binario. Se
/// comprueba una vez y se recuerda; mirar los stubs es inofensivo, llamarlos
/// sin resolver no.
bool Available();

/**
 * Si el modulo de kernel no esta cargado, lo carga y relanza la app (0.3.7.9).
 * Solo hace algo la primera vez de cada encendido; ver arm_native.cpp. Antes
 * de que nada pregunte por Available(), y solo con jit_nativo=1.
 */
void LoadModuleIfMissing(int argc, char** argv);

/// Lo que dice la version del modulo cargado, 0 si no hay ninguno. Para el log.
u32 ModuleVersion();

/**
 * Crea la tabla TTBR0 del invitado (copia de la de Sony, ver AzNativeWorldCreate).
 * Idempotente: si ya existe no hace nada. Devuelve false si no se pudo.
 */
bool EnsureWorld();

/**
 * Refleja en la MMU las paginas mapeadas de una tabla de paginas del emulador.
 * `raw` es `PageTable::RawPointers()`, el array de 2^20 punteros por pagina de
 * 4 KB que Citra ya mantiene. Solo se recorre el primer gigabyte, que es donde
 * cabe todo el mapa del 3DS (de 0x00100000 a 0x30000000) y lo que cubre TTBR0.
 *
 * Deja una linea en crash.txt con cuantas paginas se mapearon y cuanto costo.
 */
void MirrorPageTable(u8* const* raw);

/**
 * Y SOLO UN TRAMO, que es lo que hace falta de verdad.
 *
 * El reflejo completo solo se dispara cuando el emulador cambia de tabla de
 * paginas, pero durante el arranque va mapeando memoria -- el heap, el heap
 * lineal, el TLS, la pagina compartida -- sobre la MISMA tabla. Sin reflejar
 * esos cambios, el invitado se queda con una vista congelada: unas paginas sin
 * mapear y otras apuntando a fisicas que el emulador ya movio. Zafiro Alfa se
 * colgaba girando para siempre en un bucle de espera sobre un valor obsoleto
 * (log del 24-10: 4.000 rodajas entre 0x0012bb8c y 0x0012bb98 sin una sola SVC).
 *
 * Esto se llama desde MemorySystem::MapPages, que es el unico sitio por donde
 * pasan todos los mapeos y desmapeos. Silencioso: ocurre constantemente.
 */
void MirrorRange(u8* const* raw, u32 first_page, u32 pages);

/**
 * EL EMULADOR HA CAMBIADO CODIGO DEL INVITADO: fuera la cache de instrucciones.
 *
 * Zafiro Alfa carga modulos CRO constantemente, sobre todo al cargar modelos.
 * El emulador escribe esas instrucciones por SU direccion, y la cache de
 * instrucciones del Cortex-A9 puede seguir teniendo las lineas viejas de la
 * direccion del 3DS: el invitado ejecutaria codigo caducado, y eso acaba
 * escribiendo ordenes de GPU invalidas -- que es como se cuelga el driver
 * grafico de la consola.
 *
 * Se llama desde donde el emulador ya avisa de que el codigo cambio
 * (ClearInstructionCache e InvalidateCacheRange). No hace nada si el motor
 * nativo no esta activo.
 */
void CodeChanged();

/**
 * Lo mismo para un rango conocido, LIMPIANDO ANTES LA CACHE DE DATOS (0.3.8.6).
 * AzNativeIcacheAll solo invalida la de instrucciones; lo que el emulador acaba
 * de escribir (las reubicaciones de un CRO, codigo cargado a mano) puede seguir
 * en la L1 de datos sin llegar a la L2, de donde la de instrucciones vuelve a
 * leer: el invitado ejecutaria lo de antes. Smash Bros. carga y reubica CROs sin
 * parar, y con el motor nativo acababa con el segundo nucleo en 0x5000-0xc000.
 */
void CodeChangedRange(Memory::MemorySystem& memory, u32 va, u32 bytes);

/**
 * EL NUMERO DE SVC DE LA INSTRUCCION QUE ACABA DE SALTAR.
 *
 * El modulo devuelve el contexto con pc apuntando a la instruccion SIGUIENTE a
 * la SVC (es lo que la excepcion deja en lr), asi que la instruccion esta en
 * pc-4 en ARM y en pc-2 en Thumb. El numero va en los 24 bits bajos
 * (0xEF<imm24>) o en los 8 bajos en Thumb (0xDF<imm8>).
 *
 * Se lee por la MEMORIA DEL EMULADOR, no por la del invitado: el emulador ya
 * tiene esa pagina mapeada en su espacio y leerla no cuesta un cambio de mundo.
 * Devuelve false si esa direccion no esta mapeada, que seria un invitado
 * ejecutando donde no debe.
 */
bool DecodeSvc(Memory::MemorySystem& memory, u32 pc, bool thumb, u32& out_svc);

/**
 * EJECUTAR UNA RODAJA DEL HILO ACTUAL EN NATIVO, SI SE PUEDE.
 *
 * Diseno deliberadamente cobarde, y es lo importante de esta funcion: **el
 * interprete sigue siendo la autoridad**. Lo nativo solo se queda con lo
 * facil, y en cuanto pasa cualquier cosa que no sabe tratar -- un fallo de
 * memoria, una instruccion indefinida, un modo raro -- devuelve el estado
 * EXACTO en el que esta y responde false, y el interprete sigue desde ahi como
 * si nada hubiera pasado. Asi lo peor que puede hacer una ejecucion nativa es
 * no hacer nada.
 *
 * Eso tiene una consecuencia util: un fallo de memoria del invitado (un
 * registro de la GPU, por ejemplo) no hay que resolverlo aqui. Se devuelve el
 * control y el interprete reejecuta esa instruccion y lo trata con el codigo
 * que ya existe y ya funciona.
 *
 * `ticks` recibe los ciclos de ARM11 que hay que cobrarle al reloj emulado.
 * Devuelve false sin tocar nada si no esta disponible o si no se encendio.
 */
/// `then_interpreter` (0.3.9.0): la rodaja acabo en un fallo (memoria,
/// instruccion indefinida, codigo): el pc ya apunta a la instruccion que
/// fallo y el interprete tiene que ejecutarla en esta misma rodaja.
bool TryRun(ARMul_State& state, u64 max_instructions, u32& ticks, bool& then_interpreter);

/// Se enciende con "jit_nativo=1" en ajustes.txt. Apagado por defecto: esto
/// ejecuta codigo del juego de verdad en la CPU de la consola.
extern bool enabled;

/// Deja en crash.txt lo que ha hecho el motor desde la ultima vez. Sin esto no
/// hay forma de saber si esta ejecutando algo o abandonando siempre, que es
/// justo lo que me falto para diagnosticar el cuelgue del 24-10.
void LogStats();

/// Suelta las tablas y el mundo. Al cerrar el juego.
void Release();

} // namespace Core::Native
