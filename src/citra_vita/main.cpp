// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.
//
// Punto de entrada de Azahar en PS Vita.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <pthread.h>
#include <string>
#include <system_error>
#include <thread>
#include <typeinfo>
#include <vector>

#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <malloc.h>
#include <psp2/power.h>
#include <vita2d.h>

#include <cryptopp/integer.h>

#include "citra_vita/vita_covers.h"
#include "citra_vita/vita_home_menu.h"
#include "citra_vita/vita_i18n.h"
#include "citra_vita/vita_input.h"
#include "citra_vita/vita_jit_probe.h"
#include "citra_vita/vita_version.h"
#include "citra_vita/vita_window.h"
#include "common/file_util.h"
#include "common/logging/backend.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "common/vita_diag.h"
#include "core/arm/dyncom/arm_dyncom_jit.h"
#include "core/arm/dyncom/arm_dyncom_trans.h"
#include "core/arm/native/arm_native.h"
#include "core/core.h"
#include "core/frontend/applets/default_applets.h"
#include "core/frontend/image_interface.h"
#include "core/hle/service/cfg/cfg.h"
#include "core/hle/service/service.h"
#include "core/loader/loader.h"
#include "video_core/gpu.h"
#include "common/write_epochs.h"

namespace Service::Y2R {
/// Ver Y2rWorker en core/hle/service/cam/y2r_u.cpp (0.4.0.5).
extern std::atomic<bool> g_y2r_async;
} // namespace Service::Y2R
#include "video_core/pica/pica_core.h"
#include "core/hw/y2r.h"
#include "video_core/renderer_gxm/gxm_cg.h"
#include "video_core/shader/generator/cg_fs_shader_gen.h"
#include "audio_core/hle/hle.h"
#include "video_core/renderer_gxm/rasterizer_gxm.h"
#include "video_core/shader/generator/cg_vs_shader_gen.h"
#include "video_core/shader/shader_neon_jit.h"
#include "video_core/renderer_software/sw_rasterizer.h"

#ifndef AZAHAR_HEAP_MB
#define AZAHAR_HEAP_MB 192
#endif

extern "C" {
/**
 * Tamano del heap de la aplicacion.
 *
 * La newlib del VitaSDK reserva 128 MB por defecto, que ni siquiera dan para
 * la FCRAM del 3DS (128 MB justos). Sumando VRAM (6 MB), la RAM extra del
 * New 3DS (4 MB), el DSP (512 KB) y las tablas de paginas (unos 5 MB por
 * proceso: 2^20 entradas de puntero mas sus atributos), el minimo ronda los
 * 150 MB, y la cache del rasterizador crece por encima.
 *
 * libc reserva este bloque de una pieza ANTES de main(): si no cabe en el
 * presupuesto de la aplicacion, no arranca y no hay forma de avisar por
 * pantalla. Por eso el valor por defecto se queda dentro del presupuesto
 * normal de una app de Vita (~256 MB) en vez de estirarse hasta el maximo.
 * Se ajusta con -DAZAHAR_HEAP_MB=N al configurar.
 */
unsigned int _newlib_heap_size_user = AZAHAR_HEAP_MB * 1024u * 1024u;

/**
 * Tamano de pila de cada hilo creado con pthread.
 *
 * La pila de un hilo de Vita sale del presupuesto de memoria de la aplicacion,
 * no del heap de libc, y ese presupuesto va muy justo: al cargar un juego solo
 * quedan unos 25 MB. Si la reserva falla, pthread_create devuelve un error que
 * libstdc++ convierte en una excepcion, y como salta dentro de un hilo se lleva
 * el proceso por delante.
 *
 * 256 KB es de sobra: midiendo los volcados, el hilo que mas gastaba usaba
 * 9 KB. Lo que importa es que sea un valor conocido y contenido en vez del que
 * traiga la biblioteca por defecto.
 */
unsigned int _pthread_stack_default_user = 256 * 1024;
}

