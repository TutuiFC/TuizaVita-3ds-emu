// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

namespace VitaFrontend::Input {

/// Registra las fabricas de dispositivos de entrada de la Vita en Azahar.
void Init();

/// Quita las fabricas registradas por Init().
void Shutdown();

/// Rellena Settings con el mapeo de botones Vita -> 3DS.
void ApplyDefaultMapping();

/// Botones de la Vita que el piloto automatico tiene pulsados (0.3.8.2, ver
/// RunPilotIfAny en main.cpp). Se suman a los del mando en cada lectura.
void SetPilotButtons(unsigned int buttons);
/// Bits del piloto que no son botones sino el stick izquierdo al tope (0.3.9.3).
constexpr unsigned int kPilotStickRight = 0x01000000u;
constexpr unsigned int kPilotStickLeft = 0x02000000u;
constexpr unsigned int kPilotStickUp = 0x04000000u;
constexpr unsigned int kPilotStickDown = 0x08000000u;
constexpr unsigned int kPilotStickMask = 0x0F000000u;
unsigned int PilotButtons();
/// Hay una partida del piloto automatico en marcha (0.3.9.3): las notas de
/// rendimiento no tienen tope, para medir partidas largas.
void SetPilotActive(bool active);
bool PilotActive();
/// Toque del piloto en la pantalla de abajo del 3DS (0-319, 0-239), o -1
/// para soltar (0.3.9.3). Lo lee EmuWindow_Vita::UpdateTouch.
void SetPilotTouch(int x, int y);
bool PilotTouch(int& x, int& y);

} // namespace VitaFrontend::Input
