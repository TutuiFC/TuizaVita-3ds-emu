// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <cstddef>
#include <cstdlib>
#include <mutex>
#include <new>

namespace AudioCore {

/**
 * LOS NODOS DE LAS COLAS DE MUESTRAS, SIN MALLOC (0.3.9.0).
 *
 * StereoBuffer16 es una deque: cada 128 muestras pide un bloque de 512 bytes y
 * lo suelta al consumirlas, y cada fuente del DSP lo hace en cada trama de
 * audio. En la Vita eso es malloc/free de newlib sobre un heap que el
 * compilador de shaders llena de huecos (decenas de miles de reservas por
 * shader), y se nota: en Zelda Ocarina of Time 3D, con 24 fuentes, la misma
 * interpolacion costaba 38-180 us sobre la deque y 10 us sobre un array plano,
 * y empeoraba a lo largo de la partida; la mezcla del DSP llegaba a 6 ms por
 * cada trama de 5 ms y el juego esperaba al audio 33 ms por fotograma.
 *
 * Los bloques de 512 bytes salen de una lista propia y vuelven a ella; nunca se
 * devuelven al sistema (lo que haya llegado a sonar a la vez, unos cientos de
 * KB como mucho). Los de otro tamano (el mapa de la deque) van a malloc. Con
 * cerrojo: la cola la tocan el hilo del DSP y el de emulacion.
 */
class AudioBlockPool {
public:
    static constexpr std::size_t kBlockBytes = 512;

    static void* Take() {
        {
            std::lock_guard lock{Mutex()};
            if (Head() != nullptr) {
                Free* const block = Head();
                Head() = block->next;
                return block;
            }
        }
        void* const block = std::malloc(kBlockBytes);
        if (block == nullptr) {
            throw std::bad_alloc{};
        }
        return block;
    }

    static void Give(void* block) {
        std::lock_guard lock{Mutex()};
        Free* const node = static_cast<Free*>(block);
        node->next = Head();
        Head() = node;
    }

private:
    struct Free {
        Free* next;
    };
    static std::mutex& Mutex() {
        static std::mutex mutex;
        return mutex;
    }
    static Free*& Head() {
        static Free* head = nullptr;
        return head;
    }
};

template <typename T>
struct PoolAllocator {
    using value_type = T;

    PoolAllocator() noexcept = default;
    template <typename U>
    PoolAllocator(const PoolAllocator<U>&) noexcept {}

    T* allocate(std::size_t n) {
        if (n * sizeof(T) == AudioBlockPool::kBlockBytes) {
            return static_cast<T*>(AudioBlockPool::Take());
        }
        void* const p = std::malloc(n * sizeof(T));
        if (p == nullptr) {
            throw std::bad_alloc{};
        }
        return static_cast<T*>(p);
    }

    void deallocate(T* p, std::size_t n) noexcept {
        if (n * sizeof(T) == AudioBlockPool::kBlockBytes) {
            AudioBlockPool::Give(p);
            return;
        }
        std::free(p);
    }

    template <typename U>
    bool operator==(const PoolAllocator<U>&) const noexcept {
        return true;
    }
    template <typename U>
    bool operator!=(const PoolAllocator<U>&) const noexcept {
        return false;
    }
};

} // namespace AudioCore