namespace {

/// Version del port, visible en el menu y en la pantalla de arranque. Se define
/// en vita_version.h, que es el unico sitio donde se escribe (el overlay del
/// juego usa la misma constante).
using VitaFrontend::kVersion;

/// Textos de los menus en el idioma elegido (0.3.3.9, ver vita_i18n.h).
namespace I18n = VitaFrontend::I18n;
const char* T(I18n::Str id) {
    return I18n::Tr(id);
}

constexpr char kUserDir[] = "ux0:/data/azahar/";
constexpr char kRomDir[] = "ux0:/data/azahar/roms";

constexpr unsigned int kColorText = 0xFFFFFFFF;
constexpr unsigned int kColorDim = 0xFF909090;
constexpr unsigned int kColorAccent = 0xFF4FC3F7; // ARGB -> naranja azahar en BGR
constexpr unsigned int kColorBackground = 0xFF201810;

using RomEntry = VitaFrontend::HomeGame;

bool HasRomExtension(const std::string& name) {
    static const char* kExtensions[] = {".3ds", ".3dsx", ".cci", ".cxi", ".app", ".cia", ".elf"};
    for (const char* ext : kExtensions) {
        const std::size_t ext_len = std::char_traits<char>::length(ext);
        if (name.size() <= ext_len) {
            continue;
        }
        if (std::equal(ext, ext + ext_len, name.end() - ext_len,
                       [](char a, char b) { return a == std::tolower(b); })) {
            return true;
        }
    }
    return false;
}

std::vector<RomEntry> ScanRoms() {
    std::vector<RomEntry> roms;

    const SceUID dir = sceIoDopen(kRomDir);
    if (dir < 0) {
        return roms;
    }

    SceIoDirent entry{};
    while (sceIoDread(dir, &entry) > 0) {
        if (SCE_S_ISDIR(entry.d_stat.st_mode)) {
            continue;
        }
        const std::string name = entry.d_name;
        if (!HasRomExtension(name)) {
            continue;
        }
        roms.push_back({name, std::string(kRomDir) + "/" + name});
    }
    sceIoDclose(dir);

    std::sort(roms.begin(), roms.end(),
              [](const RomEntry& a, const RomEntry& b) { return a.name < b.name; });
    return roms;
}

void CreateDirectories() {
    sceIoMkdir("ux0:/data", 0777);
    sceIoMkdir("ux0:/data/azahar", 0777);
    sceIoMkdir(kRomDir, 0777);
}

/// Ajustes adaptados a lo que la Vita puede dar.
void ConfigureSettings() {
    // Motor grafico por defecto: GXM. Presenta con el chip de la consola los
    // fotogramas que sigue rasterizando el camino de software; si el
    // presentador no puede arrancar (falta libshacccg.suprx, falla la
    // compilacion de los shaders, no hay memoria de GPU), degrada solo al
    // camino de vita2d sin tocar la emulacion. En el menu, SELECT alterna entre
    // GXM y el renderer de software entero para comparar.
    Settings::values.graphics_api = Settings::GraphicsAPI::GXM;

    // Sin JIT: dynarmic solo tiene backends de x86-64 y ARM64, y la Vita es
    // ARMv7. Se usa el interprete dyncom.
    Settings::values.use_cpu_jit = false;
    Settings::values.use_shader_jit = false;
    // Shader de vertices en la GPU (0.1.4.6): PicaCore le pregunta a
    // RasterizerGXM::AccelerateDrawBatch antes de ejecutar el interprete. Lo que
    // no se pueda acelerar sigue por la CPU lote a lote.
    Settings::values.use_hw_shader = true;
    Settings::values.use_disk_shader_cache = false;
    Settings::values.async_shader_compilation = false;

    // Modo New 3DS: se decide al compilar (-DAZAHAR_NEW_3DS=ON), no aqui.
    //
    // Dejo de ser "imposible" al activar el modo de memoria ampliada: la FCRAM
    // del New 3DS son 256 MB frente a 128, y el presupuesto pasa de ~256 a
    // ~365 MB. Lo que sigue haciendo falta es un heap de >= 288 MB, y eso ata el
    // arranque a que ATTRIBUTE2=12 funcione. El CMakeLists de esta carpeta tiene
    // la cuenta completa y las dos comprobaciones que impiden encenderlo a medias.
#ifdef AZAHAR_NEW_3DS
    Settings::values.is_new_3ds = true;
#else
    Settings::values.is_new_3ds = false;
#endif

    Settings::values.resolution_factor = 1;
    Settings::values.layout_option = Settings::LayoutOption::Default;

    // Sin 3D estereoscopico: la Vita tiene una sola pantalla y el frontend solo
    // dibuja el ojo izquierdo. Algunos juegos siguen dibujando el ojo derecho
    // aunque el deslizador 3D este a cero, y todo ese trabajo se tira.
    // RightEyeDisabler bloquea esos envios a la GPU emulada antes de que lleguen
    // a rasterizarse.
    Settings::values.disable_right_eye_render = true;

    // Sin limite de velocidad: el emulador no va a llegar al 100% de todas
    // formas, y limitarlo solo anadiria esperas.
    Settings::values.frame_limit = 0;

    Settings::values.audio_emulation = Settings::AudioEmulation::HLE;

    // Audio desactivado de momento. El sink de Vita levanta su propio hilo, y
    // cada hilo consume presupuesto de memoria de la aplicacion, que va al
    // limite. Emulando a pocos fotogramas por segundo el sonido no se entiende
    // igualmente, asi que primero que vaya la imagen. Para recuperarlo basta
    // con poner SinkType::Vita aqui.
    /**
     * SONIDO ENCENDIDO (0.1.4.9, lo pidio el usuario).
     *
     * Hasta aqui la salida era Null: el DSP se emulaba igual (el juego lo
     * necesita para avanzar) pero nadie escuchaba. VitaSink (vita_sink.cpp)
     * abre el puerto principal de audio a 48 kHz y un hilo propio que pide
     * bloques de ~21 ms y los entrega a sceAudioOutOutput, que marca el ritmo.
     *
     * CON EL ESTIRADO DE AUDIO. A la velocidad actual (un 5-10 % de la real)
     * el emulador produce una decima parte del sonido que el altavoz consume.
     * Con el estirado (SoundTouch, en el hilo de audio, no en el de emulacion)
     * se oye continuo pero muy lento y grave; sin el, se oye a su tono pero a
     * trozos, con silencios. SELECT + IZQUIERDA cambia entre los dos en
     * marcha (vita_window.cpp).
     */
    Settings::values.output_type = AudioCore::SinkType::Vita;
    Settings::values.enable_audio_stretching = true;

    Settings::values.use_virtual_sd = true;

    // Operaciones de fichero en el mismo hilo, no en paralelo.
    //
    // HLERequestContext::RunAsync usa std::async(std::launch::async, ...), que
    // levanta UN HILO NUEVO en cada llamada, y el servicio de ficheros lo usa
    // en cada operacion. Al cargar un juego se hacen cientos, y en la Vita la
    // pila de cada hilo sale del presupuesto del proceso, del que quedan unos
    // 25 MB: se agotan, std::async no puede crear mas y lanza system_error.
    // Como salta dentro de un hilo del emulador, nadie la captura y se lleva el
    // proceso por delante.
    //
    // Con esto RunAsync ejecuta en linea, sin crear hilos. Va mas lento, pero
    // en una consola que ya emula por interprete eso es lo de menos.
    Settings::values.deterministic_async_operations = true;
    Settings::values.async_fs_operations = false;

    // El registro vuelve a pasar por la cola del hilo de logging. En modo
    // sincrono cada linea es una escritura a la tarjeta de memoria, y Citra
    // registra constantemente mientras emula: eso frena el emulador hasta
    // hacerlo inservible. Los eventos criticos se anotan aparte en crash.txt,
    // que se escribe con la API del sistema y no depende de esta cola.
    Settings::values.instant_debug_log = false;

    // Service::Init consulta lle_modules.at(nombre) para cada uno de los 41
    // modulos de servicio del 3DS, y .at() lanza out_of_range si la clave no
    // existe. Los frontends de escritorio rellenan el mapa al leer su fichero
    // de configuracion; aqui no hay tal fichero, asi que hay que poblarlo a
    // mano o cargar un juego falla antes de empezar.
    //
    // Todos a false = emulacion por alto nivel (HLE). Usar LLE exigiria los
    // modulos de sistema volcados de una consola real.
    for (const auto& service_module : Service::service_module_map) {
        Settings::values.lle_modules.emplace(service_module.name, false);
    }

    // La Vita tiene camaras, pero no estan portadas. "blank" es la camara nula
    // que Azahar ya trae; sin ponerla, el nombre llega vacio y el servicio deja
    // un error por cada camara aunque acabe usando la misma nula.
    for (auto& name : Settings::values.camera_name) {
        name = "blank";
    }
    for (auto& config : Settings::values.camera_config) {
        config.clear();
    }
    Settings::values.camera_flip = {};
}

// ---------------------------------------------------------------------------
// Ajustes del usuario (0.1.5.1)
// ---------------------------------------------------------------------------

/**
 * VENTANA DE AJUSTES. Se abre manteniendo START dos segundos en la lista de
 * juegos (START a secas no hace nada en el menu, y exigir que se mantenga
 * evita abrirla sin querer). Tres ajustes:
 *
 *   - Volumen: Settings::values.volume, que DspInterface::OutputFrame aplica a
 *     cada bloque de salida (con curva cubica, como el deslizador de
 *     escritorio). Vale al momento, tambien en partida.
 *   - Idioma del sistema emulado: el bloque de idioma del config del 3DS, que
 *     es lo que leen los juegos para elegir idioma. Se aplica al cargar el
 *     siguiente juego (ver Service::CFG::g_vita_system_language).
 *   - Resolucion 1x o 0.5x: FrameSkip::half_resolution. En 0.5x el
 *     rasterizador por software sombrea un pixel de cada bloque de 2x2 y lo
 *     copia a los otros tres. Los lotes que dibuja la GPU de la Vita (GXM) no
 *     cambian: ya van a la resolucion nativa del 3DS y no son el cuello de
 *     botella. Vale al momento, y en partida lo cambia tambien L+R.
 *
 * Se guardan en ux0:/data/azahar/ajustes.txt al cerrar la ventana y se leen
 * al arrancar, para no tener que repetirlos cada vez.
 */
void WriteCrashLog(const char* title, const char* detail); // definida mas abajo

/// Hay una partida del piloto automatico en marcha (ver RunPilotIfAny).
std::atomic<bool> g_pilot_running{false};
/// Hay un juego cargando o en marcha (ver StartKeepAwake).
std::atomic<bool> g_in_game{false};

constexpr char kSettingsFile[] = "ux0:/data/azahar/ajustes.txt";
constexpr SceUInt64 kSettingsHoldUs = 2'000'000;
/// Mantener SELECT en el menu de inicio abre el selector de idioma (0.3.3.9).
constexpr SceUInt64 kLanguageHoldUs = 1'000'000;

struct LanguageOption {
    int value; ///< Service::CFG::SystemLanguage, o -1 = no tocar
    I18n::Str name;
};

constexpr LanguageOption kLanguages[] = {
    {-1, I18n::kSysLangAuto},
    {Service::CFG::LANGUAGE_ES, I18n::kSysLangEs},
    {Service::CFG::LANGUAGE_EN, I18n::kSysLangEn},
    {Service::CFG::LANGUAGE_FR, I18n::kSysLangFr},
    {Service::CFG::LANGUAGE_DE, I18n::kSysLangDe},
    {Service::CFG::LANGUAGE_IT, I18n::kSysLangIt},
    {Service::CFG::LANGUAGE_PT, I18n::kSysLangPt},
    {Service::CFG::LANGUAGE_NL, I18n::kSysLangNl},
    {Service::CFG::LANGUAGE_RU, I18n::kSysLangRu},
    {Service::CFG::LANGUAGE_JP, I18n::kSysLangJa},
    {Service::CFG::LANGUAGE_ZH, I18n::kSysLangZh},
    {Service::CFG::LANGUAGE_TW, I18n::kSysLangTw},
    {Service::CFG::LANGUAGE_KO, I18n::kSysLangKo},
};
constexpr int kLanguageCount = static_cast<int>(sizeof(kLanguages) / sizeof(kLanguages[0]));

struct UserSettings {
    int volume_percent = 100;
    int language = -1;
    bool half_resolution = false;
    /// 4.3: cache de registros del JIT. Encendida por defecto (0.1.5.2).
    bool jit_reg_cache = true;
    /// VFP en el JIT (datos 0.1.5.2, aritmetica 0.1.6.1, nativa 0.1.7.0). ON.
    bool jit_vfp_data = true;
    /// 4.5: diferir sceGxmFinish hasta WriteBack/Reload. ON por defecto.
    bool gxm_no_finish = true;
    /// 4.6: presentar desde color_buffer de GXM sin WriteBack+subida. ON.
    bool gxm_present_direct = true;
    /// 4.8: enlazado directo entre bloques en el codigo generado. ON.
    bool jit_direct_link = true;
    /// 0.1.6.0: saltos VS con escape a la GPU (ver g_allow_vs_escapes). ON
    /// desde 0.1.7.3: el shader que rompia el compilador ya no deja la GPU sin
    /// shaders (se recarga, se prueba otra variante y se aparta el codigo que
    /// lo rompe). Sin esto, el shader de piel de Rubi Omega va a la CPU.
    bool vs_escapes = true;
    /// 0.1.6.1: sombrear cada indice una vez por lote. ON por defecto.
    bool full_vertex_dedup = true;
    /// 0.1.7.1: sonido. Apagado = salida nula (se aplica al arrancar el juego).
    bool sound = true;
    /// 0.1.7.1: disposicion de las pantallas (VitaFrontend::g_screen_layout).
    int screen_layout = 0;
    /// 0.1.7.1: no volver a presentar una imagen que no ha cambiado.
    bool skip_repeated = true;
    /// Programas de vertices especializados por booleanos (0.1.7.4). OFF en
    /// 0.1.8.1 (agotaban la memoria del compilador al entrar al 3D de Zafiro
    /// Alfa), y la partida bajo a 2.5 FPS con los vertices de piel en la CPU.
    /// ON otra vez en 0.1.8.3, con el compilador descargado tras cada shader de
    /// vertices y sin compilar si no hay 85 MB libres (ver gxm_cg.cpp). Clave
    /// nueva: el ajustes.txt de 0.1.8.2 trae vs_especializar=0.
    bool vs_specialize = true;
    /// 0.1.8.6: copia de pantalla en la GPU (RasterizerGXM::transfer_on_gpu).
    bool gpu_screen_copy = true;
    /// 0.2.0.0: la GPU emulada en otro nucleo (VideoCore::GPU::async_enabled).
    /// OFF hasta probarlo en consola con varios juegos.
    bool gpu_thread = false;
    /// 0.2.0.3: sin esperar al refresco al presentar (VitaFrontend::g_unlimited_speed).
    bool unlimited_speed = false;
    /// Resolucion de la GPU en mitades: 1 = 0.5x, 2 = 1x, 4 = 2x
    /// (RasterizerGXM::resolution_scale).
    int gpu_scale = 2;
    /// 0.2.1.1: saltar el lote mientras compila su shader de vertices
    /// (RasterizerGXM::async_vs). ON.
    bool async_shaders = true;
    /// 0.2.1.7: bloques Thumb en el JIT (Core::ArmJit::thumb). Sin fila en el
    /// menu: solo ajustes.txt, para apagarlo si diera problemas.
    bool jit_thumb = true;
    /// 0.2.2.9: enlaces directos del JIT parcheados (Core::ArmJit::
    /// direct_link_patch). Solo ajustes.txt, como jit_thumb.
    bool jit_link_patch = true;
    /// 0.2.3.1: superbloques en el JIT (Core::ArmJit::superblocks). Solo
    /// ajustes.txt.
    bool jit_superblocks = true;
    /// 0.3.0.0: registros fijos en el JIT (Core::ArmJit::global_regs). Solo
    /// ajustes.txt.
    bool jit_global_regs = true;
    /// 0.3.0.1: registros VFP fijos en el JIT (Core::ArmJit::vfp_regs). Solo
    /// ajustes.txt.
    bool jit_vfp_regs = true;
    /// 0.3.1.2: anillo de vertices en memoria normal y no en CDRAM
    /// (Gxm::RasterizerGXM::vertex_ring_host). Solo ajustes.txt; vale al
    /// reservarlo, en el primer lote.
    bool vertex_ring_host = true;
    /// 0.3.1.5: la mezcla del DSP en otro nucleo (AudioCore::DspHle::async_mix).
    /// Solo ajustes.txt; vale desde el siguiente tick de audio.
    bool dsp_thread = true;
    /// 0.3.1.8: perfil de bloques calientes del JIT (Core::ArmJit::profile).
    /// Apagado: recompilaba todo dos veces por partida. Solo ajustes.txt.
    bool jit_profile = false;
    /// 0.3.1.8: cada cuantas presentaciones se vuelcan las superficies que
    /// nadie ha leido (Gxm::RasterizerGXM::present_writeback_every). 1 = como
    /// hasta 0.3.1.7. Solo ajustes.txt.
    ///
    /// DE 4 A 15 (0.4.0.6), con clave nueva ("volcado_cada2=") para que no la
    /// pise el 4 que ya esta guardado. Cada uno de esos volcados espera a que
    /// la GPU acabe el fotograma que se acaba de cerrar (hasta 18 ms de chip en
    /// Super Mario 3D Land) y copia 384 KB de CDRAM sin cache: "volcado" 2 ms
    /// por fotograma de media en 0.4.0.1, por una superficie que no lee nadie.
    /// Lo que la lea por la GPU (textura, copia, transferencia) la sigue
    /// volcando en ese momento; solo una lectura directa de la CPU la veria
    /// hasta un cuarto de segundo atrasada.
    int present_writeback_every = 15;
    /// 0.3.1.8: copias de textura de una superficie entera en la GPU
    /// (Gxm::RasterizerGXM::texture_copy_gpu). Apagado desde 0.3.1.9: en
    /// Pokemon Sol costaba 8-10 ms de espera a la GPU por fotograma. Solo
    /// ajustes.txt.
    bool texture_copy_gpu = false;
    /// 0.3.3.6: saltos indirectos del JIT por la tabla de enlaces
    /// (Core::ArmJit::link_table). Solo ajustes.txt.
    bool jit_link_table = true;
    /// 0.3.3.7: trampolines de los .cro como enlace directo con guarda
    /// (Core::ArmJit::veneers). Solo ajustes.txt.
    bool jit_veneers = true;
    /// 0.3.3.8: el base ya cargado por la instruccion anterior
    /// (Core::ArmJit::reuse_base). Solo ajustes.txt.
    bool jit_reuse_base = true;
    /// 0.3.4.2: lotes con W-buffer en la GPU (Gxm::RasterizerGXM::wbuffer_on_gpu).
    /// Solo ajustes.txt.
    bool wbuffer_gpu = true;
    /// 0.3.4.4: memo de lotes por valor (Gxm::RasterizerGXM::memo_by_value).
    /// Solo ajustes.txt.
    bool memo_by_value = true;
    /// 0.3.5.0: cache de disposiciones de vertices
    /// (Gxm::RasterizerGXM::vs_layout_cache). Solo ajustes.txt.
    bool vs_layout_cache = true;
    bool cache_pipeline = true;
    /// 0.4.0.0: uniformes con memcpy (Gxm::RasterizerGXM::direct_uniforms).
    /// Solo ajustes.txt.
    bool direct_uniforms = true;
    /// 0.4.0.4: cache de subidas de vertices
    /// (Gxm::RasterizerGXM::vertex_upload_cache). Solo ajustes.txt.
    bool vertex_upload_cache = true;
    /// 0.4.0.5: Y2R en otro nucleo (Service::Y2R::g_y2r_async). Solo ajustes.txt.
    /// Apagado desde 0.4.0.9: el video de Kirby iba peor (ver y2r_u.cpp).
    bool y2r_async = false;
    /// 0.4.0.6: la espera de presentar por marcas
    /// (Gxm::RasterizerGXM::present_marks_on). Solo ajustes.txt.
    bool present_marks = true;
    /// 0.4.0.6: solo los uniformes que lee el programa de vertices
    /// (Gxm::RasterizerGXM::exact_vs_uniforms). Solo ajustes.txt.
    bool exact_vs_uniforms = true;
    /// 0.4.0.8: los vertices de la cache de subidas sin comparar si ninguna
    /// de sus paginas ha avisado (Common::WriteEpochs). Solo ajustes.txt.
    bool vertex_epochs = true;
    /// 0.4.1.1: shaders de vertices a O2 (Gxm::g_vs_o2). Solo ajustes.txt.
    bool vs_o2 = false;
    /// 0.4.1.9: fragmentos con fastprecision y combinador en half. Solo ajustes.txt.
    bool fs_fast = false;
    bool fs_half = false;
    bool jit_nativo = false;
    /// 0.3.9.8: el motor nativo tambien en el nucleo 1 (ver
    /// Core::Native::both_cores). Experimental, solo ajustes.txt.
    bool native_both_cores = false;
    /// 0.3.4.9: tamano maximo de un shader de vertices que se compila en partida,
    /// en KB (Gxm::RasterizerGXM::vs_compile_limit_kb). Solo ajustes.txt.
    int vs_limit_kb = 23;
    /// 0.3.4.6: no repetir el estado de GXM igual entre lotes
    /// (Gxm::RasterizerGXM::state_shadow_enabled). Solo ajustes.txt.
    bool state_shadow = true;
    /// 0.3.3.9: idioma de los menus (VitaFrontend::I18n::Language). Ingles por
    /// defecto; se elige manteniendo SELECT en el menu de inicio.
    int menu_language = I18n::kEnglish;
};
UserSettings g_user;

constexpr I18n::Str kLayoutNames[VitaFrontend::kScreenLayoutCount] = {
    I18n::kLayout0, I18n::kLayout1, I18n::kLayout2, I18n::kLayout3, I18n::kLayout4};

int LanguageIndex(int value) {
    for (int i = 0; i < kLanguageCount; i++) {
        if (kLanguages[i].value == value) {
            return i;
        }
    }
    return 0;
}

void ApplyUserSettings() {
    Settings::values.volume = static_cast<float>(g_user.volume_percent) / 100.0f;
    I18n::SetLanguage(g_user.menu_language);
    Service::CFG::g_vita_system_language = g_user.language;
    SwRenderer::FrameSkip::half_resolution.store(g_user.half_resolution,
                                                 std::memory_order_relaxed);
    // 4.3 (0.1.5.2): al cambiar el interruptor hay que tirar los bloques ya
    // compilados, o los viejos seguirian con el mapa de cache del momento de
    // compilarlos y el interruptor no apagaria nada.
    const u32 want_cache = g_user.jit_reg_cache ? 1u : 0u;
    if (Core::ArmJit::reg_cache.exchange(want_cache, std::memory_order_relaxed) != want_cache) {
        Core::ArmJit::Reset();
    }
    // 4.4 (0.1.5.2): igual con VFP datos: recompilar, o los bloques viejos
    // seguirian rechazando o aceptando VFP segun el momento de compilarlos.
    const u32 want_vfp = g_user.jit_vfp_data ? 1u : 0u;
    if (Core::ArmJit::vfp_data.exchange(want_vfp, std::memory_order_relaxed) != want_vfp) {
        Core::ArmJit::Reset();
    }
    // 4.5 (0.1.5.2): no hace falta recompilar nada; es solo cuando se espera.
    Gxm::RasterizerGXM::no_finish_wait.store(g_user.gxm_no_finish ? 1u : 0u,
                                             std::memory_order_relaxed);
    // 4.6 (0.1.5.2): solo cambia de donde lee la textura el presentador.
    Gxm::RasterizerGXM::present_direct.store(g_user.gxm_present_direct ? 1u : 0u,
                                             std::memory_order_relaxed);
    // 4.8 (0.1.5.2): al cambiar hay que tirar los bloques: los viejos llevan
    // (o no) el enlace ya parcheado en su codigo.
    const u32 want_link = g_user.jit_direct_link ? 1u : 0u;
    if (Core::ArmJit::direct_link.exchange(want_link, std::memory_order_relaxed) != want_link) {
        Core::ArmJit::Reset();
    }
    const u32 want_thumb = g_user.jit_thumb ? 1u : 0u;
    if (Core::ArmJit::thumb.exchange(want_thumb, std::memory_order_relaxed) != want_thumb) {
        Core::ArmJit::Reset();
    }
    const u32 want_patch = g_user.jit_link_patch ? 1u : 0u;
    if (Core::ArmJit::direct_link_patch.exchange(want_patch, std::memory_order_relaxed) !=
        want_patch) {
        Core::ArmJit::Reset();
    }
    const u32 want_super = g_user.jit_superblocks ? 1u : 0u;
    if (Core::ArmJit::superblocks.exchange(want_super, std::memory_order_relaxed) != want_super) {
        Core::ArmJit::Reset();
    }
    const u32 want_global = g_user.jit_global_regs ? 1u : 0u;
    if (Core::ArmJit::global_regs.exchange(want_global, std::memory_order_relaxed) !=
        want_global) {
        Core::ArmJit::Reset();
    }
    const u32 want_vfp_regs = g_user.jit_vfp_regs ? 1u : 0u;
    if (Core::ArmJit::vfp_regs.exchange(want_vfp_regs, std::memory_order_relaxed) !=
        want_vfp_regs) {
        Core::ArmJit::Reset();
    }
    const u32 want_table = g_user.jit_link_table ? 1u : 0u;
    if (Core::ArmJit::link_table.exchange(want_table, std::memory_order_relaxed) != want_table) {
        Core::ArmJit::Reset();
    }
    const u32 want_veneers = g_user.jit_veneers ? 1u : 0u;
    if (Core::ArmJit::veneers.exchange(want_veneers, std::memory_order_relaxed) != want_veneers) {
        Core::ArmJit::Reset();
    }
    const u32 want_reuse = g_user.jit_reuse_base ? 1u : 0u;
    if (Core::ArmJit::reuse_base.exchange(want_reuse, std::memory_order_relaxed) != want_reuse) {
        Core::ArmJit::Reset();
    }
    // 0.1.6.0: solo cambia que shaders se traducen a partir de ahora.
    Pica::Shader::Generator::GXM::g_allow_vs_escapes.store(g_user.vs_escapes,
                                                            std::memory_order_relaxed);
    Pica::g_full_vertex_dedup.store(g_user.full_vertex_dedup, std::memory_order_relaxed);
    /**
     * Sonido (0.1.7.1). Apagado: salida nula y sin estirado. El DSP del 3DS se
     * sigue emulando (los juegos esperan a que termine sus bloques), pero se
     * ahorra el estirado de SoundTouch y el envio a la Vita. Se aplica al
     * arrancar el siguiente juego, que es cuando se crea la salida de audio.
     */
    Settings::values.output_type =
        g_user.sound ? AudioCore::SinkType::Vita : AudioCore::SinkType::Null;
    Settings::values.enable_audio_stretching = g_user.sound;
    VitaFrontend::g_screen_layout.store(
        std::clamp(g_user.screen_layout, 0, VitaFrontend::kScreenLayoutCount - 1),
        std::memory_order_relaxed);
    VitaFrontend::g_skip_repeated_frames.store(g_user.skip_repeated, std::memory_order_relaxed);
    Gxm::RasterizerGXM::specialize_vs.store(g_user.vs_specialize ? 1u : 0u,
                                            std::memory_order_relaxed);
    Gxm::RasterizerGXM::transfer_on_gpu.store(g_user.gpu_screen_copy ? 1u : 0u,
                                              std::memory_order_relaxed);
    VideoCore::GPU::async_enabled.store(g_user.gpu_thread, std::memory_order_relaxed);
    VitaFrontend::g_unlimited_speed.store(g_user.unlimited_speed, std::memory_order_relaxed);
    Gxm::RasterizerGXM::resolution_scale.store(static_cast<u32>(g_user.gpu_scale),
                                               std::memory_order_relaxed);
    Gxm::RasterizerGXM::async_vs.store(g_user.async_shaders ? 1u : 0u, std::memory_order_relaxed);
    Gxm::RasterizerGXM::vertex_ring_host.store(g_user.vertex_ring_host ? 1u : 0u,
                                               std::memory_order_relaxed);
    AudioCore::DspHle::async_mix.store(g_user.dsp_thread, std::memory_order_relaxed);
    Core::ArmJit::profile.store(g_user.jit_profile ? 1u : 0u, std::memory_order_relaxed);
    Gxm::RasterizerGXM::present_writeback_every.store(
        static_cast<u32>(g_user.present_writeback_every), std::memory_order_relaxed);
    Gxm::RasterizerGXM::texture_copy_gpu.store(g_user.texture_copy_gpu ? 1u : 0u,
                                               std::memory_order_relaxed);
    Gxm::RasterizerGXM::wbuffer_on_gpu.store(g_user.wbuffer_gpu ? 1u : 0u,
                                             std::memory_order_relaxed);
    Gxm::RasterizerGXM::memo_by_value.store(g_user.memo_by_value ? 1u : 0u,
                                            std::memory_order_relaxed);
    Gxm::RasterizerGXM::vs_layout_cache.store(g_user.vs_layout_cache ? 1u : 0u,
                                              std::memory_order_relaxed);
    Gxm::RasterizerGXM::cache_pipeline.store(g_user.cache_pipeline ? 1u : 0u,
                                             std::memory_order_relaxed);
    Gxm::RasterizerGXM::direct_uniforms.store(g_user.direct_uniforms ? 1u : 0u,
                                              std::memory_order_relaxed);
    Gxm::RasterizerGXM::vertex_upload_cache.store(g_user.vertex_upload_cache ? 1u : 0u,
                                                  std::memory_order_relaxed);
    Service::Y2R::g_y2r_async.store(g_user.y2r_async, std::memory_order_relaxed);
    Gxm::RasterizerGXM::present_marks_on.store(g_user.present_marks ? 1u : 0u,
                                               std::memory_order_relaxed);
    Gxm::RasterizerGXM::exact_vs_uniforms.store(g_user.exact_vs_uniforms ? 1u : 0u,
                                                std::memory_order_relaxed);
    Common::WriteEpochs::enabled.store(g_user.vertex_epochs, std::memory_order_relaxed);
    Gxm::g_vs_o2.store(g_user.vs_o2, std::memory_order_relaxed);
    Gxm::g_fs_fast.store(g_user.fs_fast, std::memory_order_relaxed);
    Pica::Shader::Generator::GXM::g_fs_half_tev.store(g_user.fs_half,
                                                      std::memory_order_relaxed);
    Core::Native::enabled = g_user.jit_nativo;
    Core::Native::both_cores = g_user.native_both_cores;
    if (g_user.jit_nativo) {
        Common::VitaNote("nativo", "jit_nativo=1: se intentara ejecutar codigo del "
                                   "juego en la CPU de la consola");
    }
    Gxm::RasterizerGXM::vs_compile_limit_kb.store(static_cast<u32>(g_user.vs_limit_kb),
                                                  std::memory_order_relaxed);
    Gxm::RasterizerGXM::state_shadow_enabled.store(g_user.state_shadow ? 1u : 0u,
                                                   std::memory_order_relaxed);
}

/// Lee ajustes.txt. Si no existe o una linea no se entiende, se queda el valor
/// por defecto de ese ajuste: un fichero roto no puede impedir arrancar.
void LoadUserSettings() {
    const SceUID fd = sceIoOpen(kSettingsFile, SCE_O_RDONLY, 0);
    if (fd < 0) {
        return;
    }
    char buffer[2048] = {};
    const int read = sceIoRead(fd, buffer, sizeof(buffer) - 1);
    sceIoClose(fd);
    if (read <= 0) {
        return;
    }
    buffer[read] = '\0';
    const auto read_int = [&buffer](const char* key, int& out) {
        const char* p = std::strstr(buffer, key);
        int value = 0;
        if (p != nullptr && std::sscanf(p + std::strlen(key), "%d", &value) == 1) {
            out = value;
            return true;
        }
        return false;
    };
    const auto read_bool = [&read_int](const char* key, bool& out) {
        int value = 0;
        if (read_int(key, value)) {
            out = value != 0;
        }
    };
    int value = 0;
    if (read_int("volumen=", value)) {
        g_user.volume_percent = std::clamp(value, 0, 100);
    }
    if (read_int("idioma=", value)) {
        g_user.language = kLanguages[LanguageIndex(value)].value;
    }
    read_bool("resolucion_media=", g_user.half_resolution);
    read_bool("jit_cache_reg=", g_user.jit_reg_cache);
    read_bool("jit_vfp_datos=", g_user.jit_vfp_data);
    read_bool("gxm_sin_espera=", g_user.gxm_no_finish);
    read_bool("gxm_present_dir=", g_user.gxm_present_direct);
    read_bool("jit_enlace_dir=", g_user.jit_direct_link);
    // Clave nueva en 0.1.7.3: un ajustes.txt viejo trae vs_saltos=0 de cuando
    // el valor por defecto era OFF, y eso dejaria apagada la mayor ganancia.
    read_bool("vs_saltos2=", g_user.vs_escapes);
    read_bool("cache_vertices=", g_user.full_vertex_dedup);
    read_bool("sonido=", g_user.sound);
    if (read_int("pantallas=", value)) {
        g_user.screen_layout = std::clamp(value, 0, VitaFrontend::kScreenLayoutCount - 1);
    }
    read_bool("omitir_repetidas=", g_user.skip_repeated);
    read_bool("vs_especializar2=", g_user.vs_specialize);
    read_bool("copia_gpu=", g_user.gpu_screen_copy);
    read_bool("gpu_hilo=", g_user.gpu_thread);
    read_bool("sin_limite=", g_user.unlimited_speed);
    // Clave nueva en 0.2.0.4, en mitades (la de 0.2.0.3 iba en enteros).
    if (read_int("resolucion_gpu2=", value)) {
        g_user.gpu_scale = value == 1 || value == 4 ? value : 2;
    }
    read_bool("shaders_asinc=", g_user.async_shaders);
    read_bool("jit_thumb=", g_user.jit_thumb);
    read_bool("jit_parche=", g_user.jit_link_patch);
    read_bool("jit_superbloques=", g_user.jit_superblocks);
    read_bool("jit_regs_fijos=", g_user.jit_global_regs);
    read_bool("jit_vfp_fijos=", g_user.jit_vfp_regs);
    read_bool("vertices_ram=", g_user.vertex_ring_host);
    read_bool("dsp_hilo=", g_user.dsp_thread);
    read_bool("jit_perfil=", g_user.jit_profile);
    if (read_int("volcado_cada2=", value)) {
        g_user.present_writeback_every = std::clamp(value, 1, 60);
    }
    read_bool("copia_textura_gpu=", g_user.texture_copy_gpu);
    read_bool("jit_tabla_enlaces=", g_user.jit_link_table);
    read_bool("jit_saltos_cro=", g_user.jit_veneers);
    read_bool("jit_reusar_base=", g_user.jit_reuse_base);
    read_bool("wbuffer_gpu=", g_user.wbuffer_gpu);
    read_bool("memo_valor=", g_user.memo_by_value);
    read_bool("vs_cache_disp=", g_user.vs_layout_cache);
    read_bool("cache_pipeline=", g_user.cache_pipeline);
    read_bool("uniformes_directos=", g_user.direct_uniforms);
    read_bool("cache_subidas=", g_user.vertex_upload_cache);
    read_bool("y2r_hilo=", g_user.y2r_async);
    read_bool("valla_exacta=", g_user.present_marks);
    read_bool("uniformes_justos=", g_user.exact_vs_uniforms);
    read_bool("vertices_por_vaciados=", g_user.vertex_epochs);
    read_bool("vs_o2=", g_user.vs_o2);
    read_bool("fs_rapidos=", g_user.fs_fast);
    read_bool("fs_media=", g_user.fs_half);
    read_bool("jit_nativo=", g_user.jit_nativo);
    read_bool("nativo_dos_nucleos=", g_user.native_both_cores);
    if (read_int("vs_tope_kb=", value)) {
        g_user.vs_limit_kb = std::clamp(value, 16, 40);
    }
    read_bool("estado_sombra=", g_user.state_shadow);
    if (read_int("idioma_menu=", value)) {
        g_user.menu_language = std::clamp(value, 0, I18n::kLanguageCount - 1);
    }
}

/// El contenido de ajustes.txt, una clave por linea.
int FormatUserSettings(char* buffer, std::size_t size) {
    return std::snprintf(
        buffer, size,
        "volumen=%d\nidioma=%d\nresolucion_media=%d\njit_cache_reg=%d\n"
        "jit_vfp_datos=%d\ngxm_sin_espera=%d\ngxm_present_dir=%d\n"
        "jit_enlace_dir=%d\nvs_saltos2=%d\ncache_vertices=%d\nsonido=%d\npantallas=%d\n"
        "omitir_repetidas=%d\nvs_especializar2=%d\ncopia_gpu=%d\ngpu_hilo=%d\nsin_limite=%d\n"
        "resolucion_gpu2=%d\nshaders_asinc=%d\njit_thumb=%d\njit_parche=%d\n"
        "jit_superbloques=%d\njit_regs_fijos=%d\njit_vfp_fijos=%d\nvertices_ram=%d\n"
        "dsp_hilo=%d\njit_perfil=%d\nvolcado_cada2=%d\ncopia_textura_gpu=%d\n"
        "jit_tabla_enlaces=%d\njit_saltos_cro=%d\njit_reusar_base=%d\nidioma_menu=%d\n"
        "wbuffer_gpu=%d\nmemo_valor=%d\nestado_sombra=%d\nvs_tope_kb=%d\nvs_cache_disp=%d\ncache_pipeline=%d\njit_nativo=%d\n"
        "nativo_dos_nucleos=%d\nuniformes_directos=%d\ncache_subidas=%d\ny2r_hilo=%d\nvalla_exacta=%d\nuniformes_justos=%d\nvertices_por_vaciados=%d\nvs_o2=%d\n",
        g_user.volume_percent, g_user.language, g_user.half_resolution ? 1 : 0,
        g_user.jit_reg_cache ? 1 : 0, g_user.jit_vfp_data ? 1 : 0, g_user.gxm_no_finish ? 1 : 0,
        g_user.gxm_present_direct ? 1 : 0, g_user.jit_direct_link ? 1 : 0,
        g_user.vs_escapes ? 1 : 0, g_user.full_vertex_dedup ? 1 : 0, g_user.sound ? 1 : 0,
        g_user.screen_layout, g_user.skip_repeated ? 1 : 0, g_user.vs_specialize ? 1 : 0,
        g_user.gpu_screen_copy ? 1 : 0, g_user.gpu_thread ? 1 : 0,
        g_user.unlimited_speed ? 1 : 0, g_user.gpu_scale, g_user.async_shaders ? 1 : 0,
        g_user.jit_thumb ? 1 : 0, g_user.jit_link_patch ? 1 : 0,
        g_user.jit_superblocks ? 1 : 0, g_user.jit_global_regs ? 1 : 0,
        g_user.jit_vfp_regs ? 1 : 0, g_user.vertex_ring_host ? 1 : 0,
        g_user.dsp_thread ? 1 : 0, g_user.jit_profile ? 1 : 0, g_user.present_writeback_every,
        g_user.texture_copy_gpu ? 1 : 0, g_user.jit_link_table ? 1 : 0,
        g_user.jit_veneers ? 1 : 0, g_user.jit_reuse_base ? 1 : 0, g_user.menu_language,
        g_user.wbuffer_gpu ? 1 : 0, g_user.memo_by_value ? 1 : 0, g_user.state_shadow ? 1 : 0,
        g_user.vs_limit_kb, g_user.vs_layout_cache ? 1 : 0, g_user.cache_pipeline ? 1 : 0, g_user.jit_nativo ? 1 : 0,
        g_user.native_both_cores ? 1 : 0, g_user.direct_uniforms ? 1 : 0,
        g_user.vertex_upload_cache ? 1 : 0, g_user.y2r_async ? 1 : 0,
        g_user.present_marks ? 1 : 0, g_user.exact_vs_uniforms ? 1 : 0,
        g_user.vertex_epochs ? 1 : 0, g_user.vs_o2 ? 1 : 0);
}

/// Los ajustes con los que se juega, a crash.txt en una linea (0.1.8.7): sin
/// esto no se sabe si una partida lenta llevaba algo apagado en ajustes.txt.
void NoteUserSettings() {
    char buffer[2048];
    const int length = FormatUserSettings(buffer, sizeof(buffer));
    for (int i = 0; i < length && i < static_cast<int>(sizeof(buffer)) - 1; i++) {
        if (buffer[i] == '\n') {
            buffer[i] = ' ';
        }
    }
    WriteCrashLog("ajustes", buffer);
}

void SaveUserSettings() {
    char buffer[2048];
    const int length = FormatUserSettings(buffer, sizeof(buffer));
    const SceUID fd = sceIoOpen(kSettingsFile, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) {
        WriteCrashLog("ajustes", "no se pudo guardar ajustes.txt");
        return;
    }
    sceIoWrite(fd, buffer, static_cast<SceSize>(length));
    sceIoClose(fd);
}

constexpr int kSettingsRows = 20;

void DrawSettings(vita2d_pgf* font, int row) {
    constexpr int kFirstY = 100;
    // 16 desde la fila 19 (0.2.1.1): con mas la ultima pisaba la ayuda.
    constexpr int kRowStep = 16;
    constexpr int kValueX = 330;

    vita2d_start_drawing();
    vita2d_clear_screen();
    vita2d_pgf_draw_text(font, 40, 45, kColorAccent, 1.4f, T(I18n::kSettingsTitle));
    I18n::DrawTextFit(font, 40, 72, kColorDim, 0.85f, T(I18n::kSettingsSubtitle), 880);

    char volume_text[16];
    std::snprintf(volume_text, sizeof(volume_text), "%d %%", g_user.volume_percent);
    const bool gxm = Settings::values.graphics_api.GetValue() == Settings::GraphicsAPI::GXM;
    const char* values[kSettingsRows] = {
        volume_text,
        T(kLanguages[LanguageIndex(g_user.language)].name),
        g_user.half_resolution ? "0.5x" : "1x",
        g_user.jit_reg_cache ? "ON" : "OFF",
        g_user.jit_vfp_data ? "ON" : "OFF",
        g_user.gxm_no_finish ? "ON" : "OFF",
        g_user.gxm_present_direct ? "ON" : "OFF",
        g_user.jit_direct_link ? "ON" : "OFF",
        g_user.vs_escapes ? "ON" : "OFF",
        g_user.full_vertex_dedup ? "ON" : "OFF",
        g_user.sound ? "ON" : "OFF",
        T(kLayoutNames[std::clamp(g_user.screen_layout, 0, VitaFrontend::kScreenLayoutCount - 1)]),
        g_user.skip_repeated ? "ON" : "OFF",
        g_user.vs_specialize ? "ON" : "OFF",
        g_user.gpu_screen_copy ? "ON" : "OFF",
        g_user.gpu_thread ? "ON" : "OFF",
        g_user.unlimited_speed ? T(I18n::kValUnlimited) : T(I18n::kValRefresh),
        g_user.gpu_scale == 4 ? T(I18n::kVal2x) : g_user.gpu_scale == 1 ? "0.5x" : "1x",
        g_user.async_shaders ? "ON" : "OFF",
        gxm ? "GXM" : T(I18n::kValSoftware)};

    for (int i = 0; i < kSettingsRows; i++) {
        const int y = kFirstY + i * kRowStep;
        const bool selected = i == row;
        if (selected) {
            vita2d_draw_rectangle(30, static_cast<float>(y - 18), 900, 23, 0xFF402810);
        }
        const unsigned int color = selected ? kColorAccent : kColorText;
        I18n::DrawTextFit(font, 44, y, color, 0.95f, T(I18n::Offset(I18n::kSetLabel0, i)),
                          kValueX - 54);
        vita2d_pgf_draw_textf(font, kValueX, y, color, 0.95f, selected ? "<  %s  >" : "   %s",
                              values[i]);
    }

    // Barra del volumen, a la derecha de su fila.
    const float bar_width = 250.0f;
    vita2d_draw_rectangle(kValueX + 200, kFirstY - 8, bar_width, 6, 0xFF404040);
    vita2d_draw_rectangle(kValueX + 200, kFirstY - 8,
                          bar_width * static_cast<float>(g_user.volume_percent) / 100.0f, 6,
                          kColorAccent);

    /**
     * Que hace cada ajuste: que ganas (FPS, mas o menos) y que pierdes. Las
     * cifras son estimaciones de lo medido en Pokemon Rubi Omega; en otros
     * juegos cambian. Los textos estan en vita_strings.inc (kSetHelp<fila>).
     */
    const I18n::Str help = I18n::Offset(I18n::kSetHelp0a, row * 3);
    I18n::DrawTextFit(font, 40, 428, kColorDim, 0.9f, T(help), 880);
    I18n::DrawTextFit(font, 40, 450, kColorAccent, 0.9f, T(I18n::Offset(help, 1)), 880);
    I18n::DrawTextFit(font, 40, 472, kColorDim, 0.9f, T(I18n::Offset(help, 2)), 880);

    I18n::DrawTextFit(font, 40, 504, kColorDim, 0.9f, T(I18n::kSettingsFooter), 880);
    vita2d_pgf_draw_text(font, 40, 528, kColorDim, 0.85f, kVersion);
    vita2d_end_drawing();
    vita2d_swap_buffers();
}

/// La ventana de ajustes. Vuelve al cerrarla, con los ajustes aplicados y
/// guardados.
void RunSettingsMenu(vita2d_pgf* font) {
    // L+R en partida cambia la resolucion sin pasar por aqui: partir del valor
    // real, no del ultimo que se guardo.
    g_user.half_resolution =
        SwRenderer::FrameSkip::half_resolution.load(std::memory_order_relaxed);
    g_user.jit_reg_cache = Core::ArmJit::reg_cache.load(std::memory_order_relaxed) != 0;
    g_user.jit_vfp_data = Core::ArmJit::vfp_data.load(std::memory_order_relaxed) != 0;
    g_user.gxm_no_finish = Gxm::RasterizerGXM::no_finish_wait.load(std::memory_order_relaxed) != 0;
    g_user.gxm_present_direct =
        Gxm::RasterizerGXM::present_direct.load(std::memory_order_relaxed) != 0;
    g_user.jit_direct_link = Core::ArmJit::direct_link.load(std::memory_order_relaxed) != 0;
    g_user.vs_escapes =
        Pica::Shader::Generator::GXM::g_allow_vs_escapes.load(std::memory_order_relaxed);
    g_user.full_vertex_dedup = Pica::g_full_vertex_dedup.load(std::memory_order_relaxed);
    g_user.vs_specialize =
        Gxm::RasterizerGXM::specialize_vs.load(std::memory_order_relaxed) != 0;
    g_user.gpu_screen_copy =
        Gxm::RasterizerGXM::transfer_on_gpu.load(std::memory_order_relaxed) != 0;
    g_user.gpu_thread = VideoCore::GPU::async_enabled.load(std::memory_order_relaxed);
    g_user.unlimited_speed = VitaFrontend::g_unlimited_speed.load(std::memory_order_relaxed);
    g_user.gpu_scale =
        static_cast<int>(Gxm::RasterizerGXM::resolution_scale.load(std::memory_order_relaxed));
    g_user.async_shaders = Gxm::RasterizerGXM::async_vs.load(std::memory_order_relaxed) != 0;

    int row = 0;
    // Todo lo que ya este pulsado al entrar (el START que la abrio) cuenta como
    // mantenido, no como pulsacion nueva: si no, se cerraria en el acto.
    unsigned int previous_buttons = ~0u;
    while (true) {
        SceCtrlData pad{};
        sceCtrlPeekBufferPositive(0, &pad, 1);
        const unsigned int pressed = pad.buttons & ~previous_buttons;
        previous_buttons = pad.buttons;

        if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_START)) {
            break;
        }
        if (pressed & SCE_CTRL_DOWN) {
            row = (row + 1) % kSettingsRows;
        }
        if (pressed & SCE_CTRL_UP) {
            row = (row + kSettingsRows - 1) % kSettingsRows;
        }
        const int step = (pressed & SCE_CTRL_RIGHT) ? 1 : (pressed & SCE_CTRL_LEFT) ? -1 : 0;
        if (step != 0) {
            switch (row) {
            case 0:
                g_user.volume_percent = std::clamp(g_user.volume_percent + step * 10, 0, 100);
                break;
            case 1: {
                const int index =
                    (LanguageIndex(g_user.language) + step + kLanguageCount) % kLanguageCount;
                g_user.language = kLanguages[index].value;
                break;
            }
            case 2:
                g_user.half_resolution = !g_user.half_resolution;
                break;
            case 3:
                g_user.jit_reg_cache = !g_user.jit_reg_cache;
                break;
            case 4:
                g_user.jit_vfp_data = !g_user.jit_vfp_data;
                break;
            case 5:
                g_user.gxm_no_finish = !g_user.gxm_no_finish;
                break;
            case 6:
                g_user.gxm_present_direct = !g_user.gxm_present_direct;
                break;
            case 7:
                g_user.jit_direct_link = !g_user.jit_direct_link;
                break;
            case 8:
                g_user.vs_escapes = !g_user.vs_escapes;
                break;
            case 9:
                g_user.full_vertex_dedup = !g_user.full_vertex_dedup;
                break;
            case 10:
                g_user.sound = !g_user.sound;
                break;
            case 11:
                g_user.screen_layout =
                    (g_user.screen_layout + step + VitaFrontend::kScreenLayoutCount) %
                    VitaFrontend::kScreenLayoutCount;
                break;
            case 12:
                g_user.skip_repeated = !g_user.skip_repeated;
                break;
            case 13:
                g_user.vs_specialize = !g_user.vs_specialize;
                break;
            case 14:
                g_user.gpu_screen_copy = !g_user.gpu_screen_copy;
                break;
            case 15:
                g_user.gpu_thread = !g_user.gpu_thread;
                break;
            case 16:
                g_user.unlimited_speed = !g_user.unlimited_speed;
                break;
            case 18:
                g_user.async_shaders = !g_user.async_shaders;
                break;
            case 19: {
                // Motor grafico del siguiente juego (antes, SELECT en el menu de
                // inicio). El renderer se construye en System::Load.
                const bool gxm =
                    Settings::values.graphics_api.GetValue() == Settings::GraphicsAPI::GXM;
                Settings::values.graphics_api =
                    gxm ? Settings::GraphicsAPI::Software : Settings::GraphicsAPI::GXM;
                break;
            }
            default:
                // 0.5x -> 1x -> 2x con DERECHA, al reves con IZQUIERDA.
                g_user.gpu_scale = step > 0 ? (g_user.gpu_scale == 1 ? 2 : g_user.gpu_scale == 2 ? 4 : 1)
                                            : (g_user.gpu_scale == 4 ? 2 : g_user.gpu_scale == 2 ? 1 : 4);
                break;
            }
            ApplyUserSettings();
        }
        DrawSettings(font, row);
    }
    ApplyUserSettings();
    SaveUserSettings();
    NoteUserSettings();
}

