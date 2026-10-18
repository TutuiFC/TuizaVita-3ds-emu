// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <string>
#include <vector>

namespace VitaFrontend::Covers {

/**
 * Caratulas de los juegos (0.3.4.1), descargadas de GameTDB
 * (art.gametdb.com/3ds/box/<region>/<ID>.png), la misma base de datos que usan
 * los cargadores de la 3DS. Se buscan por el codigo de producto del ROM
 * ("CTR-P-ECLA" -> ECLA), primero en la region del idioma del menu.
 *
 * Se guardan en ux0:/data/azahar/caratulas/<ID>.png; si GameTDB no tiene
 * ninguna, <ID>.none para no volver a preguntar en cada arranque. Van por HTTP
 * normal: GameTDB lo sirve igual que HTTPS y asi no hacen falta SSL ni los
 * certificados de la consola.
 */

/// El ID de 4 letras del juego, o "" si no se puede leer (3DSX, CIA, cifrado raro).
std::string ReadProductId(const std::string& rom_path);

/// ux0:/data/azahar/caratulas/<ID>.png
std::string CoverPath(const std::string& id);

/**
 * Descarga en otro hilo las caratulas que falten. Si no hay conexion, no hace
 * nada. La red se inicia aqui y se apaga al terminar o en Stop(): durante la
 * partida no queda nada reservado.
 */
void Start(const std::vector<std::string>& rom_paths);

/// Para el hilo (termina la descarga en curso) y apaga la red. Antes de cada juego.
void Stop();

struct Progress {
    bool active;
    int done;
    int total;
};
Progress GetProgress();

} // namespace VitaFrontend::Covers
