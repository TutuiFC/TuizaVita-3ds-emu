// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#ifdef __PSVITA__

#include <string>

namespace Common {

/**
 * Lo ocupado de cada nucleo de usuario y el tiempo de CPU de cada hilo con
 * nombre (los que pasan por VitaPinThreadToUserCore o VitaSetThreadPriority),
 * desde la llamada anterior (0.3.2.0). Solo desde un hilo a la vez.
 */
std::string VitaThreadSummary();

/**
 * Cuanto ha trabajado cada uno de los tres nucleos de usuario desde la llamada
 * anterior, en por cientos (0.4.3.5), para el overlay. Sale de idleClock del
 * sistema: es el uso REAL del nucleo, de cualquier hilo, no solo los nuestros.
 * Con su propio estado, aparte del de VitaThreadSummary. Devuelve false en la
 * primera llamada (no hay con que comparar) o si la consola no contesta.
 */
bool VitaCoreUsage(double percent[3]);

} // namespace Common

#endif