/// "Cargando shaders" con barra de progreso, antes de empezar el juego (0.2.0.4).
void DrawShaderLoading(vita2d_pgf* font, u32 done, u32 total) {
    vita2d_start_drawing();
    vita2d_clear_screen();
    vita2d_pgf_draw_text(font, 40, 200, kColorAccent, 1.3f, T(I18n::kLoadingShaders));
    constexpr float kBarX = 40.0f;
    constexpr float kBarY = 240.0f;
    constexpr float kBarWidth = 880.0f;
    vita2d_draw_rectangle(kBarX, kBarY, kBarWidth, 18.0f, 0xFF404040);
    if (total > 0) {
        vita2d_draw_rectangle(kBarX, kBarY,
                              kBarWidth * static_cast<float>(done) / static_cast<float>(total),
                              18.0f, kColorAccent);
        vita2d_pgf_draw_textf(font, 40, 300, kColorText, 1.0f, T(I18n::kShaderCount), done, total);
    } else {
        I18n::DrawTextFit(font, 40, 300, kColorDim, 1.0f, T(I18n::kNoShaders1), 880);
        I18n::DrawTextFit(font, 40, 330, kColorDim, 1.0f, T(I18n::kNoShaders2), 880);
    }
    vita2d_end_drawing();
    vita2d_swap_buffers();
}

