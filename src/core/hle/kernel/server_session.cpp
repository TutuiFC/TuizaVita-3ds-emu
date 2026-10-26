// Copyright 2016-2024 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <tuple>
#include <boost/serialization/shared_ptr.hpp>
#include <boost/serialization/string.hpp>
#include <boost/serialization/vector.hpp>
#include "common/archives.h"
#include "core/hle/kernel/client_port.h"
#include "core/hle/kernel/client_session.h"
#include "core/hle/kernel/hle_ipc.h"
#include "core/hle/kernel/server_session.h"
#include "core/hle/kernel/session.h"
#include "core/hle/kernel/thread.h"
#ifdef __PSVITA__
#include <atomic>
#include <fmt/format.h>
#include "common/vita_diag.h"
#include "core/arm/arm_interface.h"
#include "core/core_timing.h"
#endif

SERIALIZE_EXPORT_IMPL(Kernel::ServerSession)

namespace Kernel {

template <class Archive>
void ServerSession::serialize(Archive& ar, const unsigned int) {
    ar& boost::serialization::base_object<WaitObject>(*this);
    ar & name;
    ar & parent;
    ar & hle_handler;
    ar & pending_requesting_threads;
    ar & currently_handling;
    ar & mapped_buffer_context;
}
SERIALIZE_IMPL(ServerSession)

ServerSession::ServerSession(KernelSystem& kernel) : WaitObject(kernel), kernel(kernel) {}
ServerSession::~ServerSession() {
    // This destructor will be called automatically when the last ServerSession handle is closed by
    // the emulated application.

    // Decrease the port's connection count.
    if (parent->port)
        parent->port->ConnectionClosed();

    // TODO(Subv): Wake up all the ClientSession's waiting threads and set
    // the SendSyncRequest result to 0xC920181A.

    parent->server = nullptr;
}

ResultVal<std::shared_ptr<ServerSession>> ServerSession::Create(KernelSystem& kernel,
                                                                std::string name) {
    auto server_session{std::make_shared<ServerSession>(kernel)};

    server_session->name = std::move(name);
    server_session->parent = nullptr;

    return server_session;
}

bool ServerSession::ShouldWait(const Thread* thread) const {
    // Closed sessions should never wait, an error will be returned from svcReplyAndReceive.
    if (parent->client == nullptr)
        return false;
    // Wait if we have no pending requests, or if we're currently handling a request.
    return pending_requesting_threads.empty() || currently_handling != nullptr;
}

void ServerSession::Acquire(Thread* thread) {
    ASSERT_MSG(!ShouldWait(thread), "object unavailable!");

    // If the client endpoint was closed, don't do anything. This ServerSession is now useless and
    // will linger until its last handle is closed by the running application.
    if (parent->client == nullptr)
        return;

    // We are now handling a request, pop it from the stack.
    ASSERT(!pending_requesting_threads.empty());
    currently_handling = pending_requesting_threads.back();
    pending_requesting_threads.pop_back();
}

#ifdef __PSVITA__
namespace {
/**
 * LA ESPERA DEL IPC SIN DORMIR (0.3.3.1). Una peticion a un servicio HLE duerme
 * al hilo 39 us emulados (ver HandleSyncRequest): dos cambios de contexto, un
 * evento y dos replanificaciones por llamada, y los juegos hacen 120-520 por
 * segundo (SendSyncRequest en crash.txt). Si no hay ningun otro hilo listo en
 * este nucleo y la rodaja no se acaba antes (ningun evento en esos 39 us), lo
 * que pasaria es exactamente esto: el nucleo ocioso hasta que el hilo despierta
 * con la respuesta. Asi que se gasta ese tiempo en el contador y el hilo sigue.
 */
/**
 * LAS FASES DE UNA PETICION HLE (0.4.0.0, diagnostico). En Kirby's Extra Epic
 * Yarn cada SendSyncRequest a gsp::Gpu cuesta ~81 us (ipc ms por s) y el
 * servicio en si ~27 (servicios:). Una de cada ocho peticiones se cronometra
 * por partes para ver donde se va el resto.
 */
constexpr u32 kIpcPhases = 6;
std::array<std::atomic<u64>, kIpcPhases> g_ipc_phase_us{};
std::atomic<u32> g_ipc_phase_samples{0};
u32 g_ipc_phase_tick = 0;

struct IpcPhases {
    bool timed;
    unsigned long long mark = timed ? Common::VitaMicros() : 0;
    void Mark(u32 phase) {
        if (timed) {
            const unsigned long long now = Common::VitaMicros();
            g_ipc_phase_us[phase].fetch_add(now - mark, std::memory_order_relaxed);
            mark = now;
        }
    }
    ~IpcPhases() {
        if (timed) {
            Mark(kIpcPhases - 1);
            g_ipc_phase_samples.fetch_add(1, std::memory_order_relaxed);
        }
    }
};

bool ConsumeIpcDelay(KernelSystem& kernel, u64 delay_ns) {
    if (kernel.current_cpu == nullptr || kernel.GetCurrentThreadManager().HaveReadyThreads()) {
        return false;
    }
    Core::Timing::Timer& timer = kernel.current_cpu->GetTimer();
    const s64 cycles = nsToCycles(delay_ns);
    if (timer.GetDowncount() <= cycles) {
        return false;
    }
    timer.AddTicks(static_cast<u64>(cycles));
    return true;
}
} // Anonymous namespace

std::string TakeIpcPhaseText() {
    const u32 samples = g_ipc_phase_samples.exchange(0, std::memory_order_relaxed);
    std::array<double, kIpcPhases> us{};
    for (u32 i = 0; i < kIpcPhases; i++) {
        us[i] = static_cast<double>(g_ipc_phase_us[i].exchange(0, std::memory_order_relaxed)) /
                (samples != 0 ? samples : 1);
    }
    /* Lo que cuesta leer el reloj, para descontarlo: cada fase lleva una
     * lectura (una llamada al kernel). */
    const unsigned long long clock_begin = Common::VitaMicros();
    for (u32 i = 0; i < 15; i++) {
        (void)Common::VitaMicros();
    }
    const double clock_us = static_cast<double>(Common::VitaMicros() - clock_begin) / 16.0;
    return fmt::format("ipc fases us por llamada: leer {:.1f} contexto {:.1f} servicio {:.1f} "
                       "escribir {:.1f} soltar {:.1f} final {:.1f} ({} muestras, reloj {:.2f} us "
                       "por lectura)",
                       us[0], us[1], us[2], us[3], us[4], us[5], samples, clock_us);
}
#endif

Result ServerSession::HandleSyncRequest(std::shared_ptr<Thread> thread) {
#ifdef __PSVITA__
    IpcPhases phases{hle_handler != nullptr && (++g_ipc_phase_tick & 7u) == 0};
#endif
    // The ServerSession received a sync request, this means that there's new data available
    // from its ClientSession, so wake up any threads that may be waiting on a svcReplyAndReceive or
    // similar.

    // If this ServerSession has an associated HLE handler, forward the request to it.
    if (hle_handler != nullptr) {
        std::array<u32_le, IPC::COMMAND_BUFFER_LENGTH + 2 * IPC::MAX_STATIC_BUFFERS> cmd_buf;
        auto current_process = thread->owner_process.lock();
        ASSERT(current_process);
        kernel.memory.ReadBlock(*current_process, thread->GetCommandBufferAddress(), cmd_buf.data(),
                                cmd_buf.size() * sizeof(u32));
#ifdef __PSVITA__
        phases.Mark(0);
#endif

        auto context =
            std::make_shared<Kernel::HLERequestContext>(kernel, SharedFrom(this), thread);
        context->PopulateFromIncomingCommandBuffer(cmd_buf.data(), current_process);
#ifdef __PSVITA__
        phases.Mark(1);
#endif

        hle_handler->HandleSyncRequest(*context);
#ifdef __PSVITA__
        phases.Mark(2);
#endif

        ASSERT(thread->status == Kernel::ThreadStatus::Running ||
               thread->status == Kernel::ThreadStatus::WaitHleEvent);
        // Only write the response immediately if the thread is still running. If the HLE handler
        // put the thread to sleep then the writing of the command buffer will be deferred to the
        // wakeup callback.
        if (thread->status == Kernel::ThreadStatus::Running) {
            context->WriteToOutgoingCommandBuffer(cmd_buf.data(), *current_process);
            kernel.memory.WriteBlock(*current_process, thread->GetCommandBufferAddress(),
                                     cmd_buf.data(), cmd_buf.size() * sizeof(u32));
        }
#ifdef __PSVITA__
        phases.Mark(3);
#endif
    }
#ifdef __PSVITA__
    phases.Mark(4); // el contexto y el proceso, soltados al cerrar el bloque
#endif

    if (thread->status == ThreadStatus::Running) {
#ifdef __PSVITA__
        if (hle_handler != nullptr && ConsumeIpcDelay(kernel, 39000)) {
            WakeupAllWaitingThreads();
            return ResultSuccess;
        }
#endif
        // Put the thread to sleep until the server replies, it will be awoken in
        // svcReplyAndReceive for LLE servers.
        thread->status = ThreadStatus::WaitIPC;

        if (hle_handler != nullptr) {
            // For HLE services, we put the request threads to sleep for a short duration to
            // simulate IPC overhead, but only if the HLE handler didn't put the thread to sleep for
            // other reasons like an async callback. The IPC overhead is needed to prevent
            // starvation when a thread only does sync requests to HLE services while a
            // lower-priority thread is waiting to run.

            // This delay was approximated in a homebrew application by measuring the average time
            // it takes for svcSendSyncRequest to return when performing the SetLcdForceBlack IPC
            // request to the GSP:GPU service in a n3DS with firmware 11.6. The measured values have
            // a high variance and vary between models.
            static constexpr u64 IPCDelayNanoseconds = 39000;
            thread->WakeAfterDelay(IPCDelayNanoseconds);
        } else {
            // Add the thread to the list of threads that have issued a sync request with this
            // server.
            pending_requesting_threads.push_back(std::move(thread));
        }
    }

    // If this ServerSession does not have an HLE implementation, just wake up the threads waiting
    // on it.
    WakeupAllWaitingThreads();
    return ResultSuccess;
}

KernelSystem::SessionPair KernelSystem::CreateSessionPair(const std::string& name,
                                                          std::shared_ptr<ClientPort> port) {
    auto server_session = ServerSession::Create(*this, name + "_Server").Unwrap();
    auto client_session{std::make_shared<ClientSession>(*this)};
    client_session->name = name + "_Client";

    std::shared_ptr<Session> parent(new Session);
    parent->client = client_session.get();
    parent->server = server_session.get();
    parent->port = port;

    client_session->parent = parent;
    server_session->parent = parent;

    return std::make_pair(std::move(server_session), std::move(client_session));
}

} // namespace Kernel
