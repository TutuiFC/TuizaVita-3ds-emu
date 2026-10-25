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
unsigned int PilotButtons();

} // namespace VitaFrontend::Input