void DrawMessage(vita2d_pgf* font, const char* line1, const char* line2) {
    vita2d_start_drawing();
    vita2d_clear_screen();
    vita2d_pgf_draw_text(font, 40, 240, kColorText, 1.2f, line1);
    if (line2 != nullptr) {
        vita2d_pgf_draw_text(font, 40, 280, kColorDim, 1.0f, line2);
    }
    vita2d_end_drawing();
    vita2d_swap_buffers();
}

const char* LoadErrorMessage(Core::System::ResultStatus status) {
    switch (status) {
    case Core::System::ResultStatus::ErrorGetLoader:
        return T(I18n::kLoadErrNoLoader);
    case Core::System::ResultStatus::ErrorLoader:
        return T(I18n::kLoadErrLoader);
    case Core::System::ResultStatus::ErrorLoader_ErrorEncrypted:
        return T(I18n::kLoadErrEncrypted);
    case Core::System::ResultStatus::ErrorLoader_ErrorInvalidFormat:
        return T(I18n::kLoadErrFormat);
    case Core::System::ResultStatus::ErrorLoader_ErrorGbaTitle:
        return T(I18n::kLoadErrGba);
    case Core::System::ResultStatus::ErrorSystemFiles:
        return T(I18n::kLoadErrSystemFiles);
    case Core::System::ResultStatus::ErrorNotInitialized:
        return T(I18n::kLoadErrNotInitialized);
    case Core::System::ResultStatus::ErrorSystemMode:
        // OJO: este no significa lo que dice. Es el cajon de sastre de
        // Core::System::Load: LoadKernelMemoryMode solo falla si no consigue
        // ABRIR Y LEER el ROM, y todo error que no sea cifrado / formato
        // invalido / titulo de GBA acaba aqui. El generico ResultStatus::Error
        // se devuelve en mas de quince sitios distintos de ncch_container.cpp.
        //
        // El texto lo dice ahora, y ademas la pantalla de error enseña el
        // tamaño del fichero: un ROM de mas de 2 GB era exactamente este
        // sintoma antes de pasar la lectura de ficheros a sceIo.
        return T(I18n::kLoadErrRead);
    default:
        return T(I18n::kLoadErrUnknown);
    }
}

/// Fuente global: la necesita el manejador de terminate, que no recibe argumentos.
vita2d_pgf* g_font = nullptr;

/// Identificador del hilo principal, para distinguirlo en el manejador.
SceUID g_main_thread_id = 0;

/// Ultimo error capturado, para poder enseñarlo sin bloquear al que lo lanzo.
std::string g_last_error;

constexpr char kCrashLog[] = "ux0:/data/azahar/crash.txt";

/// Escribe un mensaje en ux0:/data/azahar/crash.txt usando solo la API del
/// sistema: sirve incluso antes de que exista pantalla o runtime de C++.
void WriteCrashLog(const char* title, const char* detail) {
    sceIoMkdir("ux0:/data", 0777);
    sceIoMkdir("ux0:/data/azahar", 0777);
    const char* game_log = Common::VitaGameLog();
    for (const char* file : {static_cast<const char*>(kCrashLog), game_log}) {
        if (file[0] == '\0') {
            continue;
        }
        const SceUID fd = sceIoOpen(file, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
        if (fd < 0) {
            continue;
        }
        sceIoWrite(fd, title, std::strlen(title));
        sceIoWrite(fd, ": ", 2);
        if (detail != nullptr) {
            sceIoWrite(fd, detail, std::strlen(detail));
        }
        sceIoWrite(fd, "\n", 1);
        sceIoClose(fd);
    }
}

/**
 * El crash.txt de este juego (0.1.9.7): ux0:/data/azahar/crash_<nombre del
 * fichero de la ROM>.txt, con lo que no sea letra o numero cambiado por '_'.
 * Empieza de cero en cada partida con la version y los ajustes; el de la
 * partida anterior de ese juego queda como crash_<nombre>_anterior.txt.
 */
void StartGameLog(const std::string& rom_path) {
    std::string name = rom_path.substr(rom_path.find_last_of("/:") + 1);
    const std::size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) {
        name.resize(dot);
    }
    for (char& c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c))) {
            c = '_';
        }
    }
    if (name.size() > 60) {
        name.resize(60);
    }
    const std::string base = "ux0:/data/azahar/crash_" + name;
    const std::string path = base + ".txt";
    const std::string previous = base + "_anterior.txt";
    Common::SetVitaGameLog(nullptr);
    sceIoRemove(previous.c_str());
    sceIoRename(path.c_str(), previous.c_str());
    Common::SetVitaGameLog(path.c_str());
    WriteCrashLog("version", kVersion);
    NoteUserSettings();
}

/// Extrae tipo y mensaje de la excepcion que esta provocando el terminate.
/// El tipo importa tanto como el mensaje: "Unknown error" a secas no dice nada,
/// pero sabiendo que viene de un std::system_error ya se sabe donde mirar.
std::string CurrentExceptionText() {
    if (!std::current_exception()) {
        // Sin excepcion en vuelo el terminate viene de un ASSERT de Azahar, que
        // deja apuntado el fichero y la linea antes de parar. El mensaje del
        // assert ya esta en crash.txt, escrito por el propio LOG_CRITICAL.
        const char* fatal = Common::LastFatalMessage();
        if (fatal != nullptr && fatal[0] != '\0') {
            return std::string("ASSERT en ") + fatal + " (mensaje completo en crash.txt)";
        }
        return "sin excepcion activa (terminate directo)";
    }
    try {
        std::rethrow_exception(std::current_exception());
    } catch (const std::system_error& e) {
        return std::string("system_error [") + std::to_string(e.code().value()) + " " +
               e.code().category().name() + "] " + e.what();
    } catch (const std::bad_alloc& e) {
        return std::string("bad_alloc (sin memoria): ") + e.what();
    } catch (const std::exception& e) {
        return std::string("[") + typeid(e).name() + "] " + e.what();
    } catch (...) {
        return "excepcion de tipo no estandar";
    }
}

/**
 * Manejador de terminate para ANTES de main().
 *
 * Los inicializadores estaticos (Citra tiene casi 200) corren antes que main,
 * asi que una excepcion ahi no la ve ni el try/catch ni el manejador que se
 * instala dentro. Y sin vita2d todavia no hay pantalla donde escribir, asi que
 * el mensaje va a un fichero.
 */
[[noreturn]] void OnTerminateEarly() {
    WriteCrashLog("Excepcion antes de main", CurrentExceptionText().c_str());
    sceKernelExitProcess(1);
    __builtin_unreachable();
}

/// Se instala en cuanto arranca el proceso, antes que los demas constructores.
/// Deja constancia de que llego hasta aqui, para poder distinguir "fallo antes
/// de arrancar nada" de "fallo mas adelante".
__attribute__((constructor(101))) void InstallEarlyTerminateHandler() {
    WriteCrashLog("inicio", "proceso arrancado, inicializadores estaticos por delante");
    std::set_terminate(OnTerminateEarly);
}

} // namespace

