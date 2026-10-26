// Copyright 2026 Azahar Vita port
// Licensed under GPLv2 or any later version

#include "core/arm/native/arm_native.h"

#ifdef __PSVITA__

#include <cstring>
#include <fmt/format.h>
#include <psp2/appmgr.h>
#include <psp2/io/stat.h>
#ifdef AZAHAR_TAIHEN
#include <taihen.h>
#endif
#include "common/vita_diag.h"

namespace {
/// Como el NoteFmt del rasterizador: VitaNote toma una cadena ya formada.
template <typename... Args>
void Note(fmt::format_string<Args...> format, Args&&... args) {
    Common::VitaNote("nativo", fmt::format(format, std::forward<Args>(args)...).c_str());
}
} // Anonymous namespace

#include "core/arm/native/aznative.h"
#include "core/arm/skyeye_common/arm_regformat.h"
#include "core/arm/skyeye_common/armstate.h"
#include "core/core.h"
#include "core/hle/kernel/svc.h"
#include "core/memory.h"

namespace Core::Native {
namespace {

/**
 * ESTA RESUELTO ESE STUB? Un stub de importacion debil son tres palabras que el
 * cargador reescribe al arrancar el ejecutable. Resuelto queda
 * "movw r12,#n / svc #0 / bx lr"; sin resolver, "sub r12,pc,#8 / mov pc,#0", y
 * llamarlo salta a la direccion 0 y mata el proceso. LEERLO es inofensivo.
 *
 * Esto se aprendio a golpes: en el spike se miraba la palabra 0 buscando el
 * centinela de enlace 0x00010008, pero el cargador reescribe el stub SIEMPRE,
 * asi que la comprobacion daba por resuelto lo que no lo estaba y el primer
 * salto era a 0.
 */
constexpr u32 kStubUnresolvedWord1 = 0xE3A0F000u; // mov pc, #0
constexpr u32 kStubLinkSentinel = 0x00010008u;

bool StubResolved(const void* fn) {
    const u32* const w = static_cast<const u32*>(fn);
    return w[0] != kStubLinkSentinel && w[1] != kStubUnresolvedWord1;
}

enum class State { Unknown, Absent, Present };
State g_state = State::Unknown;
u32 g_version = 0;
bool g_world = false;
/// Cuantas veces se ha reflejado la tabla, para no llenar el log.
u32 g_mirrors = 0;
/// Reflejos de tramo (MapPages): ocurren constantemente, asi que solo se cuentan.
u32 g_ranges = 0;
u32 g_range_pages = 0;
u32 g_range_cleared = 0;
/// Veces que el emulador cambio codigo del invitado (ver CodeChanged).
u32 g_code_changes = 0;

} // Anonymous namespace

/// El nombre del motivo de salida, para el log.
const char* ReasonName(u32 reason) {
    switch (reason) {
    case AZ_REASON_UNDEF:
        return "indefinida";
    case AZ_REASON_SVC:
        return "svc";
    case AZ_REASON_PABT:
        return "fallo de codigo";
    case AZ_REASON_DABT:
        return "fallo de datos";
    case AZ_REASON_IRQ:
        return "interrupcion";
    case AZ_REASON_BUDGET:
        return "tiempo";
    case AZ_REASON_MODE:
        return "modo";
    default:
        return "?";
    }
}


u32 ModuleVersion() {
    return g_version;
}

bool Available() {
    if (g_state != State::Unknown) {
        return g_state == State::Present;
    }
    g_state = State::Absent;
    // Los cinco que hacen falta para esta pieza. Se mira CADA uno: el modulo
    // podria ser de una version vieja que exporta unos y no otros.
    if (!StubResolved(reinterpret_cast<const void*>(&AzNativeVersion)) ||
        !StubResolved(reinterpret_cast<const void*>(&AzNativeWorldCreate)) ||
        !StubResolved(reinterpret_cast<const void*>(&AzNativeMapTable)) ||
        !StubResolved(reinterpret_cast<const void*>(&AzNativeUnmapTable)) ||
        !StubResolved(reinterpret_cast<const void*>(&AzNativeWorldDestroy))) {
        Note("el modulo de kernel no esta cargado: todo por el JIT, como siempre (abre una "
             "vez la app de prueba para cargarlo)");
        return false;
    }
    g_version = static_cast<u32>(AzNativeVersion());
    if (g_version != AZ_NATIVE_VERSION) {
        Note("el modulo cargado es 0x{:08x} y este Azahar espera 0x{:08x}: no se "
                            "usa. Apaga la consola del todo y vuelve a abrir la app de prueba",
                            g_version, AZ_NATIVE_VERSION);
        return false;
    }
    g_state = State::Present;
    Note("modulo de kernel 0x{:08x} presente", g_version);
    return true;
}

/**
 * CARGAR EL MODULO SIN LA APP DE PRUEBA (0.3.7.9). Un modulo de kernel se queda
 * cargado hasta que la consola se apaga del todo, y hasta ahora solo lo cargaba
 * la app de prueba (AZNT00001): tras cualquier apagado -- y un GPUCRASH apaga --
 * Azahar arrancaba sin el y todo iba por el JIT sin que nadie lo notara (el
 * 25-10, todas las partidas). Ahora lo carga Azahar, igual que la app de
 * prueba: taiLoadStartKernelModule y relanzarse, porque las importaciones
 * debiles del modulo solo las resuelve el cargador al arrancar el ejecutable.
 * El argumento kRelaunchArg evita el bucle: si tras relanzar sigue sin estar,
 * se avisa y se sigue con el JIT.
 */
void LoadModuleIfMissing(int argc, char** argv) {
    if (StubResolved(reinterpret_cast<const void*>(&AzNativeVersion))) {
        return;
    }
    constexpr const char* kRelaunchArg = "--aznative-relanzada";
    for (int i = 0; i < argc; i++) {
        if (argv[i] != nullptr && std::strcmp(argv[i], kRelaunchArg) == 0) {
            Note("relanzada y el modulo sigue sin cargarse: todo por el JIT");
            return;
        }
    }
#ifdef AZAHAR_TAIHEN
    if (!StubResolved(reinterpret_cast<const void*>(&taiLoadStartKernelModuleForUser))) {
        Note("sin taiHEN: el modulo no se puede cargar desde aqui");
        return;
    }
    // El de la carpeta de Azahar primero (se puede subir ahi por FTP), y si no,
    // el de la app de prueba, que es donde ha estado siempre.
    const char* path = nullptr;
    for (const char* candidate :
         {"ux0:app/AZHR00001/aznative.skprx", "ux0:app/AZNT00001/aznative.skprx"}) {
        SceIoStat stat{};
        if (sceIoGetstat(candidate, &stat) >= 0) {
            path = candidate;
            break;
        }
    }
    if (path == nullptr) {
        Note("no hay aznative.skprx ni en AZHR00001 ni en AZNT00001: todo por el JIT");
        return;
    }
    const SceUID id = taiLoadStartKernelModule(path, 0, nullptr, 0);
    // 0x8002d013: ya estaba cargado, pero despues de que arrancara este proceso
    // (sus importaciones no lo ven). Relanzar lo arregla igual.
    if (id < 0 && static_cast<u32>(id) != 0x8002d013u) {
        // 0x90010009 es TAI_ERROR_NOT_ALLOWED: taiHEN no deja cargar modulos de
        // kernel a un eboot "safe" (25-10). Hay que generarlo con
        // -DAZAHAR_UNSAFE=ON, como la app de prueba (vita_create_self UNSAFE).
        Note("taiLoadStartKernelModule({}) = 0x{:08x}: todo por el JIT ({})", path,
             static_cast<u32>(id),
             static_cast<u32>(id) == 0x90010009u
                 ? "el eboot es safe: hay que generarlo con -DAZAHAR_UNSAFE=ON"
                 : "activa 'Enable unsafe homebrew' en los ajustes de HENkaku");
        return;
    }
    Note("modulo cargado desde {} (0x{:08x}): se relanza Azahar para usarlo", path,
         static_cast<u32>(id));
    char arg[] = "--aznative-relanzada";
    char* const args[] = {arg, nullptr};
    const int exec = sceAppMgrLoadExec("app0:eboot.bin", args, nullptr);
    Note("no se pudo relanzar (0x{:08x}): todo por el JIT hasta el proximo arranque",
         static_cast<u32>(exec));
#else
    Note("compilado sin taiHEN: el modulo no se puede cargar desde aqui");
#endif
}

bool EnsureWorld() {
    if (g_world) {
        return true;
    }
    if (!Available()) {
        return false;
    }
    AzWorldInfo info;
    std::memset(&info, 0, sizeof(info));
    const int rc = AzNativeWorldCreate(&info);
    /**
     * -3 ES EXITO. AzNativeWorldCreate devuelve -3 cuando YA hay una tabla
     * creada, y eso es justo el estado que se quiere: la app de prueba (la que
     * carga el modulo) deja una puesta, y el modulo sigue vivo entre apps. En
     * 0.3.6.2 esto se tomaba por fallo, se reintentaba en cada llamada y
     * escribia una linea en el log cada vez -- abriendo y cerrando un fichero
     * de la tarjeta miles de veces por segundo. Eso, y no el reflejo, era lo
     * que arrastraba el emulador.
     */
    const bool ya_estaba = rc >= 0 && info.rc == -3 && info.paso == AZ_MUNDO_LISTO;
    if (!ya_estaba && (rc < 0 || info.rc < 0 || info.paso != AZ_MUNDO_LISTO)) {
        // Una sola vez: si no se puede, no se va a poder en el siguiente lote.
        g_state = State::Absent;
        Note("no se pudo crear la tabla del invitado: rc_llamada={:#x} rc={:#x} paso={}. Se "
             "sigue con el JIT",
             static_cast<u32>(rc), static_cast<u32>(info.rc), info.paso);
        return false;
    }
    g_world = true;
    Note("tabla del invitado {}: ttbr0 de Sony {:#010x}, el nuestro {:#010x} ({} entradas "
         "copiadas)",
         ya_estaba ? "ya estaba puesta" : "creada", info.ttbr0_sony, info.ttbr0_az,
         info.copiadas);
    return true;
}

void MirrorPageTable(u8* const* raw) {
    if (raw == nullptr || !EnsureWorld()) {
        return;
    }
    /**
     * TABLAS LIMPIAS AL EMPEZAR, y es importante: el modulo sigue cargado entre
     * arranques de Azahar, asi que las tablas de segundo nivel de la sesion
     * ANTERIOR siguen puestas. Pero la memoria del emulador cae en fisicas
     * distintas cada vez (4d064000 un arranque, 4d051000 el siguiente, medido),
     * de modo que toda entrada que esta sesion no vuelva a escribir apuntaria a
     * memoria de la sesion pasada, que ahora es de otro. Soltarlas hace que la
     * siguiente llamada cree el bloque de cero.
     */
    if (g_mirrors == 0) {
        AzMapTableInfo limpio;
        std::memset(&limpio, 0, sizeof(limpio));
        AzNativeUnmapTable(&limpio);
    }
    /**
     * Solo el primer gigabyte. Ahi cabe todo el mapa del 3DS -- de
     * PROCESS_IMAGE_VADDR (0x00100000) a NEW_LINEAR_HEAP_VADDR (0x30000000) --
     * y es exactamente lo que cubre TTBR0 con TTBCR.N=2 en la Vita. Por encima
     * esta TTBR1, donde viven el kernel de Sony y el propio emulador, y ahi no
     * se toca nada.
     */
    constexpr u32 kPages = 0x40000000u / Memory::CITRA_PAGE_SIZE;
    AzMapTableInfo info;
    std::memset(&info, 0, sizeof(info));
    const int rc = AzNativeMapTable(raw, 0, kPages, &info);
    // Las primeras veces y luego de vez en cuando: esto puede llamarse mucho.
    const bool note = g_mirrors < 4 || (g_mirrors & 0x3Fu) == 0;
    g_mirrors++;
    if (rc < 0 || info.rc < 0) {
        // Igual que arriba: una sola linea y se deja de intentar.
        g_state = State::Absent;
        Note("reflejar la tabla fallo: rc_llamada={:#x} rc={:#x} pagina={:#x}. Se sigue con el "
             "JIT",
             static_cast<u32>(rc), static_cast<u32>(info.rc), info.fallo_pagina);
        return;
    }
    if (!note) {
        return;
    }
    Note("tabla reflejada #{}: {} paginas en {} megabytes, {} sin fisica, {} SIN ALINEAR, {} us",
         g_mirrors, info.mapeadas, info.megas, info.sin_fisica, info.sin_alinear, info.us);
    if (info.sin_alinear != 0) {
        // Si esto no es cero, la memoria del invitado no esta alineada a pagina
        // y la MMU no la puede mapear: ver GuestRam en core/memory.cpp.
        Note("   AVISO: {} paginas sin alinear (la primera, fisica {:#010x}): esas no se mapean",
             info.sin_alinear, info.primera_sin_alinear);
    }
    if (info.mapeadas != 0) {
        Note("   va->pa: {:08x}->{:08x} {:08x}->{:08x} {:08x}->{:08x}",
                            info.muestra_va[0], info.muestra_pa[0], info.muestra_va[1],
                            info.muestra_pa[1], info.muestra_va[2], info.muestra_pa[2]);
    }
}

bool DecodeSvc(Memory::MemorySystem& memory, u32 pc, bool thumb, u32& out_svc) {
    if (thumb) {
        const u8* const p = memory.GetPointer(pc - 2);
        if (p == nullptr) {
            return false;
        }
        u16 inst = 0;
        std::memcpy(&inst, p, sizeof(inst));
        if ((inst & 0xFF00u) != 0xDF00u) {
            return false;
        }
        out_svc = inst & 0x00FFu;
        return true;
    }
    const u8* const p = memory.GetPointer(pc - 4);
    if (p == nullptr) {
        return false;
    }
    u32 inst = 0;
    std::memcpy(&inst, p, sizeof(inst));
    /* Cualquier codigo de condicion vale: el invitado puede tener una SVC
     * condicional, y si llego a saltar es que la condicion se cumplio. Lo que
     * se comprueba es que sea una SVC de verdad. */
    if ((inst & 0x0F000000u) != 0x0F000000u) {
        return false;
    }
    out_svc = inst & 0x00FFFFFFu;
    return true;
}


/**
 * EL MOTOR. Traduce el estado del interprete al del invitado nativo, corre, y
 * vuelve a traducir. Ver la nota de TryRun en la cabecera: el interprete manda,
 * esto solo se queda con lo facil.
 */
namespace {

/// Tope de microsegundos de una rodaja nativa, pase lo que pase.
constexpr u32 kSliceUs = 500;

/**
 * INSTRUCCIONES DEL INVITADO POR MICROSEGUNDO NATIVO, de los 883 MIPS medidos
 * en hardware. Se usa para dos cosas: convertir las instrucciones que el reloj
 * PERMITE en el tiempo que se le puede dar al invitado, y al revés, estimar las
 * que ha ejecutado para cobrarselas.
 */
constexpr u32 kInstrPerUs = 883;

/**
 * POR DEBAJO DE ESTO NO VALE LA PENA: cada salida del mundo nativo cuesta 4,9
 * us medidos, asi que una rodaja de 10 us seria medio tiempo de peaje. Esas se
 * las queda el interprete.
 */
constexpr u32 kMinSliceUs = 20;

/**
 * QUE SE LE COBRA AL RELOJ DEL INVITADO. Un tick del reloj emulado es UNA
 * INSTRUCCION del invitado: el interprete devuelve `num_instrs` como ticks
 * (arm_dyncom_interpreter.cpp), asi que aqui hay que estimar instrucciones, no
 * ciclos.
 *
 * El primer modelo cobraba `us * 1730` y estaba mal dos veces: aplicaba los
 * ciclos por instruccion del ARM11 cuando no hacian falta, y cobraba el tiempo
 * ENTERO de la rodaja incluyendo el peaje del cambio de mundo. Con el 94 % de
 * las rodajas saliendo por SVC a los pocos microsegundos, ese peaje es casi
 * todo el tiempo medido: le cobraba al juego decenas de miles de instrucciones
 * que nunca ejecuto. El resultado fue "velocidad 86 %" con 13 fps de verdad.
 *
 * Ahora: instrucciones = (tiempo medido - los cambios de mundo) x 883 MIPS,
 * los dos numeros medidos en hardware. Sigue siendo una estimacion, y la buena
 * necesita contar instrucciones de verdad; el contador del PMU salio "no
 * disponible" desde la app, pero el modulo es privilegiado y desde ahi deberia
 * poder habilitarse. Eso es lo siguiente.
 */
constexpr u32 kSwitchUs = 5;
constexpr u32 kTicksFloor = 32;

u32 g_native_slices = 0;
u32 g_native_svcs = 0;
u32 g_native_bails = 0;
/// Instrucciones cobradas al reloj. Comprobacion cruzada: con el JIT se midio
/// que Zafiro hace ~345.000 instrucciones por vblank, asi que el modelo deberia
/// quedar en ese orden y no diez veces mas.
u64 g_native_ticks = 0;
/// El motivo del ultimo abandono, para el log.
const char* g_last_bail = nullptr;

void StateToCtx(const ARMul_State& state, AzGuestCtx& ctx) {
    for (int i = 0; i < 16; i++) {
        ctx.r[i] = state.Reg[i];
    }
    ctx.cpsr = state.Cpsr;
    ctx.tls = state.CP15[CP15_THREAD_URO];
    ctx.fpscr = state.VFP[VFP_FPSCR];
    for (int i = 0; i < 64; i++) {
        // ExtReg son 64 palabras (s0-s63) y d[] son 32 dobles: la misma memoria.
        reinterpret_cast<u32*>(ctx.d)[i] = state.ExtReg[i];
    }
}

void CtxToState(const AzGuestCtx& ctx, ARMul_State& state) {
    for (int i = 0; i < 16; i++) {
        state.Reg[i] = ctx.r[i];
    }
    /**
     * EL CPSR, CON CUIDADO. El interprete lleva las banderas en campos sueltos
     * (NFlag, ZFlag...) por velocidad y Cpsr solo esta al dia en los bordes de
     * rodaja. Al volver de nativo hay que escribir las DOS cosas, o el
     * interprete seguiria con las banderas de antes de la rodaja y tomaria los
     * saltos condicionales al reves.
     */
    state.Cpsr = ctx.cpsr;
    state.NFlag = (ctx.cpsr >> 31) & 1u;
    state.ZFlag = (ctx.cpsr >> 30) & 1u;
    state.CFlag = (ctx.cpsr >> 29) & 1u;
    state.VFlag = (ctx.cpsr >> 28) & 1u;
    state.TFlag = (ctx.cpsr >> 5) & 1u;
    state.Mode = ctx.cpsr & 0x1Fu;
    state.VFP[VFP_FPSCR] = ctx.fpscr;
    for (int i = 0; i < 64; i++) {
        state.ExtReg[i] = reinterpret_cast<const u32*>(ctx.d)[i];
    }
}

} // Anonymous namespace

bool enabled = false;

/// El resumen, sin reiniciar los contadores (lo llama tambien TryRun).
void LogStatsNow() {
    Note("rodajas {} | svc despachadas {} | abandonos {} (el ultimo por: {}) | tramos {} "
         "({} paginas puestas, {} quitadas) | {} instr cobradas ({} por rodaja) | codigo cambiado {}",
         g_native_slices, g_native_svcs, g_native_bails,
         g_last_bail != nullptr ? g_last_bail : "-", g_ranges, g_range_pages, g_range_cleared,
         g_native_ticks, g_native_slices != 0 ? g_native_ticks / g_native_slices : 0,
         g_code_changes);
}

bool TryRun(ARMul_State& state, u64 max_instructions, u32& ticks, bool& then_interpreter) {
    then_interpreter = false;
    ticks = 0;
    if (!enabled || max_instructions == 0 || !Available() || !g_world) {
        return false;
    }
    /**
     * Solo modo usuario. El codigo del 3DS en modo sistema o supervisor toca
     * cosas que el modulo no reproduce (registros bancados, SPSR propios), asi
     * que eso se lo queda el interprete entero.
     */
    if ((state.Cpsr & 0x1Fu) != 0x10u) {
        g_last_bail = "modo";
        g_native_bails++;
        return false;
    }

    static AzGuestCtx ctx;
    /**
     * EL PRESUPUESTO SALE DE LA RODAJA QUE DA EL RELOJ, no de un numero fijo.
     *
     * El reloj emulado pide rodajas cortas porque al final de cada una hay
     * eventos: el vblank, el DSP, despertar hilos. Correr 500 us siempre son
     * unas 440.000 instrucciones del invitado, o sea pasarse el siguiente
     * evento por cientos de miles de instrucciones. El interprete no tiene este
     * problema porque cuenta instrucciones de verdad; aqui hay que estimarlas.
     *
     * Si el reloj da para menos de kMinSliceUs, la rodaja se la queda el
     * interprete entera: por debajo de eso el peaje del cambio de mundo seria
     * la mitad del tiempo.
     */
    const u64 budget = max_instructions / kInstrPerUs;
    if (budget < kMinSliceUs) {
        g_last_bail = "rodaja corta";
        g_native_bails++;
        return false;
    }
    const u32 budget_us = budget > kSliceUs ? kSliceUs : static_cast<u32>(budget);

    StateToCtx(state, ctx);

    AzRunInfo info;
    std::memset(&info, 0, sizeof(info));
    // El monitor exclusivo real, limpio a la entrada y a la salida: un STREX
    // nativo cuyo LDREX fue en otra rodaja (de este o del otro nucleo) falla.
    // Ver ARM_DynCom::ExecuteInstructions (0.3.8.7).
    __asm__ volatile("clrex" ::: "memory");
    const int rc = AzNativeRun(&ctx, AZ_RUN_WORLD | AZ_RUN_IRQ | AZ_RUN_VFP, budget_us, &info);
    __asm__ volatile("clrex" ::: "memory");
    if (rc < 0) {
        // La llamada en si fallo: no se ha ejecutado nada, el estado sigue como
        // estaba y el interprete no se entera.
        g_last_bail = "llamada";
        g_native_bails++;
        return false;
    }

    g_native_slices++;
    const u32 pc_antes = state.Reg[15];
    const u32 lr_antes = state.Reg[14];
    CtxToState(ctx, state);
    /**
     * EL SALTO FUERA DEL CODIGO (0.3.8.9, diagnostico). En Smash Bros. con el
     * motor nativo el segundo nucleo acaba ejecutando ceros por debajo de
     * 0x00100000 (donde empieza el codigo de cualquier juego) y la emulacion
     * se arrastra pagina a pagina. Para saber de donde salta: las ultimas
     * rodajas de cada nucleo en un anillo, y la primera vez que una sale (o
     * entra) por debajo de esa direccion, el anillo y los registros al log.
     */
    {
        struct SliceTrace {
            const void* core;
            u32 in_pc, in_lr, out_pc, out_lr, reason;
        };
        static SliceTrace ring[16];
        static u32 ring_at = 0;
        static u32 wild_notes = 0;
        ring[ring_at++ & 15u] = {&state, pc_antes, lr_antes, ctx.r[15], ctx.r[14], ctx.reason};
        const bool wild = ctx.r[15] < 0x00100000u && pc_antes >= 0x00100000u;
        if (wild && wild_notes < 3) {
            wild_notes++;
            Note("salto fuera del codigo: nucleo {} rodaja {} entra pc {:#010x} lr {:#010x} sale "
                 "pc {:#010x} motivo {} | r0 {:08x} r1 {:08x} r2 {:08x} r3 {:08x} r4 {:08x} r5 "
                 "{:08x} r6 {:08x} r7 {:08x} r8 {:08x} r9 {:08x} r10 {:08x} r11 {:08x} r12 {:08x} "
                 "sp {:08x} lr {:08x} cpsr {:08x}",
                 fmt::ptr(&state), g_native_slices, pc_antes, lr_antes, ctx.r[15],
                 ReasonName(ctx.reason), ctx.r[0], ctx.r[1], ctx.r[2], ctx.r[3], ctx.r[4],
                 ctx.r[5], ctx.r[6], ctx.r[7], ctx.r[8], ctx.r[9], ctx.r[10], ctx.r[11],
                 ctx.r[12], ctx.r[13], ctx.r[14], ctx.cpsr);
            for (u32 i = 0; i < 16; i++) {
                const SliceTrace& t = ring[(ring_at + i) & 15u];
                if (t.core == nullptr) {
                    continue;
                }
                Note("  antes: nucleo {} entra pc {:#010x} lr {:#010x} -> sale pc {:#010x} lr "
                     "{:#010x} motivo {}",
                     t.core, t.in_pc, t.in_lr, t.out_pc, t.out_lr, ReasonName(t.reason));
            }
        }
    }
    /* El peaje de cada entrada y salida no es trabajo del invitado: ver
     * kSwitchUs. El suelo evita cobrar cero, que dejaria el reloj clavado. */
    const u32 peaje = info.entries * kSwitchUs;
    const u32 utiles = info.us > peaje ? info.us - peaje : 0u;
    ticks = utiles * kInstrPerUs;
    if (ticks < kTicksFloor) {
        ticks = kTicksFloor;
    }
    g_native_ticks += ticks;

    /**
     * LAS PRIMERAS RODAJAS, UNA POR LINEA. El motivo de no tener esto es que la
     * telemetria iba en el volcado periodico, que solo ocurre cuando el juego ya
     * corre -- y si el juego no arranca, no hay nada que leer. Dos diagnosticos
     * seguidos los tuve que sacar leyendo el codigo. Asi no.
     */
    /* Y un resumen cada 2000 rodajas, para ver si el reparto cambia con el
     * tiempo sin llenar el log. */
    if ((g_native_slices % 2000u) == 0u) {
        LogStatsNow();
    }
    if (g_native_slices <= 24) {
        Note("#{} pc {:#010x} -> {:#010x} motivo {} ({} us, {} entradas, {} irq)",
             g_native_slices, pc_antes, ctx.r[15], ReasonName(ctx.reason), info.us, info.entries,
             info.irq_exits);
    }

    switch (ctx.reason) {
    case AZ_REASON_SVC: {
        /**
         * La SVC del invitado, al mismo codigo que usa el interprete. El pc que
         * devuelve el modulo ya apunta a la instruccion siguiente, que es
         * exactamente donde el interprete espera continuar.
         */
        u32 svc = 0;
        if (!DecodeSvc(state.system.Memory(), ctx.r[15], (ctx.cpsr & (1u << 5)) != 0, svc)) {
            g_last_bail = "svc ilegible";
            g_native_bails++;
            return true; // el estado ya esta escrito; el interprete sigue
        }
        g_native_svcs++;
        Kernel::SVCContext{state.system}.CallSVC(svc & 0xFFFFu);
        return true;
    }
    case AZ_REASON_BUDGET:
        // Se acabo la rodaja sin novedad: lo normal.
        return true;
    default: {
        /**
         * Fallo de memoria, instruccion indefinida, lo que sea: el interprete
         * ejecuta esa instruccion con el codigo que ya existe. Esto NO es un
         * error, es el reparto de trabajo.
         *
         * EL PC DEL FALLO, NO EL DE VUELTA (0.3.9.0). El modulo devuelve el lr
         * de la excepcion tal cual (srsdb en vectors.S; solo corrige el de las
         * IRQ), y ese no es la instruccion que fallo: aborto de datos +8,
         * aborto de codigo +4, indefinida +4 en ARM y +2 en Thumb. Hasta aqui
         * se daba por bueno y ademas la rodaja siguiente volvia a nativo en ese
         * pc: la instruccion que fallaba no se ejecutaba nunca, y con un aborto
         * de datos tampoco la siguiente. De vez en cuando eso rompia el juego:
         * Smash Bros. con el segundo nucleo en nativo acababa en su rutina de
         * error y New Super Mario Bros. 2 saltando a 0, a los 1-3 minutos.
         */
        const bool thumb = (ctx.cpsr & (1u << 5)) != 0;
        u32 back = 0;
        if (ctx.reason == AZ_REASON_DABT) {
            back = 8;
        } else if (ctx.reason == AZ_REASON_PABT) {
            back = 4;
        } else if (ctx.reason == AZ_REASON_UNDEF) {
            back = thumb ? 2 : 4;
        }
        state.Reg[15] = ctx.r[15] - back;
        then_interpreter = true;
        g_last_bail = ReasonName(ctx.reason);
        g_native_bails++;
        return true;
    }
    }
}

void LogStats() {
    if (!enabled) {
        return;
    }
    if (g_native_slices == 0 && g_native_bails == 0) {
        Note("el motor no ha entrado ni una vez");
        return;
    }
    Note("rodajas {} | svc despachadas {} | abandonos {} (el ultimo por: {})", g_native_slices,
         g_native_svcs, g_native_bails, g_last_bail != nullptr ? g_last_bail : "-");
    g_native_slices = 0;
    g_native_svcs = 0;
    g_native_bails = 0;
    /* Tambien este, que si no el "por rodaja" sale sin sentido: el total
     * seguia creciendo toda la partida mientras las rodajas se reiniciaban
     * cada 2000, y daba 3 millones de instrucciones por rodaja (imposible, el
     * tope de una rodaja de 500 us son 441.500). */
    g_native_ticks = 0;
}


void MirrorRange(u8* const* raw, u32 first_page, u32 pages) {
    if (raw == nullptr || pages == 0 || g_state != State::Present || !g_world) {
        return;
    }
    // Solo el primer gigabyte: lo de arriba es TTBR1 y ahi no se toca nada.
    constexpr u32 kMaxPage = 0x40000000u / Memory::CITRA_PAGE_SIZE;
    if (first_page >= kMaxPage) {
        return;
    }
    if (first_page + pages > kMaxPage) {
        pages = kMaxPage - first_page;
    }
    AzMapTableInfo info;
    std::memset(&info, 0, sizeof(info));
    const int rc = AzNativeMapTable(raw, first_page, pages, &info);
    if (rc < 0 || info.rc < 0) {
        g_state = State::Absent;
        Note("reflejar el tramo {:#x}+{} fallo: rc={:#x}. Se sigue con el JIT", first_page, pages,
             static_cast<u32>(info.rc));
        return;
    }
    g_range_pages += info.mapeadas;
    g_range_cleared += info.borradas;
    g_ranges++;
}


void CodeChanged() {
    if (!enabled || g_state != State::Present) {
        return;
    }
    AzNativeIcacheAll();
    g_code_changes++;
}

void CodeChangedRange(Memory::MemorySystem& memory, u32 va, u32 bytes) {
    if (!enabled || g_state != State::Present) {
        return;
    }
    // Pagina a pagina: la memoria del invitado no es contigua en el emulador.
    // AzNativeCleanRange limpia por la direccion del que llama (L1 y L2).
    const u32 end = va + bytes;
    u32 at = va;
    while (at < end) {
        const u32 page_end = (at | (Memory::CITRA_PAGE_SIZE - 1)) + 1;
        const u32 chunk = (end < page_end ? end : page_end) - at;
        if (u8* const host = memory.GetPointer(at)) {
            AzNativeCleanRange(host, chunk);
        }
        at += chunk;
    }
    AzNativeIcacheAll();
    g_code_changes++;
}


/**
 * AL CERRAR CADA SESION, todo fuera (0.3.8.3). Hasta ahora no la llamaba nadie,
 * y MirrorPageTable solo limpia las tablas del modulo en el PRIMER reflejo del
 * proceso (g_mirrors == 0). Una segunda partida en el mismo proceso -- otro
 * juego desde el menu, o el mismo juego relanzandose, como hace Zafiro Alfa al
 * arrancar -- heredaba las entradas de la primera, que apuntan a la memoria del
 * invitado anterior, ya liberada: el 25-10 la emulacion se quedaba parada en
 * cuanto empezaba (Inazuma Eleven GO tras Zafiro, y Zafiro tras su propio
 * relanzamiento, con pc 0). Lo llama System::Shutdown, con los nucleos ya
 * destruidos, y con g_mirrors a cero el siguiente reflejo empieza limpio.
 */
void Release() {
    if (!Available()) {
        return;
    }
    AzMapTableInfo t;
    std::memset(&t, 0, sizeof(t));
    AzNativeUnmapTable(&t);
    if (g_world) {
        AzWorldInfo w;
        std::memset(&w, 0, sizeof(w));
        AzNativeWorldDestroy(&w);
        g_world = false;
    }
    g_mirrors = 0;
    g_ranges = 0;
    g_range_pages = 0;
    g_range_cleared = 0;
    g_code_changes = 0;
    g_native_slices = 0;
    g_native_svcs = 0;
    g_native_bails = 0;
    g_native_ticks = 0;
    g_last_bail = nullptr;
    Note("sesion cerrada: tablas del invitado y mundo liberados");
}

} // namespace Core::Native

#else // !__PSVITA__

namespace Core::Native {
bool Available() {
    return false;
}
void LoadModuleIfMissing(int, char**) {}
u32 ModuleVersion() {
    return 0;
}
bool EnsureWorld() {
    return false;
}
void MirrorPageTable(u8* const*) {}
bool DecodeSvc(Memory::MemorySystem&, u32, bool, u32&) {
    return false;
}
bool enabled = false;
bool TryRun(ARMul_State&, u64, u32& ticks, bool& then_interpreter) {
    then_interpreter = false;
    ticks = 0;
    return false;
}
void MirrorRange(u8* const*, u32, u32) {}
void CodeChanged() {}
void CodeChangedRange(Memory::MemorySystem&, u32, u32) {}
void LogStats() {}
void Release() {}
} // namespace Core::Native

#endif
