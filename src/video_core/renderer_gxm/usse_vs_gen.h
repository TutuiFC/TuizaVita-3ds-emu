// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <cstdint>
#include <vector>

/**
 * GENERADOR PROPIO DE SHADERS DE VERTICES PARA LA USSE (0.4.7.1).
 *
 * POR QUE. Cada shader de vertices nuevo pasa hoy por dos traducciones: del
 * bytecode de la PICA a Cg (cg_vs_shader_gen.cpp) y de Cg a USSE con el
 * compilador de la consola (libshacccg), que tarda de 1,5 a 9,5 s por programa
 * y varios minutos en los de piel grandes. Mientras tanto el lote se salta o va
 * por la CPU: es la rascada de 30-40 s al entrar en una zona nueva. El banco
 * del 09-11 cerro la via de "escribir un Cg que compile antes" (el compilador
 * cuesta 6-8 ms por instruccion emitida, haga lo que haga el texto).
 *
 * QUE HACE ESTO. Traduce el programa de la PICA DIRECTAMENTE a instrucciones
 * de la USSE y escribe el contenedor GXP que GXM espera, sin compilador: unas
 * decenas de microsegundos por programa. El resultado se registra en el
 * parcheador como cualquier otro programa. El de Cg se sigue compilando detras
 * y lo sustituye cuando llega (su codigo esta optimizado; el de aqui no).
 *
 * DE DONDE SALE EL FORMATO. De 1.682 programas de vertices compilados por el
 * propio compilador de Sony (los paquetes de serie) y de los patrones de bits
 * que documenta Vita3K (GPL-2). Solo se emiten formas de instruccion que
 * aparecen en ese corpus, con los mismos bits de control; el porque de cada
 * una va en el .cpp.
 *
 * Este fichero no depende del resto del emulador a proposito: se compila
 * tambien en el PC (azv_herramientas/usse/pc) para probar la traduccion contra
 * un interprete de la USSE y uno de la PICA sin pasar por la consola.
 */
namespace Gxm::Usse {

/// De donde sale un registro de entrada (como VSInputSource de cg_vs_shader_gen.h).
struct VsInput {
    enum Kind : std::uint8_t { None = 0, Array = 1, Default = 2 };
    std::uint8_t kind = None;
    std::uint8_t components = 4;
};

struct VsRequest {
    /// Bytecode y patrones de swizzle de la PICA (4096 palabras cada uno).
    const std::uint32_t* code = nullptr;
    const std::uint32_t* swizzle = nullptr;
    std::uint32_t main_offset = 0;
    /// Registro de salida de la PICA -> atributo de salida (0-6), o -1.
    std::int8_t output_attr[16];
    /// Semantica (VSOutputAttributes::Semantic, 0-23) -> atributo * 4 + componente, o -1
    /// (lo no mapeado vale 1, como en el hardware).
    std::int8_t semantic[24];
    VsInput inputs[16];
    /// Las salidas de la variante fija contra la que se enlazo el de fragmentos.
    bool lit = false;
    bool proj = false;
    bool wbuffer = false;
    /**
     * Solo para el banco de pruebas (banco_usse): la posicion de pantalla sale del
     * registro de entrada 'debug_position_input' tal cual y la que calcula el
     * programa va a la salida del cuaternion (TEXCOORD3). Asi cada vertice de
     * prueba cae en un pixel elegido y lo calculado se lee del color. -1 = no.
     */
    int debug_position_input = -1;
};

struct VsResult {
    std::vector<std::uint8_t> gxp;
    /// Por que no se pudo (literal), o nullptr.
    const char* reason = nullptr;
    std::uint32_t instructions = 0;
    std::uint32_t temp_words = 0;
    std::uint32_t pa_words = 0;
    std::uint32_t sa_words = 0;
    /// Que uniformes declara el programa (para subir solo esos).
    bool uses_ints = false;
    bool uses_bools = false;
    bool uses_defaults = false;
};

/// Palabras (de 32 bits) de cada uniforme dentro del buffer por defecto. Fijas:
/// el que sube los uniformes no tiene que preguntar nada.
constexpr std::uint32_t kUniformFloatWord = 0;     ///< vs_f[96], float4
constexpr std::uint32_t kUniformIntWord = 384;     ///< vs_i[4], float4
constexpr std::uint32_t kUniformBoolWord = 400;    ///< vs_b[16], float
constexpr std::uint32_t kUniformDefaultWord = 416; ///< vs_default[16], float4
constexpr std::uint32_t kUniformWbufWord = 480;    ///< vs_wbuf, float2
constexpr std::uint32_t kUniformWords = 484;

/// Traduce. false (con 'reason') si el programa usa algo que esto no hace: el que
/// llama sigue con el camino de siempre (Cg), que es el de referencia.
bool GenerateVs(const VsRequest& request, VsResult& result);

} // namespace Gxm::Usse