extern "C" {

/**
 * Implementacion propia de pthread_cancel.
 *
 * libstdc++ decide si el programa tiene hilos comprobando si resuelve una
 * referencia DEBIL a pthread_cancel (ver gthr-default.h). Sin ella,
 * __gthread_active_p() devuelve 0 y cualquier std::thread lanza
 * "Enable multithreading to use std::thread".
 *
 * Forzar la del VitaSDK con -Wl,-u arrastraba tambien pte_throw y
 * pte_osThreadCancel, la maquinaria de cancelacion de pthread-embedded, que
 * tiene su propio estado global. Definiendola aqui el simbolo resuelve igual,
 * el enlazador ya no saca ese objeto del archivo, y no entra nada mas.
 *
 * Azahar nunca cancela hilos, asi que esta funcion no llega a ejecutarse.
 */
#ifdef AZAHAR_ENABLE_THREADS
int pthread_cancel(pthread_t thread) {
    (void)thread;
    return ENOSYS;
}
#endif

/**
 * Guardas propias para los "static" locales de funcion.
 *
 * C++ protege la inicializacion de un static local frente a varios hilos
 * llamando a __cxa_guard_acquire/release. La implementacion de libstdc++ se
 * apoya en su maquinaria de espera atomica (bits/atomic_wait.h), que en esta
 * consola falla: con los hilos activos, las ~4600 guardas del binario tumbaban
 * la aplicacion antes incluso de llegar a main().
 *
 * La salida facil era compilar con -fno-threadsafe-statics, que quita las
 * guardas del todo. Arranca, pero deja las inicializaciones sin proteger: dos
 * hilos pueden inicializar el mismo objeto a la vez y corromper memoria.
 *
 * Aqui se sustituyen por una implementacion propia con un mutex normal y
 * corriente. Es mas lenta que la de libstdc++, pero la inicializacion de un
 * static ocurre una sola vez, asi que da igual.
 *
 * El mutex es recursivo a proposito: la inicializacion de un static puede
 * disparar la de otro, y hay que poder anidarlas sin bloquearse.
 */
static pthread_mutex_t g_static_guard_mutex;
static pthread_cond_t g_static_guard_cond;

__attribute__((constructor(102))) static void InitStaticGuardMutex() {
    pthread_mutex_init(&g_static_guard_mutex, nullptr);
    pthread_cond_init(&g_static_guard_cond, nullptr);
}

/**
 * Interceptores de las funciones de hilos.
 *
 * libstdc++ convierte el valor que devuelven en un std::system_error sin
 * mirarlo. Estamos viendo codigos como 0x8BBDC1CC, que cambian entre
 * ejecuciones y caen en rango de direcciones: no son errores, es memoria sin
 * inicializar. Estos envoltorios (activados con -Wl,--wrap) anotan quien
 * devuelve que, para saber cual de todas es.
 */
/// Escribe un hexadecimal de 8 digitos en buf y devuelve cuantos caracteres uso.
std::size_t AppendHex32(char* buf, unsigned int value) {
    buf[0] = '0';
    buf[1] = 'x';
    std::size_t n = 2;
    for (int shift = 28; shift >= 0; shift -= 4) {
        const unsigned int digit = (value >> shift) & 0xF;
        buf[n++] = static_cast<char>(digit < 10 ? '0' + digit : 'a' + digit - 10);
    }
    return n;
}

void LogThreadError(const char* what, int rc) {
    // Formateo a mano: la printf reducida de la Vita no es de fiar con %X.
    char buf[80];
    std::size_t n = 0;
    while (what[n] != '\0' && n < 40) {
        buf[n] = what[n];
        n++;
    }
    buf[n++] = ' ';
    n += AppendHex32(buf + n, static_cast<unsigned int>(rc));
    buf[n] = '\0';
    WriteCrashLog("hilos", buf);
}

/**
 * Interceptor de la funcion con la que libstdc++ lanza std::system_error.
 *
 * Ninguna de las funciones de hilos interceptadas devolvia error, asi que la
 * excepcion sale de otro sitio. Envolviendo directamente el punto por el que
 * pasan TODAS (std::thread, std::mutex, std::call_once, condition_variable...)
 * y anotando la direccion de retorno, esa direccion se traduce luego con
 * addr2line al fichero y la linea exactos de quien la lanza.
 */
[[noreturn]] void __real__ZSt20__throw_system_errori(int code);
[[noreturn]] void __wrap__ZSt20__throw_system_errori(int code) {
    char buf[80];
    std::size_t n = 0;
    const char* label = "system_error codigo ";
    while (label[n] != '\0') {
        buf[n] = label[n];
        n++;
    }
    n += AppendHex32(buf + n, static_cast<unsigned int>(code));
    const char* from = " lanzado desde ";
    for (std::size_t i = 0; from[i] != '\0'; i++) {
        buf[n++] = from[i];
    }
    n += AppendHex32(buf + n,
                     static_cast<unsigned int>(reinterpret_cast<std::uintptr_t>(
                         __builtin_return_address(0))));
    const char* tid = " hilo ";
    for (std::size_t i = 0; tid[i] != '\0'; i++) {
        buf[n++] = tid[i];
    }
    n += AppendHex32(buf + n, static_cast<unsigned int>(sceKernelGetThreadId()));
    buf[n] = '\0';
    WriteCrashLog("throw", buf);

    // __builtin_return_address da basura aqui (el compilador convierte esta
    // funcion en un salto directo y se pierde el marco). Se recorre la pila a
    // mano buscando direcciones que caigan dentro del codigo: esas son los
    // retornos de la cadena de llamadas, y traducidas dan la traza real.
    {
        const std::uintptr_t* stack =
            reinterpret_cast<const std::uintptr_t*>(__builtin_frame_address(0));
        char trace[96];
        std::size_t t = 0;
        std::size_t found = 0;
        for (std::size_t i = 0; i < 96 && found < 6; i++) {
            const std::uintptr_t value = stack[i];
            if (value >= 0x81000040u && value <= 0x812d0000u) {
                t += AppendHex32(trace + t, static_cast<unsigned int>(value));
                trace[t++] = ' ';
                found++;
            }
        }
        trace[t] = '\0';
        WriteCrashLog("traza", trace);
    }

    __real__ZSt20__throw_system_errori(code);
}

int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*),
                          void* arg) {
    const int rc = __real_pthread_create(thread, attr, start, arg);
    if (rc != 0) {
        LogThreadError("pthread_create", rc);
    }
    return rc;
}

int __real_pthread_mutex_lock(pthread_mutex_t*);
int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex) {
    const int rc = __real_pthread_mutex_lock(mutex);
    if (rc != 0) {
        LogThreadError("pthread_mutex_lock", rc);
    }
    return rc;
}

int __real_pthread_join(pthread_t, void**);
int __wrap_pthread_join(pthread_t thread, void** value) {
    const int rc = __real_pthread_join(thread, value);
    if (rc != 0) {
        LogThreadError("pthread_join", rc);
    }
    return rc;
}

int __real_pthread_detach(pthread_t);
int __wrap_pthread_detach(pthread_t thread) {
    const int rc = __real_pthread_detach(thread);
    if (rc != 0) {
        LogThreadError("pthread_detach", rc);
    }
    return rc;
}

// En el ABI de ARM la guarda es una palabra de 4 bytes: el bit 0 del primero
// indica "ya inicializado". Los demas quedan libres, y aqui se usa el segundo
// para marcar "hay un hilo inicializandolo ahora mismo".
int __cxa_guard_acquire(int* guard) {
    unsigned char* bytes = reinterpret_cast<unsigned char*>(guard);

    pthread_mutex_lock(&g_static_guard_mutex);
    while (true) {
        // Lectura con semantica de ADQUISICION, emparejada con la liberacion
        // de __cxa_guard_release: asegura ver el objeto completo, no solo el
        // flag. Coger el mutex ya ordena, pero dejarlo explicito evita que un
        // cambio futuro lo rompa sin darse cuenta.
        if ((__atomic_load_n(&bytes[0], __ATOMIC_ACQUIRE) & 1) != 0) {
            pthread_mutex_unlock(&g_static_guard_mutex);
            return 0; // ya inicializado, el llamante no hace nada
        }
        if (bytes[1] == 0) {
            // Nadie lo esta haciendo: marcarlo y SOLTAR el mutex.
            //
            // Soltarlo aqui es lo importante. La version anterior devolvia con
            // el mutex cogido durante toda la inicializacion, y como es uno
            // solo para las 4641 guardas del binario, bastaba con que un
            // inicializador esperase a otro hilo para bloquear el programa
            // entero: ese otro hilo no podia ni empezar un static distinto.
            bytes[1] = 1;
            pthread_mutex_unlock(&g_static_guard_mutex);
            return 1;
        }
        // Otro hilo esta con esta misma guarda: esperar a que termine.
        pthread_cond_wait(&g_static_guard_cond, &g_static_guard_mutex);
    }
}

void __cxa_guard_release(int* guard) {
    unsigned char* bytes = reinterpret_cast<unsigned char*>(guard);

    pthread_mutex_lock(&g_static_guard_mutex);
    bytes[1] = 0;

    // El flag se escribe con semantica de LIBERACION, y esto es imprescindible
    // en ARM.
    //
    // Cuando otro hilo pasa por un static ya inicializado, GCC no llama a esta
    // funcion: comprueba el bit en linea (ldr + dmb + tst) y usa el objeto
    // directamente. Esa barrera suya solo sirve si al escribir el flag se
    // garantiza que las escrituras del objeto ya son visibles.
    //
    // Coger el mutex es una barrera de ADQUISICION, no de liberacion, asi que
    // no ordenaba nada respecto a la construccion del objeto, que ocurrio antes
    // de entrar aqui. En x86 da igual porque su modelo de memoria es fuerte; en
    // ARM el otro hilo podia ver el flag puesto y el objeto a medio escribir,
    // incluido su puntero de tabla virtual: la siguiente llamada virtual
    // saltaba a una direccion invalida.
    __atomic_store_n(&bytes[0], static_cast<unsigned char>(1), __ATOMIC_RELEASE);

    pthread_mutex_unlock(&g_static_guard_mutex);
    pthread_cond_broadcast(&g_static_guard_cond);
}

void __cxa_guard_abort(int* guard) {
    // El inicializador lanzo: dejar la guarda libre para que otro reintente.
    volatile unsigned char* bytes = reinterpret_cast<volatile unsigned char*>(guard);

    pthread_mutex_lock(&g_static_guard_mutex);
    bytes[1] = 0;
    pthread_mutex_unlock(&g_static_guard_mutex);
    pthread_cond_broadcast(&g_static_guard_cond);
}

} // extern "C"

namespace {

/// Espera a que el usuario pulse CIRCULO.
void WaitForCircle() {
    SceCtrlData pad{};
    // Primero esperar a que suelte, para no comerse la pulsacion anterior.
    do {
        sceCtrlPeekBufferPositive(0, &pad, 1);
    } while (pad.buttons & SCE_CTRL_CIRCLE);
    do {
        sceCtrlPeekBufferPositive(0, &pad, 1);
    } while (!(pad.buttons & SCE_CTRL_CIRCLE));
}

/// Memoria de usuario libre en KB, o -1 si no se puede consultar.
int FreeMemoryKB() {
    SceKernelFreeMemorySizeInfo info{};
    info.size = sizeof(info);
    if (sceKernelGetFreeMemorySize(&info) < 0) {
        return -1;
    }
    return info.size_user / 1024;
}

/**
 * CDRAM libre, en KB.
 *
 * Son 128 MB de memoria dedicada al chip grafico que NO salen del presupuesto
 * de RAM de usuario, y hoy solo la toca vita2d para sus buffers de pantalla. El
 * backend GXM va a vivir ahi, asi que conviene ver el numero desde el principio:
 * es el unico sitio donde se puede gastar memoria sin quitarsela al heap.
 */
int FreeCdramKB() {
    SceKernelFreeMemorySizeInfo info{};
    info.size = sizeof(info);
    if (sceKernelGetFreeMemorySize(&info) < 0) {
        return -1;
    }
    return info.size_cdram / 1024;
}

/**
 * Presupuesto total de RAM de usuario, en KB, deducido.
 *
 * No se puede leer directamente: sceKernelGetFreeMemorySize dice lo que QUEDA,
 * y para cuando corre main() el heap de newlib ya esta reservado entero (lo
 * hace libc antes de entrar). Sumandoselo de vuelta sale el presupuesto con el
 * que arranco el proceso, menos el binario y las reservas del sistema -- que
 * son unos pocos MB y no cambian la conclusion.
 *
 * Llamarlo PRONTO importa: cada hilo y cada textura que se cree despues salen
 * de aqui, y entonces el numero ya no es el presupuesto sino lo que sobra.
 */
int TotalUserMemoryKB() {
    const int free_kb = FreeMemoryKB();
    if (free_kb < 0) {
        return -1;
    }
    return free_kb + static_cast<int>(AZAHAR_HEAP_MB) * 1024;
}

/**
 * Si la consola ha concedido de verdad el modo de memoria ampliada.
 *
 * No hay ninguna llamada que responda "ATTRIBUTE2=12 ha sido aceptado": lo
 * unico observable es cuanta memoria hay. Una aplicacion normal se mueve en
 * ~256 MB de presupuesto y el modo ampliado lo sube a ~365, asi que el total
 * deducido cae claramente a un lado u otro de 300 MB y nunca cerca.
 *
 * Es una deduccion, no una lectura, y por eso lo que se pinta en pantalla es la
 * cifra ADEMAS del SI/no: si algun dia el umbral se queda mal puesto, el numero
 * sigue diciendo la verdad.
 */
bool ExtendedMemoryGranted() {
    const int total_kb = TotalUserMemoryKB();
    return total_kb > 300 * 1024;
}

/// Muestra un error a pantalla completa con el estado de memoria y espera.
void DrawFatalError(vita2d_pgf* font, const char* title, const std::string& detail) {
    vita2d_start_drawing();
    vita2d_clear_screen();
    vita2d_pgf_draw_text(font, 40, 80, 0xFF4040FF, 1.3f, title);

    // El texto puede ser largo: se parte en lineas de ~70 caracteres.
    int y = 130;
    for (std::size_t i = 0; i < detail.size() && y < 400; i += 70) {
        vita2d_pgf_draw_text(font, 40, y, kColorText, 0.95f, detail.substr(i, 70).c_str());
        y += 26;
    }

    vita2d_pgf_draw_textf(font, 40, 440, kColorDim, 0.95f, T(I18n::kFreeUserMemory),
                          FreeMemoryKB());

    // El heap de libc es cosa aparte de la memoria libre de la consola: lo
    // reserva newlib entero antes de main(), asi que la linea de arriba puede
    // decir que sobra memoria mientras el heap esta a tope. Justo eso paso con
    // el error "Sin memoria" al cargar una ROM: se persiguio el presupuesto de
    // la aplicacion y de la disposicion del binario durante dos versiones
    // cuando lo que faltaba era heap. Con estas cifras se ve de un vistazo.
    const struct mallinfo info = mallinfo();
    vita2d_pgf_draw_textf(font, 40, 464, kColorDim, 0.95f,
                          T(I18n::kHeapInfo),
                          static_cast<unsigned int>(AZAHAR_HEAP_MB),
                          static_cast<int>(info.uordblks / 1024),
                          static_cast<int>(info.fordblks / 1024));
    vita2d_pgf_draw_text(font, 40, 500, kColorDim, 0.95f, T(I18n::kCircleBack));
    vita2d_end_drawing();
    vita2d_swap_buffers();

    WaitForCircle();
}

/// Carga y ejecuta un juego hasta que el usuario sale o falla.
void RunGame(vita2d_pgf* font, const std::string& path) {
    // Ver StartKeepAwake: sin tocar la consola, el sistema no la suspende.
    g_in_game.store(true, std::memory_order_relaxed);
    struct InGameEnd {
        ~InGameEnd() {
            g_in_game.store(false, std::memory_order_relaxed);
        }
    } in_game_end;
    DrawMessage(font, T(I18n::kLoading), path.c_str());
    LOG_INFO(Frontend, "Cargando {} (memoria libre: {} KB)", path, FreeMemoryKB());

    // Las rutas que use el emulador quedan anotadas: si algo peta recorriendo
    // directorios, saber cual estaba mirando es media respuesta.
    StartGameLog(path);
    WriteCrashLog("carga", path.c_str());
    WriteCrashLog("ruta user", FileUtil::GetUserPath(FileUtil::UserPath::UserDir).c_str());
    WriteCrashLog("ruta sdmc", FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir).c_str());
    WriteCrashLog("ruta nand", FileUtil::GetUserPath(FileUtil::UserPath::NANDDir).c_str());

    auto& system = Core::System::GetInstance();

    WriteCrashLog("carga", "creando ventana de emulacion");
    auto window = std::make_unique<VitaFrontend::EmuWindow_Vita>();
    window->SetStatsFont(font);

    WriteCrashLog("carga", "llamando a System::Load");

