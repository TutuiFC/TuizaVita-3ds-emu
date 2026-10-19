// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <vita2d.h>

namespace VitaFrontend::I18n {

/**
 * Idioma de los menus (0.3.3.9). Es solo el de la interfaz del port: el que ve
 * el juego es otro ajuste (fila "Idioma de la consola"). Por defecto, ingles.
 * Se cambia manteniendo SELECT en el menu de inicio y se guarda en ajustes.txt
 * como "idioma_menu=" con el numero de esta lista.
 */
enum Language : int {
    kEnglish = 0,
    kSpanish,
    kCatalan,
    kFrench,
    kGerman,
    kItalian,
    kPortuguese,
    kLanguageCount,
};

/// Un identificador por texto. La lista sale de vita_strings.inc.
enum Str : int {
#define TR(id, ...) id,
#include "vita_strings.inc"
#undef TR
    kStrCount,
};

void SetLanguage(int language);
int CurrentLanguage();

/// Nombre de un idioma escrito en ese mismo idioma ("Español", "Deutsch").
const char* NativeName(int language);

/// El texto en el idioma actual.
const char* Tr(Str id);
/// El texto en un idioma concreto (el selector lo usa para la vista previa).
const char* TrIn(int language, Str id);

/// Str + desplazamiento, para las filas de los ajustes (kSetLabel0 + fila).
inline Str Offset(Str base, int offset) {
    return static_cast<Str>(static_cast<int>(base) + offset);
}

/**
 * La fuente de los menus: la latina de la consola para los acentos (la que da
 * vita2d_load_default_pgf no los trae) y la japonesa para todo lo demas, que
 * es lo que sale en los titulos japoneses de los iconos. Si no se puede
 * cargar, la de siempre.
 */
vita2d_pgf* LoadFont();

/// Escribe un texto reduciendo la escala si no cabe en max_width pixeles.
void DrawTextFit(vita2d_pgf* font, int x, int y, unsigned int color, float scale,
                 const char* text, int max_width);

} // namespace VitaFrontend::I18n
