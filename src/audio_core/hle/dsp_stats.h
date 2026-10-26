// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>

namespace AudioCore::HLE::Stats {

/**
 * EN QUE SE VA EL "dsp" DE crash.txt (0.3.1.4). En New Super Mario Bros. 2 son
 * 8-10 ms por fotograma y en las cuevas de Pokemon Sol 7-13, y la cifra era un
 * total. Lo escribe el hilo de emulacion (el DSP HLE va en su tick de audio) y
 * lo lee y pone a cero el overlay con Take.
 *   ticks          ticks de audio (uno cada 160 muestras)
 *   active         fuentes encendidas, sumadas en cada tick
 *   decode_us      decodificar buffers enteros (al empezar a sonar, al dar la
 *                  vuelta un bucle, al alargarse uno a medio sonar); el ADPCM
 *                  va por tramos desde 0.3.3.2 y solo cuentan los grandes
 *   decoded        muestras decodificadas, todas
 * La mezcla es el resto de Common::FrameStats::dsp_us, quitando el AAC.
 */
inline std::atomic<unsigned long long> ticks{0};
inline std::atomic<unsigned long long> active{0};
inline std::atomic<unsigned long long> decode_us{0};
inline std::atomic<unsigned long long> decoded{0};
/// Lo que tarda la mezcla en el hilo del DSP (0.3.1.5), fuera del de emulacion.
inline std::atomic<unsigned long long> worker_us{0};
/// Lo que el hilo de emulacion espera a que acabe esa mezcla (0.3.2.1).
inline std::atomic<unsigned long long> wait_us{0};

/**
 * EN QUE SE VA LA MEZCLA (0.3.9.0). Zelda Ocarina of Time 3D pasa ~5 ms por
 * cada trama de audio de 5 ms con 24 fuentes. Se cronometra una trama de
 * cada 32 (cada lectura del reloj es una llamada al kernel) y se reparte
 * entre las fases; RunMix lo anota cada 10 s.
 */
inline bool phase_timing = false;
inline unsigned long long phase_refresh_us = 0;
inline unsigned long long phase_decode_us = 0;
inline unsigned long long phase_interp_us = 0;
inline unsigned long long phase_filter_us = 0;
inline unsigned long long phase_samples_in = 0;
/// Llamadas a la interpolacion por la ventana contigua y por la deque (lento),
/// y el mayor ritmo visto (en 1/1000).
inline std::atomic<unsigned int> interp_fast{0};
inline std::atomic<unsigned int> interp_slow{0};
inline std::atomic<unsigned int> interp_max_rate{0};

} // namespace AudioCore::HLE::Stats