    // Azahar esta escrito para PC, donde una reserva de memoria no falla nunca:
    // lanza bad_alloc y nadie la captura. Sin este try, en la Vita eso acaba en
    // un abort() silencioso y la consola solo dice "la aplicacion ha fallado".
    Core::System::ResultStatus load_result;
    try {
        load_result = system.Load(*window, path);
    } catch (const std::bad_alloc&) {
        LOG_ERROR(Frontend, "Sin memoria al cargar {}", path);
        DrawFatalError(font, T(I18n::kErrNoMemory), T(I18n::kErrNoMemoryDetail));
        if (system.IsPoweredOn()) {
            system.Shutdown();
        }
        return;
    } catch (const std::exception& e) {
        LOG_ERROR(Frontend, "Excepcion al cargar {}: {}", path, e.what());
        DrawFatalError(font, T(I18n::kErrLoad), e.what());
        if (system.IsPoweredOn()) {
            system.Shutdown();
        }
        return;
    }
    if (load_result != Core::System::ResultStatus::Success) {
        LOG_ERROR(Frontend, "Fallo al cargar {}: {}", path, static_cast<int>(load_result));
        DrawFatalError(font, LoadErrorMessage(load_result), system.GetStatusDetails());
        if (system.IsPoweredOn()) {
            system.Shutdown();
        }
        return;
    }

    LOG_INFO(Frontend, "Juego cargado (memoria libre: {} KB)", FreeMemoryKB());
    if (Settings::GetWorkingGraphicsAPI() == Settings::GraphicsAPI::GXM) {
        // Los shaders que este juego uso en partidas anteriores, a memoria antes
        // de empezar (ver PreloadCgCache). Aun no hay emulacion: vita2d es solo
        // de este hilo.
        u64 program_id = 0;
        system.GetAppLoader().ReadProgramId(program_id);
        DrawShaderLoading(font, 0, 0);
        const u32 loaded = Gxm::PreloadCgCache(
            program_id, [font](u32 done, u32 total) { DrawShaderLoading(font, done, total); });
        if (loaded == 0) {
            // Primera partida de este juego: que se lea el aviso.
            sceKernelDelayThread(800 * 1000);
        }
    }
    WriteCrashLog("carga", "System::Load OK, entrando al bucle de emulacion");
    system.RegisterCoreLoopThreadId();

    // Mientras el juego no haya pintado su primer fotograma, la pantalla se
    // queda con el "Cargando..." y no hay forma de saber si avanza o esta
    // colgado. Este contador se dibuja hasta que llegue ese primer fotograma.
    const SceUInt64 start_us = sceKernelGetProcessTimeWide();

    // Contador de 32 bits a proposito: la printf reducida de la Vita no
    // implementa los especificadores de 64 bits (%llu). Pasarle uno descuadra
    // los argumentos, lee basura y acaba saltando a una direccion invalida.
    // Es el mismo fallo que tenia sscanf con %zd.
    unsigned int loops = 0;
    /**
     * VIGILANTE DE CONGELACIONES (0.1.9.0). En 0.1.8.9, al pulsar A en el
     * titulo de Zafiro Alfa el emulador se quedaba parado sin crashear y sin
     * escribir nada mas en crash.txt. Este hilo mira si el bucle avanza; si
     * pasan 6 segundos (y otra vez a los 30) sin una vuelta, anota en que paso
     * esta el hilo de emulacion (Common::vita_stage) y los PC de los ARM11.
     */
    std::atomic<unsigned int> heartbeat{0};
    std::atomic<bool> watchdog_stop{false};
    /**
     * 0.3.8.8: con prioridad alta. A la de por defecto (160, la del hilo de
     * emulacion) no llego a escribir nada cuando Smash Bros. se quedo parado
     * en el arranque con el JIT: el log se corto sin "congelado". Duerme casi
     * siempre, asi que no le quita nada a nadie. Ademas anota las palabras del
     * juego alrededor de cada PC (para desensamblarlas fuera) y, solo con el
     * piloto automatico, a los 20 s provoca un fallo a proposito: el volcado
     * de la consola trae los registros de TODOS los hilos, que es lo que dice
     * donde esta atascado cada uno.
     */
    std::thread watchdog([&heartbeat, &watchdog_stop, &system, &window] {
        sceKernelChangeThreadPriority(sceKernelGetThreadId(), 64);
        const auto words_at = [&system](u32 pc) {
            std::string out;
            const u32 begin = (pc & ~3u) - 16;
            for (u32 at = begin; at < begin + 40; at += 4) {
                const u8* p = system.Memory().GetPointer(at);
                u32 word = 0;
                if (p != nullptr) {
                    std::memcpy(&word, p, 4);
                }
                out += fmt::format(" {:08x}", word);
            }
            return fmt::format("{:#010x}:{}", begin, out);
        };
        unsigned int last = ~0u;
        unsigned int still = 0;
        /**
         * SIN FOTOGRAMAS PERO SIN PARAR (0.3.8.8). Smash Bros. con el JIT se
         * quedaba en el arranque sin "congelado": el bucle seguia dando vueltas,
         * pero tan despacio que no llegaba a la siguiente marca de avance. Cada
         * 10 s sin ningun fotograma nuevo se anota cuanto ha avanzado y en que
         * se va el ARM (las cuentas del JIT, que sin fotogramas no lee nadie).
         */
        unsigned int tick = 0;
        unsigned int beat_loops = 0;
        u64 beat_frames = 0;
        unsigned int beats = 0;
        while (!watchdog_stop.load(std::memory_order_relaxed)) {
            sceKernelDelayThread(1000 * 1000);
            const unsigned int now = heartbeat.load(std::memory_order_relaxed);
            if (++tick % 10 == 0) {
                const u64 frames = window->FramesPresented();
                if (frames == beat_frames && now != beat_loops && beats < 40) {
                    beats++;
                    Core::ArmJit::Stats jit;
                    Core::ArmJit::TakeStats(jit);
                    u32 code_bytes = 0;
                    u32 code_blocks = 0;
                    Core::ArmJit::CodeUsage(code_bytes, code_blocks);
                    const u32 pc0 = system.GetCore(0).GetPC();
                    const std::string text = fmt::format(
                        "10 s sin fotogramas: {} ciclos (van {}), en '{}', gpu en '{}', pc0 "
                        "{:#010x} pc1 {:#010x} | arm: {} instr ({} jit), {} rodajas, {} ms "
                        "(svc {}, compilar {}), {} bloques en {} KB",
                        now - beat_loops, now, Common::vita_stage.load(std::memory_order_relaxed),
                        Common::vita_gpu_stage.load(std::memory_order_relaxed), pc0,
                        system.GetCore(1).GetPC(), jit.instructions, jit.jit_instructions,
                        jit.slices, jit.arm_us / 1000, jit.svc_us / 1000, jit.compile_us / 1000,
                        code_blocks, code_bytes / 1024);
                    WriteCrashLog("latido", text.c_str());
                    if (beats <= 3) {
                        WriteCrashLog("latido: codigo pc0", words_at(pc0).c_str());
                    }
                }
                beat_frames = frames;
                beat_loops = now;
            }
            if (now != last) {
                last = now;
                still = 0;
                continue;
            }
            still++;
            if (still == 6 || still == 30) {
                const u32 pc0 = system.GetCore(0).GetPC();
                const u32 pc1 = system.GetCore(1).GetPC();
                const std::string text = fmt::format(
                    "{} s sin avanzar, en '{}' (ciclo {}), pc0 {:#010x} pc1 {:#010x}, hilo de "
                    "la gpu en '{}'",
                    still, Common::vita_stage.load(std::memory_order_relaxed), now, pc0, pc1,
                    Common::vita_gpu_stage.load(std::memory_order_relaxed));
                WriteCrashLog("congelado", text.c_str());
                WriteCrashLog("congelado: codigo pc0", words_at(pc0).c_str());
                if (pc1 != 0) {
                    WriteCrashLog("congelado: codigo pc1", words_at(pc1).c_str());
                }
            }
            if (still == 20 && g_pilot_running.load(std::memory_order_relaxed)) {
                WriteCrashLog("congelado", "piloto: 20 s parado, fallo a proposito para tener "
                                           "el volcado con los registros de todos los hilos");
                sceKernelDelayThread(200 * 1000);
                *reinterpret_cast<volatile unsigned int*>(0x00000C00) = still;
            }
        }
    });

    bool user_quit = false;

    try {
        while (!window->ShouldExit() && !user_quit) {
            // La ventana solo mira los botones cuando el juego termina un
            // fotograma. Si no llega ninguno, no habria forma de salir, asi que
            // la combinacion se comprueba tambien aqui.
            if ((loops % 16) == 0) {
                SceCtrlData pad{};
                sceCtrlPeekBufferPositive(0, &pad, 1);
                pad.buttons |= VitaFrontend::Input::PilotButtons();
                if ((pad.buttons & SCE_CTRL_START) && (pad.buttons & SCE_CTRL_SELECT)) {
                    user_quit = true;
                }
            }

            const bool loading_screen = window->FramesPresented() == 0 && (loops % 64) == 0;
            if (loading_screen) {
                // vita2d desde este hilo solo con el de la GPU parado: con la GPU
                // en otro nucleo, la primera presentacion puede estar en marcha.
                system.GPU().Sync();
            }
            if (loading_screen && window->FramesPresented() == 0) {
                const unsigned int seconds =
                    static_cast<unsigned int>((sceKernelGetProcessTimeWide() - start_us) / 1000000);
                vita2d_start_drawing();
                vita2d_clear_screen();
                vita2d_pgf_draw_text(font, 40, 80, kColorAccent, 1.3f, T(I18n::kEmulating));
                vita2d_pgf_draw_text(font, 40, 130, kColorText, 0.95f, T(I18n::kNoFirstFrame));
                vita2d_pgf_draw_textf(font, 40, 170, kColorDim, 0.95f, T(I18n::kTimeCycles),
                                      seconds, loops);
                vita2d_pgf_draw_textf(font, 40, 210, kColorDim, 0.95f, T(I18n::kFreeMemory),
                                      FreeMemoryKB());

                // Si algun hilo murio por una excepcion, se avisa aqui sin
                // tapar el contador: interesa mas saber si la emulacion avanza.
                if (!g_last_error.empty()) {
                    vita2d_pgf_draw_text(font, 40, 280, 0xFF4040FF, 0.9f, T(I18n::kThreadDied));
                    vita2d_pgf_draw_text(font, 40, 308, kColorDim, 0.85f,
                                         g_last_error.substr(0, 74).c_str());
                }

                vita2d_pgf_draw_text(font, 40, 500, kColorDim, 0.9f, T(I18n::kExitCombo));
                vita2d_end_drawing();
                vita2d_swap_buffers();
            }
            // Rastro del avance en crash.txt: si el proceso muere de golpe, la
            // ultima marca dice cuantos ciclos llego a dar y con cuanta memoria.
            //
            // Los primeros 64 ciclos se anotan uno a uno porque es justo donde
            // se ha estado muriendo; a partir de ahi solo las potencias de dos,
            // para no castigar la tarjeta de memoria con escrituras constantes.
            if (loops < 64 || (loops & (loops - 1)) == 0) {
                char mark[96];
                std::size_t m = 0;
                const char* label = "ciclo ";
                while (label[m] != '\0') {
                    mark[m] = label[m];
                    m++;
                }
                m += AppendHex32(mark + m, loops);
                const char* mem = " memoria KB ";
                for (std::size_t i = 0; mem[i] != '\0'; i++) {
                    mark[m++] = mem[i];
                }
                m += AppendHex32(mark + m, static_cast<unsigned int>(FreeMemoryKB()));

                // El contador de ciclos dice cuanto avanza, pero no donde. El PC
                // de los dos nucleos ARM11 si: una direccion que no se mueve
                // entre marcas es un juego colgado, y una que salta a un sitio
                // absurdo es codigo emulado corriendo por donde no debe.
                const char* pcs = " pc0 ";
                for (std::size_t i = 0; pcs[i] != '\0'; i++) {
                    mark[m++] = pcs[i];
                }
                m += AppendHex32(mark + m, system.GetCore(0).GetPC());
                const char* pc1 = " pc1 ";
                for (std::size_t i = 0; pc1[i] != '\0'; i++) {
                    mark[m++] = pc1[i];
                }
                m += AppendHex32(mark + m, system.GetCore(1).GetPC());
                mark[m] = '\0';
                WriteCrashLog("avance", mark);
            }
            loops++;
            heartbeat.store(loops, std::memory_order_relaxed);

            Common::vita_stage.store("emulando", std::memory_order_relaxed);
            const Core::System::ResultStatus result = system.RunLoop();
            if (result != Core::System::ResultStatus::Success &&
                result != Core::System::ResultStatus::ShutdownRequested) {
                LOG_ERROR(Frontend, "RunLoop devolvio {}: {}", static_cast<int>(result),
                          system.GetStatusDetails());
                WriteCrashLog("RunLoop error", system.GetStatusDetails().c_str());
                system.GPU().StopThread();
                DrawFatalError(font, T(I18n::kErrEmulation), system.GetStatusDetails());
                break;
            }
            if (result == Core::System::ResultStatus::ShutdownRequested) {
                break;
            }
        }
    } catch (const std::bad_alloc&) {
        // Anotado tambien en crash.txt: la pantalla se puede perder si el
        // usuario cierra antes de leerla, el fichero no.
        WriteCrashLog("emulacion", "bad_alloc: sin memoria durante la emulacion");
        LOG_ERROR(Frontend, "Sin memoria durante la emulacion");
        system.GPU().StopThread();
        DrawFatalError(font, T(I18n::kErrEmulationMemory), T(I18n::kErrEmulationMemoryDetail));
    } catch (const std::exception& e) {
        std::string detail = std::string("[") + typeid(e).name() + "] " + e.what();
        WriteCrashLog("emulacion", detail.c_str());
        LOG_ERROR(Frontend, "Excepcion durante la emulacion: {}", e.what());
        system.GPU().StopThread();
        DrawFatalError(font, T(I18n::kErrEmulation), detail);
    } catch (...) {
        WriteCrashLog("emulacion", "excepcion de tipo no estandar");
        system.GPU().StopThread();
        DrawFatalError(font, T(I18n::kErrEmulation), T(I18n::kErrNonStandard));
    }
    watchdog_stop.store(true, std::memory_order_relaxed);
    watchdog.join();

    // La pantalla superior del juego para la ficha del menu de inicio (0.3.4.0),
    // con el ultimo fotograma todavia en pantalla. Solo al salir con START+SELECT:
    // tras un error lo que se ve es el mensaje, no el juego.
    if (window->ShouldExit() && window->FramesPresented() > 0) {
        int top_x = 0;
        int top_y = 0;
        int top_w = 0;
        int top_h = 0;
        VitaFrontend::EmuWindow_Vita::TopScreenRect(top_x, top_y, top_w, top_h);
        VitaFrontend::CaptureGameScreen(path, top_x, top_y, top_w, top_h);
    }

    // Resumen de la cache de traduccion al cerrar la partida (salga como salga:
    // el usuario cerro, o algo de lo de arriba lo corto). Sirve para decidir si
    // agrandar TRANS_CACHE_MB compensa: "capacidad" es la unica cifra que tiene
    // que ver con el tamano del buffer, "invalidacion" pasaria igual con un
    // buffer infinito. Ver el comentario de GetTransCacheFlushCounts.
    {
        std::size_t flushes_capacity = 0;
        std::size_t flushes_invalidation = 0;
        GetTransCacheFlushCounts(flushes_capacity, flushes_invalidation);
        char summary[96] = "vaciados por capacidad ";
        std::size_t n = std::strlen(summary);
        n += AppendHex32(summary + n, static_cast<unsigned int>(flushes_capacity));
        const char* sep = " por invalidacion ";
        for (std::size_t i = 0; sep[i] != '\0'; i++) {
            summary[n++] = sep[i];
        }
        n += AppendHex32(summary + n, static_cast<unsigned int>(flushes_invalidation));
        summary[n] = '\0';
        WriteCrashLog("cache de traduccion", summary);
    }

    system.Shutdown();
    // El menu se dibuja con la espera al refresco, aunque la partida fuera sin limite.
    vita2d_set_vblank_wait(1);
}

/**
 * Ultimo recurso ante una excepcion no capturada.
 *
 * El try/catch de main() solo cubre el hilo principal: una excepcion lanzada en
 * cualquier otro hilo (el de audio, el de logging, los del emulador) llama a
 * std::terminate directamente y el proceso muere sin que main se entere.
 *
 * Este manejador recupera la excepcion en curso y la pinta en pantalla, venga
 * del hilo que venga.
 */
[[noreturn]] void OnTerminate() {
    const std::string detail = CurrentExceptionText();
    WriteCrashLog("Excepcion no capturada", detail.c_str());
    g_last_error = detail;

    // Se probo a matar solo el hilo que lanza, dejando viva la emulacion. No
    // sirve: System::Load se queda esperando a ese hilo y la carga no termina
    // nunca, asi que la consola parece colgada y encima no se aprende nada.
    // Mejor cerrar y quedarse con el registro, que es lo que informa.
    (void)g_main_thread_id;

    // Ojo: este manejador puede correr en CUALQUIER hilo. Si se queda esperando
    // a que el usuario pulse un boton, el resto de hilos siguen vivos y pintando
    // encima, asi que el mensaje no se ve y la consola parece colgada -- que es
    // exactamente lo que pasaba. Se muestra unos segundos y se sale solo.
    if (g_font != nullptr) {
        for (int frame = 0; frame < 60 * 8; frame++) {
            vita2d_start_drawing();
            vita2d_clear_screen();
            vita2d_pgf_draw_text(g_font, 40, 80, 0xFF4040FF, 1.3f, T(I18n::kErrUncaught));
            int y = 130;
            for (std::size_t i = 0; i < detail.size() && y < 420; i += 70) {
                vita2d_pgf_draw_text(g_font, 40, y, kColorText, 0.95f,
                                     detail.substr(i, 70).c_str());
                y += 26;
            }
            vita2d_pgf_draw_textf(g_font, 40, 460, kColorDim, 0.95f, T(I18n::kFreeUserMemory),
                                  FreeMemoryKB());
            vita2d_pgf_draw_textf(g_font, 40, 500, kColorDim, 0.95f, T(I18n::kErrUncaughtFooter),
                                  8 - frame / 60);
            vita2d_end_drawing();
            vita2d_swap_buffers();
        }
    }

    sceKernelExitProcess(1);
    __builtin_unreachable();
}

/// Pinta en que paso del arranque estamos. Si la app muere, lo ultimo que se
/// vea en pantalla dice exactamente donde.
void DrawStep(vita2d_pgf* font, const char* step, I18n::Str shown) {
    WriteCrashLog("paso", step);
    vita2d_start_drawing();
    vita2d_clear_screen();
    vita2d_pgf_draw_text(font, 40, 60, kColorAccent, 1.3f, "Azahar  -  PS Vita");
    vita2d_pgf_draw_text(font, 40, 90, kColorDim, 0.85f, kVersion);
    vita2d_pgf_draw_text(font, 40, 140, kColorText, 1.0f, T(I18n::kStarting));
    vita2d_pgf_draw_text(font, 40, 180, kColorDim, 1.0f, T(shown));

    // El presupuesto de memoria, a la vista desde el primer fotograma.
    //
    // Es el numero que decide todo lo demas -- si el heap puede subir, si el
    // modo New 3DS es viable, cuanto sitio queda para el backend GXM -- y hasta
    // ahora solo aparecia en la pantalla de error fatal, o sea justo cuando ya
    // no sirve para decidir nada. Aqui se ve siempre, se puede fotografiar y no
    // hace falta reproducir ningun fallo para leerlo.
    const bool extended = ExtendedMemoryGranted();
    vita2d_pgf_draw_textf(font, 40, 232, extended ? 0xFF80FF80 : 0xFF60C0FF, 0.85f,
                          T(I18n::kMemoryLine), TotalUserMemoryKB() / 1024,
                          FreeMemoryKB() / 1024, extended ? T(I18n::kYes) : T(I18n::kNo));
    vita2d_pgf_draw_textf(font, 40, 256, kColorDim, 0.85f, T(I18n::kHeapCdramLine),
                          static_cast<unsigned int>(AZAHAR_HEAP_MB), FreeCdramKB() / 1024);

    vita2d_end_drawing();
    vita2d_swap_buffers();
}

/**
 * EL PILOTO AUTOMATICO (0.3.8.2). Para medir sin nadie delante: si existe
 * ux0:/data/azahar/piloto.txt, Azahar arranca el juego que dice, sin pasar por
 * el menu, y pulsa los botones de su guion a su hora. Al terminar mantiene
 * START+SELECT, que sale del juego como siempre. El fichero se gasta al leerlo
 * (pasa a piloto_usado.txt): abrir Azahar a mano despues no lo repite.
 *
 *   juego=POKEMON ZAFIRO ALFA (OROCHI IORI).cci
 *   salir=240
 *   # segundo  botones   milisegundos   (desde que empieza a cargar el juego)
 *   30 start 300
 *   34 a 300
 *   60 abajo 3000
 *   70 a+derecha 500
 *
 * Botones con el nombre de la 3DS: a b x y l r start select arriba abajo
 * izquierda derecha (unidos con +). Se pulsan como los de la Vita a los que
 * estan asignados (ver ApplyDefaultMapping).
 */
constexpr char kPilotPath[] = "ux0:/data/azahar/piloto.txt";
constexpr char kPilotUsedPath[] = "ux0:/data/azahar/piloto_usado.txt";

struct PilotStep {
    double at_s = 0;
    unsigned int buttons = 0;
    unsigned int hold_ms = 0;
    int touch_x = -1; ///< "toque:X:Y" (0.3.9.3), pantalla de abajo del 3DS
    int touch_y = -1;
};
struct Pilot {
    std::string rom;
    double exit_s = 0;
    std::vector<PilotStep> steps;
    std::atomic<bool> stop{false};
};

/**
 * "captura" (0.3.9.3): no es un boton. El piloto guarda la pantalla entera
 * (960x544 reducida a 480x272, RGB de 24 bits sin cabecera de formato) en
 * ux0:/data/azahar/piloto_capturas/NN.rgb, para ver desde fuera de la consola
 * donde esta el juego cuando nadie lo mira.
 */
constexpr unsigned int kPilotShot = 0x80000000u;
constexpr char kPilotShotDir[] = "ux0:/data/azahar/piloto_capturas";

void ClearPilotShots() {
    const SceUID dir = sceIoDopen(kPilotShotDir);
    if (dir < 0) {
        sceIoMkdir(kPilotShotDir, 0777);
        return;
    }
    SceIoDirent entry{};
    std::vector<std::string> names;
    while (sceIoDread(dir, &entry) > 0) {
        names.emplace_back(entry.d_name);
    }
    sceIoDclose(dir);
    for (const std::string& name : names) {
        sceIoRemove((std::string(kPilotShotDir) + "/" + name).c_str());
    }
}

void SavePilotShot(unsigned int index) {
    SceDisplayFrameBuf frame{};
    frame.size = sizeof(frame);
    if (sceDisplayGetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME) < 0 || frame.base == nullptr ||
        frame.pixelformat != SCE_DISPLAY_PIXELFORMAT_A8B8G8R8 || frame.width < 960 ||
        frame.height < 544) {
        Common::VitaNote("piloto", "captura: no hay framebuffer que leer");
        return;
    }
    constexpr int kW = 480;
    constexpr int kH = 272;
    std::vector<u8> image(static_cast<std::size_t>(kW * kH * 3));
    const auto* pixels = static_cast<const u32*>(frame.base);
    for (int y = 0; y < kH; y++) {
        const u32* line = pixels + static_cast<std::size_t>(y * 2) * frame.pitch;
        u8* out = image.data() + static_cast<std::size_t>(y * kW * 3);
        for (int x = 0; x < kW; x++) {
            const u32 c = line[x * 2];
            out[x * 3 + 0] = static_cast<u8>(c & 0xFF);
            out[x * 3 + 1] = static_cast<u8>((c >> 8) & 0xFF);
            out[x * 3 + 2] = static_cast<u8>((c >> 16) & 0xFF);
        }
    }
    const std::string path = fmt::format("{}/{:02}.rgb", kPilotShotDir, index);
    const SceUID fd = sceIoOpen(path.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, image.data(), static_cast<SceSize>(image.size()));
        sceIoClose(fd);
    }
    Common::VitaNote("piloto", fmt::format("captura {:02}", index).c_str());
}

unsigned int PilotButtonMask(const std::string& names) {
    static const std::pair<const char*, unsigned int> kNames[] = {
        {"a", SCE_CTRL_CIRCLE},       {"b", SCE_CTRL_CROSS},        {"x", SCE_CTRL_TRIANGLE},
        {"y", SCE_CTRL_SQUARE},       {"l", SCE_CTRL_LTRIGGER},     {"r", SCE_CTRL_RTRIGGER},
        {"start", SCE_CTRL_START},    {"select", SCE_CTRL_SELECT},  {"arriba", SCE_CTRL_UP},
        {"abajo", SCE_CTRL_DOWN},     {"izquierda", SCE_CTRL_LEFT}, {"derecha", SCE_CTRL_RIGHT},
        {"captura", kPilotShot},
        {"stick_derecha", VitaFrontend::Input::kPilotStickRight},
        {"stick_izquierda", VitaFrontend::Input::kPilotStickLeft},
        {"stick_arriba", VitaFrontend::Input::kPilotStickUp},
        {"stick_abajo", VitaFrontend::Input::kPilotStickDown},
    };
    unsigned int mask = 0;
    std::size_t begin = 0;
    while (begin <= names.size()) {
        const std::size_t end = std::min(names.find('+', begin), names.size());
        const std::string name = names.substr(begin, end - begin);
        for (const auto& [known, bit] : kNames) {
            if (name == known) {
                mask |= bit;
            }
        }
        begin = end + 1;
    }
    return mask;
}

bool LoadPilot(Pilot& pilot) {
    const SceUID fd = sceIoOpen(kPilotPath, SCE_O_RDONLY, 0);
    if (fd < 0) {
        return false;
    }
    std::string text;
    char buffer[1024];
    int read = 0;
    while ((read = sceIoRead(fd, buffer, sizeof(buffer))) > 0) {
        text.append(buffer, static_cast<std::size_t>(read));
    }
    sceIoClose(fd);
    // Gastado: abrir Azahar a mano despues no vuelve a pilotar.
    sceIoRemove(kPilotUsedPath);
    sceIoRename(kPilotPath, kPilotUsedPath);
    std::size_t begin = 0;
    while (begin < text.size()) {
        std::size_t end = text.find('\n', begin);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(begin, end - begin);
        begin = end + 1;
        if (const std::size_t hash = line.find('#'); hash != std::string::npos) {
            line.resize(hash);
        }
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        if (line.rfind("juego=", 0) == 0) {
            pilot.rom = line.substr(6);
        } else if (line.rfind("salir=", 0) == 0) {
            pilot.exit_s = std::atof(line.c_str() + 6);
        } else {
            char names[128] = {};
            double at = 0;
            unsigned int hold = 0;
            if (std::sscanf(line.c_str(), "%lf %127s %u", &at, names, &hold) == 3) {
                PilotStep step{at, PilotButtonMask(names), hold};
                if (const char* touch = std::strstr(names, "toque:")) {
                    std::sscanf(touch, "toque:%d:%d", &step.touch_x, &step.touch_y);
                }
                pilot.steps.push_back(step);
            }
        }
    }
    std::sort(pilot.steps.begin(), pilot.steps.end(),
              [](const PilotStep& a, const PilotStep& b) { return a.at_s < b.at_s; });
    return !pilot.rom.empty();
}

/// Duerme hasta 'until_us' (reloj de proceso) en trozos, para poder parar.
bool PilotSleepUntil(const Pilot& pilot, SceUInt64 until_us) {
    while (!pilot.stop.load(std::memory_order_relaxed)) {
        const SceUInt64 now = sceKernelGetProcessTimeWide();
        if (now >= until_us) {
            return true;
        }
        sceKernelDelayThread(static_cast<SceUInt32>(std::min<SceUInt64>(until_us - now, 50000)));
    }
    return false;
}

void* PilotMain(void* arg) {
    Pilot& pilot = *static_cast<Pilot*>(arg);
    const SceUInt64 start = sceKernelGetProcessTimeWide();
    ClearPilotShots();
    unsigned int shots = 0;
    for (const PilotStep& step : pilot.steps) {
        if (!PilotSleepUntil(pilot, start + static_cast<SceUInt64>(step.at_s * 1e6))) {
            return nullptr;
        }
        if ((step.buttons & kPilotShot) != 0) {
            SavePilotShot(++shots);
            if ((step.buttons & ~kPilotShot) == 0) {
                continue;
            }
        }
        VitaFrontend::Input::SetPilotButtons(step.buttons & ~kPilotShot);
        VitaFrontend::Input::SetPilotTouch(step.touch_x, step.touch_y);
        PilotSleepUntil(pilot, sceKernelGetProcessTimeWide() + step.hold_ms * 1000ull);
        VitaFrontend::Input::SetPilotButtons(0);
        VitaFrontend::Input::SetPilotTouch(-1, -1);
    }
    if (pilot.exit_s > 0 &&
        PilotSleepUntil(pilot, start + static_cast<SceUInt64>(pilot.exit_s * 1e6))) {
        Common::VitaNote("piloto", "fin del guion: START+SELECT para salir del juego");
        VitaFrontend::Input::SetPilotButtons(SCE_CTRL_START | SCE_CTRL_SELECT);
        PilotSleepUntil(pilot, sceKernelGetProcessTimeWide() + 3'000'000ull);
        VitaFrontend::Input::SetPilotButtons(0);
    }
    return nullptr;
}

/// Si hay piloto.txt, juega su partida antes de enseñar el menu.
void RunPilotIfAny(vita2d_pgf* font, const std::vector<RomEntry>& roms) {
    Pilot pilot;
    if (!LoadPilot(pilot)) {
        return;
    }
    const auto rom = std::find_if(roms.begin(), roms.end(),
                                  [&](const RomEntry& entry) { return entry.name == pilot.rom; });
    if (rom == roms.end()) {
        Common::VitaNote("piloto", fmt::format("no esta el juego '{}'", pilot.rom).c_str());
        return;
    }
    Common::VitaNote("piloto", fmt::format("{}: {} pasos, salir a los {:.0f} s", pilot.rom,
                                           pilot.steps.size(), pilot.exit_s)
                                   .c_str());
    pthread_t thread{};
    const bool started = pthread_create(&thread, nullptr, &PilotMain, &pilot) == 0;
    g_pilot_running.store(true, std::memory_order_relaxed);
    VitaFrontend::Input::SetPilotActive(true);
    RunGame(font, rom->path);
    VitaFrontend::Input::SetPilotActive(false);
    g_pilot_running.store(false, std::memory_order_relaxed);
    pilot.stop.store(true, std::memory_order_relaxed);
    if (started) {
        pthread_join(thread, nullptr);
    }
    VitaFrontend::Input::SetPilotButtons(0);
    if (pilot.exit_s > 0) {
        /**
         * Y AZAHAR SE CIERRA SOLO (0.3.8.3). Matarlo desde fuera (quit all de
         * vitacompanion) en plena compilacion de un shader dejaba la marca de
         * "compilando" puesta, y el siguiente arranque lo contaba como aviso
         * contra ese shader: el bucle de pruebas acabaria mandando shaders
         * sanos a la lista negra. Un segundo y medio para que salgan las notas.
         */
        Gxm::CgForgetInterruptedCompile();
        Common::VitaNote("piloto", "partida terminada: Azahar se cierra");
        sceKernelDelayThread(1500 * 1000);
        sceKernelExitProcess(0);
    }
}

/**
 * LA CONSOLA DESPIERTA MIENTRAS HAY JUEGO (0.3.8.8). Azahar no llamaba nunca a
 * sceKernelPowerTick, y la Vita solo sabe que se esta usando por los botones:
 * sin tocarla, al cumplirse el tiempo de suspension automatica el sistema
 * dejaba el proceso entero parado. Lo destapo el piloto automatico con la
 * consola sola (25-10: cualquier juego se paraba a los 13 s de abrirse), pero
 * le pasa igual a quien mira una escena larga sin pulsar nada. Mientras hay un
 * juego cargando o en marcha (g_in_game, ver RunGame), cada segundo se anula
 * el temporizador de suspension; los de oscurecer y apagar la pantalla siguen
 * como el usuario los tenga.
 *
 * SIGUE VIVO? Con el piloto automatico, ademas, cada 2 s se reescribe
 * ux0:/data/azahar/vivo.txt con un contador y la etapa de cada hilo: si
 * crash.txt deja de crecer y vivo.txt sigue contando, el proceso esta vivo y
 * algo bloquea solo a unos hilos. Solo con piloto.txt, para no escribir de mas
 * en la tarjeta en una partida normal.
 */
bool g_write_alive = false;

void* AliveMain(void*) {
    sceKernelChangeThreadPriority(sceKernelGetThreadId(), 64);
    unsigned int count = 0;
    while (true) {
        if (g_in_game.load(std::memory_order_relaxed)) {
            sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
        }
        if (g_write_alive && (++count & 1) == 0) {
            const std::string text = fmt::format(
                "{} {} s etapa '{}' gpu '{}'\n", count / 2,
                static_cast<unsigned int>(sceKernelGetProcessTimeWide() / 1000000),
                Common::vita_stage.load(std::memory_order_relaxed),
                Common::vita_gpu_stage.load(std::memory_order_relaxed));
            const SceUID fd = sceIoOpen("ux0:/data/azahar/vivo.txt",
                                        SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
            if (fd >= 0) {
                sceIoWrite(fd, text.data(), static_cast<SceSize>(text.size()));
                sceIoClose(fd);
            }
        }
        sceKernelDelayThread(1000 * 1000);
    }
    return nullptr;
}

void StartKeepAwake() {
    SceIoStat stat{};
    g_write_alive = sceIoGetstat(kPilotPath, &stat) >= 0;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 32 * 1024);
    pthread_t thread;
    if (pthread_create(&thread, &attr, &AliveMain, nullptr) == 0) {
        pthread_detach(thread);
    }
    pthread_attr_destroy(&attr);
}

/// Los argumentos de main(), para Core::Native::LoadModuleIfMissing.
int g_argc = 0;
char** g_argv = nullptr;

/// Cuerpo real del programa. main() lo envuelve para poder enseñar por pantalla
/// cualquier excepcion en vez de morir con un abort() mudo.
void Run(vita2d_pgf* font) {
    // El idioma de los menus, antes de pintar nada: si no, el arranque saldria
    // en ingles aunque se haya elegido otro. El resto se vuelve a leer en el paso 3.
    LoadUserSettings();
    I18n::SetLanguage(g_user.menu_language);

    DrawStep(font, "1/7 creando carpetas", I18n::kStep1);
    CreateDirectories();
    FileUtil::SetUserPath(kUserDir);

    DrawStep(font, "2/7 iniciando registro de eventos", I18n::kStep2);
    Common::Log::Initialize();
    Common::Log::SetGlobalFilter(Common::Log::Filter(Common::Log::Level::Info));
    Common::Log::Start();
    // La version va en la PRIMERA linea del log a proposito: con varios ciclos
    // de compilar, instalar y mirar, la duda "¿pero que build es esta?" se
    // resuelve abriendo el fichero, sin tener que cruzar fechas.
    LOG_INFO(Frontend, "Azahar para PS Vita iniciando ({})", kVersion);

    DrawStep(font, "3/7 aplicando ajustes", I18n::kStep3);
    ConfigureSettings();
    // Encima, lo que el usuario eligio en la ventana de ajustes la ultima vez.
    LoadUserSettings();
    ApplyUserSettings();
    NoteUserSettings();
    if (g_user.jit_nativo) {
        // Si el modulo de kernel no esta, lo carga y RELANZA la app (0.3.7.9).
        Core::Native::LoadModuleIfMissing(g_argc, g_argv);
    }

    DrawStep(font, "4/7 configurando mandos", I18n::kStep4);
    VitaFrontend::Input::Init();
    VitaFrontend::Input::ApplyDefaultMapping();

    DrawStep(font, "5/7 preparando el nucleo", I18n::kStep5);

    // CryptoPP inicializa sus tablas de punteros a funcion de forma perezosa,
    // la primera vez que se construye un Integer. Ese camino tiene una carrera
    // que en ARM si se manifiesta (ver integer.cpp): si dos hilos entran a la
    // vez, uno puede llamar a un puntero aun sin escribir y saltar a basura.
    //
    // Construir uno aqui, con un solo hilo en marcha, deja las tablas listas
    // antes de que el emulador arranque los suyos.
    {
        const CryptoPP::Integer warm_up(1);
        (void)warm_up;
    }

    auto& system = Core::System::GetInstance();

    DrawStep(font, "6/7 registrando applets", I18n::kStep6);
    Frontend::RegisterDefaultApplets(system);
    system.RegisterImageInterface(std::make_shared<Frontend::ImageInterface>());

    DrawStep(font, "7/7 buscando juegos", I18n::kStep7);
    VitaFrontend::HomeMenu menu(font);
    const std::vector<RomEntry> roms = ScanRoms();
    menu.SetGames(roms);
    // Ver Gxm::RunCgBenchIfAny (0.4.0.0): solo con banco_cg/hacer.txt.
    Gxm::RunCgBenchIfAny();
    // Ver HW::Y2R::RunBenchIfAny (0.4.1.4): solo con banco_mem/y2r.txt.
    HW::Y2R::RunBenchIfAny();
    Gxm::RunMemBenchIfAny();
    RunPilotIfAny(font, roms);
    // Las caratulas que falten, en otro hilo (0.3.4.1). Ver vita_covers.h.
    std::vector<std::string> rom_paths;
    for (const RomEntry& rom : roms) {
        rom_paths.push_back(rom.path);
    }
    VitaFrontend::Covers::Start(rom_paths);
    bool running = true;

    // Estado anterior de los botones: sin esto un solo toque de la cruceta
    // recorreria la lista entera, porque el menu se dibuja a 60 fps.
    unsigned int previous_buttons = 0;
    // Cuando empezo a mantenerse START o SELECT (0 = no se esta manteniendo).
    // Solo cuenta una pulsacion NUEVA: el START+SELECT que viene mantenido de
    // salir de un juego, o el boton que cerro los ajustes, no abre nada.
    SceUInt64 start_hold_begin = 0;
    SceUInt64 select_hold_begin = 0;

    // Mantener un boton solo (sin el otro) durante 'hold_us' abre su ventana.
    // Devuelve de 0 a 1 lo que lleva, o mas de 1 cuando ya toca abrirla.
    const auto hold_progress = [](const SceCtrlData& pad, unsigned int pressed,
                                  unsigned int button, unsigned int other, SceUInt64& begin,
                                  SceUInt64 hold_us) {
        if ((pad.buttons & button) == 0 || (pad.buttons & other) != 0) {
            begin = 0;
            return 0.0f;
        }
        const SceUInt64 now = sceKernelGetProcessTimeWide();
        if (pressed & button) {
            begin = now;
        }
        if (begin == 0) {
            return 0.0f;
        }
        return static_cast<float>(now - begin) / static_cast<float>(hold_us);
    };

    while (running) {
        SceCtrlData pad{};
        sceCtrlPeekBufferPositive(0, &pad, 1);
        const unsigned int pressed = pad.buttons & ~previous_buttons;
        previous_buttons = pad.buttons;

        const float settings_hold = hold_progress(pad, pressed, SCE_CTRL_START, SCE_CTRL_SELECT,
                                                  start_hold_begin, kSettingsHoldUs);
        const float language_hold = hold_progress(pad, pressed, SCE_CTRL_SELECT, SCE_CTRL_START,
                                                  select_hold_begin, kLanguageHoldUs);
        if (settings_hold >= 1.0f) {
            RunSettingsMenu(font);
            start_hold_begin = 0;
            previous_buttons = ~0u;
            continue;
        }
        if (language_hold >= 1.0f) {
            const int chosen = VitaFrontend::RunLanguagePicker(menu, font);
            if (chosen >= 0 && chosen != g_user.menu_language) {
                // La caratula de cada region se pide con el idioma del menu:
                // el hilo no puede leerlo mientras cambia.
                VitaFrontend::Covers::Stop();
                g_user.menu_language = chosen;
                I18n::SetLanguage(chosen);
                SaveUserSettings();
                VitaFrontend::Covers::Start(rom_paths);
                // Los titulos de los iconos tambien van en el idioma elegido.
                menu.RefreshTitles();
            }
            select_hold_begin = 0;
            previous_buttons = ~0u;
            continue;
        }

        if (pressed & SCE_CTRL_CIRCLE) {
            running = false;
        }
        if (menu.Update(pad.buttons, pressed) && menu.Count() > 0) {
            // La textura de iconos vuelve a la CDRAM y la red se apaga: el juego
            // necesita la memoria y la CPU.
            VitaFrontend::Covers::Stop();
            menu.ReleaseTextures();
            RunGame(font, menu.SelectedPath());
            menu.ForgetScreenshot(menu.SelectedPath());
            VitaFrontend::Covers::Start(rom_paths);
            // Al volver del juego hay que reiniciar el estado de botones para
            // que la combinacion de salida no se lea aqui como una pulsacion.
            previous_buttons = ~0u;
            start_hold_begin = 0;
            select_hold_begin = 0;
            vita2d_set_clear_color(kColorBackground);
            continue;
        }

        menu.Draw(settings_hold, language_hold);
    }
    VitaFrontend::Covers::Stop();

    VitaFrontend::Input::Shutdown();
    Common::Log::Stop();
}

} // Anonymous namespace

int main(int argc, char** argv) {
    g_argc = argc;
    g_argv = argv;
    // El Cortex-A9 de la Vita va a 333 MHz por defecto. Emular un ARM11 con un
    // interprete es lo mas caro que puede hacer esta consola, asi que se sube
    // todo al maximo que permite el sistema.
    //
    // En una compilacion "safe" (la de por defecto) estas llamadas pueden ser
    // rechazadas y devolver error. No pasa nada: se sigue a la velocidad
    // normal, solo que mas lento. Arrancar importa mas que el overclock.
    // Marcas de grano fino: si el proceso muere, la ultima linea de crash.txt
    // dice exactamente hasta donde llego.
    // La VERSION, lo primero de todo.
    //
    // Sin esto un crash.txt no se puede atribuir a una compilacion concreta, y
    // un volcado de memoria no sirve de nada: las direcciones solo casan con el
    // azahar.elf de SU build, y ese fichero se sobrescribe en cada compilacion.
    // Ha pasado dos veces: llego un volcado, las direcciones resolvieron a
    // funciones sin ninguna relacion entre si, y la unica conclusion posible fue
    // "este ELF no es el de esa build".
    //
    // build.sh guarda ahora una copia del ELF por version. Con esta linea se
    // sabe cual hay que usar.
    //
    // UN crash.txt POR SESION (0.1.7.3). El fichero crecia sin fin (6 MB con
    // sesiones desde 0.1.4), y el que llegaba para analizar era a menudo el de
    // una version vieja sin que se notara. Ahora cada arranque empieza uno
    // nuevo y el de la sesion anterior queda en crash_anterior.txt.
    sceIoMkdir("ux0:/data", 0777);
    sceIoMkdir("ux0:/data/azahar", 0777);
    StartKeepAwake();
    sceIoRemove("ux0:/data/azahar/crash_anterior.txt");
    sceIoRename(kCrashLog, "ux0:/data/azahar/crash_anterior.txt");
    // Lo mismo con los shaders que no compilan: solo los de esta sesion.
    sceIoRemove("ux0:/data/azahar/cg_error_anterior.txt");
    sceIoRename("ux0:/data/azahar/cg_error.txt", "ux0:/data/azahar/cg_error_anterior.txt");
    WriteCrashLog("version", kVersion);
    WriteCrashLog("main", "entrando (inicializadores estaticos superados)");

    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    // Leer de vuelta lo que la consola ha concedido de verdad.
    //
    // Estas llamadas pueden ser rechazadas y nadie miraba el resultado. Todas
    // las cuentas de "ciclos por pixel" de la sesion daban 444 MHz por hecho
    // sin ninguna prueba de que lo fuera. Si sale 333, esas cuentas iban un 33%
    // infladas y ademas hay un tercio de rendimiento ahi parado.
    // FPU en modo rapido ANTES de emular nada. Ver VitaEnableFastFloatMode.
    Common::VitaEnableFastFloatMode();

    Common::g_arm_clock_mhz = scePowerGetArmClockFrequency();
    {
        char buffer[96];
        const auto result = fmt::format_to_n(
            buffer, sizeof(buffer) - 1, "arm {} MHz, bus {} MHz, gpu {} MHz (pedidos 444/222/222)",
            Common::g_arm_clock_mhz, scePowerGetBusClockFrequency(),
            scePowerGetGpuClockFrequency());
        *result.out = 0;
        WriteCrashLog("relojes", buffer);
    }
    {
        // Lo que cuesta leer el reloj (0.2.1.0): los desgloses lo leen miles de
        // veces por fotograma, y en la Vita es una llamada al kernel.
        const unsigned long long begin = Common::VitaMicros();
        for (int i = 0; i < 1000; i++) {
            (void)Common::VitaMicros();
        }
        const unsigned long long elapsed = Common::VitaMicros() - begin;
        char buffer[64];
        const auto result =
            fmt::format_to_n(buffer, sizeof(buffer) - 1, "1000 lecturas en {} us", elapsed);
        *result.out = 0;
        WriteCrashLog("reloj", buffer);
    }

    // El presupuesto de memoria, anotado ANTES de reservar nada.
    //
    // Va a crash.txt ademas de a la pantalla porque el escenario que importa es
    // justo el que no se puede fotografiar: si la aplicacion muere despues, esta
    // linea dice con cuanta memoria habia arrancado. Y si ATTRIBUTE2=12 dejara
    // de concederse en una actualizacion de firmware, aqui se veria de inmediato
    // en vez de aparecer como un fallo de reserva sin relacion aparente.
    {
        char buffer[128];
        const auto result =
            fmt::format_to_n(buffer, sizeof(buffer) - 1,
                             "usuario {} MB totales / {} MB libres, heap {} MB, CDRAM {} MB, "
                             "modo ampliado {}",
                             TotalUserMemoryKB() / 1024, FreeMemoryKB() / 1024,
                             static_cast<unsigned int>(AZAHAR_HEAP_MB), FreeCdramKB() / 1024,
                             ExtendedMemoryGranted() ? "SI" : "no");
        *result.out = 0;
        WriteCrashLog("memoria", buffer);
    }

    // El hilo principal es el que emula la CPU del 3DS, y esa es la parte de
    // todo esto que mas machaca la cache: la cache de traduccion son 24 MB por
    // los que va saltando. Atarlo a un nucleo fijo evita que el planificador lo
    // mueva y tire a la basura lo que ya tenia caliente en L1/L2.
    //
    // No compite con los hilos del rasterizador aunque comparta nucleo con uno
    // de ellos: mientras se rasteriza, este hilo esta bloqueado esperandolos.
    Common::VitaPinThreadToUserCore(0, "emulacion");

    // Antes de nada util: averiguar si esta consola permite JIT. El resultado va
    // a crash.txt bajo "jit" y no afecta a nada mas -- solo informa. Ver
    // vita_jit_probe.cpp para por que hace falta saberlo antes de escribir un
    // recompilador y no despues.
    VitaFrontend::ProbeJitSupport();
    // Antes que el JIT ARM, que se queda con lo que quede (ver ReserveCodeMemory).
    Pica::Shader::Fast::ReserveCodeMemory();

    vita2d_init();
    WriteCrashLog("main", "vita2d_init hecho");

    vita2d_set_clear_color(kColorBackground);
    // Latina + japonesa (0.3.3.9): la de por defecto no trae los acentos de
    // los menus traducidos. Ver I18n::LoadFont.
    vita2d_pgf* font = I18n::LoadFont();
    WriteCrashLog("main", "fuente cargada");
    g_font = font;
    g_main_thread_id = sceKernelGetThreadId();

    // Cubre las excepciones de cualquier hilo; el try de abajo solo cubre este.
    std::set_terminate(OnTerminate);

    // Una excepcion que escape hasta aqui acaba en abort(), y la consola solo
    // dice "la aplicacion ha fallado" sin mas pistas. Capturarla y pintarla en
    // pantalla convierte un crash mudo en un diagnostico legible.
    try {
        Run(font);
    } catch (const std::exception& e) {
        DrawFatalError(font, T(I18n::kErrFatalStart), e.what());
    } catch (...) {
        DrawFatalError(font, T(I18n::kErrFatalStart), T(I18n::kErrUnknownException));
    }

    vita2d_free_pgf(font);
    vita2d_fini();
    sceKernelExitProcess(0);
    return 0;
}
