// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <atomic>
#include <cmath>
#include <memory>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include "citra_vita/vita_input.h"
#include "common/param_package.h"
#include "common/settings.h"
#include "core/frontend/input.h"

namespace VitaFrontend::Input {

namespace {

constexpr char kEngineName[] = "vita";

/// Indices de los ejes analogicos, tal como se codifican en el ParamPackage.
enum class AxisId {
    LeftStick = 0,
    RightStick = 1,
};

/// El stick de la Vita reposa en 128 y recorre 0..255.
constexpr int kStickCenter = 128;

/**
 * Zona muerta del stick.
 *
 * Los sticks de la Vita se descentran con el uso y rara vez vuelven a 128
 * exacto. Sin zona muerta el personaje del juego camina solo.
 */
constexpr float kDeadzone = 0.15f;

/**
 * UNA LECTURA POR ACTUALIZACION (0.3.2.1). Cada boton y cada stick leia el
 * mando por su cuenta: 13 sceCtrlPeekBufferPositive en cada actualizacion del
 * HID del 3DS, que son 234 por segundo emulado. crash.txt de 0.3.2.0, Pokemon
 * Sol: "HID::UpdatePadCallback" 14-47 ms por segundo en el hilo de emulacion
 * (0,13-0,18 ms por llamada). Las 13 llegan seguidas, en microsegundos: con la
 * lectura de hace menos de kPadFreshUs, el estado es el mismo. Solo las llama
 * el hilo de emulacion.
 */
/// Ver SetPilotButtons.
std::atomic<unsigned int> g_pilot_buttons{0};

/**
 * START Y SELECT SEGUIDOS ABREN EL MENU DE LA PARTIDA (0.4.6.1). Cada uno por
 * separado es un boton del 3DS, asi que hay que saber si viene el otro antes de
 * darselo al juego: la primera espera kComboHoldUs, y si llega la segunda en ese
 * tiempo el juego no ve ninguna. Pasado ese tiempo el juego recibe la primera
 * (estirada kComboTapUs si ya se habia soltado, para no perder un toque corto),
 * y la segunda aun abre el menu hasta kComboWindowUs: mejor un START de mas para
 * el juego que no abrir el menu a quien pulsa con calma.
 */
constexpr unsigned int kComboButtons = SCE_CTRL_START | SCE_CTRL_SELECT;
constexpr SceUInt64 kComboHoldUs = 350 * 1000;
constexpr SceUInt64 kComboTapUs = 100 * 1000;
constexpr SceUInt64 kComboWindowUs = 600 * 1000;

std::atomic<bool> g_game_menu_request{false};
/// Botones que el juego no ve hasta que se suelten. Ver SwallowButtons.
std::atomic<unsigned int> g_swallowed{0};

unsigned int FilterGameMenuCombo(unsigned int buttons, SceUInt64 now) {
    static unsigned int previous = 0;
    static unsigned int first = 0;
    static SceUInt64 first_at = 0;

    unsigned int swallowed = g_swallowed.load(std::memory_order_relaxed);
    if (swallowed != 0) {
        const unsigned int still = swallowed & buttons;
        if (still != swallowed) {
            g_swallowed.store(still, std::memory_order_relaxed);
        }
        swallowed = still;
        buttons &= ~still;
    }

    const unsigned int combo = buttons & kComboButtons;
    const unsigned int fresh = combo & ~previous;
    previous = combo;
    if (first != 0 && now - first_at > kComboWindowUs) {
        first = 0;
    }
    if (fresh != 0) {
        if (fresh == kComboButtons || (first != 0 && (fresh & ~first) != 0)) {
            first = 0;
            previous = 0;
            g_swallowed.store(swallowed | kComboButtons, std::memory_order_relaxed);
            g_game_menu_request.store(true, std::memory_order_relaxed);
            return buttons & ~kComboButtons;
        }
        first = fresh;
        first_at = now;
    }
    if (first == 0) {
        return buttons;
    }
    const SceUInt64 age = now - first_at;
    if (age < kComboHoldUs) {
        return buttons & ~first;
    }
    return age < kComboHoldUs + kComboTapUs ? buttons | first : buttons;
}

SceCtrlData ReadPad() {
    constexpr SceUInt64 kPadFreshUs = 2000;
    static SceCtrlData cached{};
    static SceUInt64 cached_at = 0;
    const SceUInt64 now = sceKernelGetProcessTimeWide();
    if (cached_at == 0 || now - cached_at >= kPadFreshUs) {
        sceCtrlPeekBufferPositive(0, &cached, 1);
        cached_at = now;
        const unsigned int pilot = g_pilot_buttons.load(std::memory_order_relaxed);
        cached.buttons = FilterGameMenuCombo(cached.buttons | (pilot & ~kPilotStickMask), now);
        // El stick del piloto (0.3.9.3): al tope en la direccion pedida.
        if ((pilot & kPilotStickMask) != 0) {
            cached.lx = (pilot & kPilotStickRight) ? 255 : (pilot & kPilotStickLeft) ? 0 : 128;
            cached.ly = (pilot & kPilotStickDown) ? 255 : (pilot & kPilotStickUp) ? 0 : 128;
        }
    }
    return cached;
}

class VitaButton final : public ::Input::ButtonDevice {
public:
    explicit VitaButton(unsigned int mask_) : mask(mask_) {}

    bool GetStatus() const override {
        return (ReadPad().buttons & mask) != 0;
    }

private:
    unsigned int mask;
};

class VitaButtonFactory final : public ::Input::Factory<::Input::ButtonDevice> {
public:
    std::unique_ptr<::Input::ButtonDevice> Create(const Common::ParamPackage& params) override {
        return std::make_unique<VitaButton>(
            static_cast<unsigned int>(params.Get("button", 0)));
    }
};

class VitaAnalog final : public ::Input::AnalogDevice {
public:
    explicit VitaAnalog(AxisId axis_) : axis(axis_) {}

    std::tuple<float, float> GetStatus() const override {
        const SceCtrlData pad = ReadPad();
        const unsigned char raw_x = axis == AxisId::LeftStick ? pad.lx : pad.rx;
        const unsigned char raw_y = axis == AxisId::LeftStick ? pad.ly : pad.ry;

        float x = (static_cast<int>(raw_x) - kStickCenter) / 127.0f;
        // El 3DS considera positivo hacia arriba; la Vita, hacia abajo.
        float y = -(static_cast<int>(raw_y) - kStickCenter) / 127.0f;

        const float magnitude = std::sqrt(x * x + y * y);
        if (magnitude < kDeadzone) {
            return std::make_tuple(0.0f, 0.0f);
        }
        // Reescalar para que el recorrido util siga llegando a 1.0 pese a la
        // zona muerta; si no, el stick nunca alcanzaria el maximo.
        const float scaled = std::min((magnitude - kDeadzone) / (1.0f - kDeadzone), 1.0f);
        x = x / magnitude * scaled;
        y = y / magnitude * scaled;

        return std::make_tuple(x, y);
    }

private:
    AxisId axis;
};

class VitaAnalogFactory final : public ::Input::Factory<::Input::AnalogDevice> {
public:
    std::unique_ptr<::Input::AnalogDevice> Create(const Common::ParamPackage& params) override {
        return std::make_unique<VitaAnalog>(static_cast<AxisId>(params.Get("axis", 0)));
    }
};

/**
 * Movimiento: la Vita tiene giroscopo y acelerometro, pero pedirlos obliga a
 * mantener despierto el servicio de sensores, que cuesta CPU. Con el emulador
 * ya al limite no compensa, asi que se devuelve un 3DS quieto y boca arriba,
 * que es lo que esperan los juegos que no usan movimiento.
 */
class VitaMotion final : public ::Input::MotionDevice {
public:
    std::tuple<Common::Vec3<float>, Common::Vec3<float>> GetStatus() const override {
        return std::make_tuple(Common::Vec3<float>{0.0f, 0.0f, -1.0f},
                               Common::Vec3<float>{0.0f, 0.0f, 0.0f});
    }
};

class VitaMotionFactory final : public ::Input::Factory<::Input::MotionDevice> {
public:
    std::unique_ptr<::Input::MotionDevice> Create(const Common::ParamPackage& params) override {
        return std::make_unique<VitaMotion>();
    }
};

std::string ButtonParam(unsigned int mask) {
    Common::ParamPackage param;
    param.Set("engine", kEngineName);
    param.Set("button", static_cast<int>(mask));
    return param.Serialize();
}

std::string AnalogParam(AxisId axis) {
    Common::ParamPackage param;
    param.Set("engine", kEngineName);
    param.Set("axis", static_cast<int>(axis));
    return param.Serialize();
}

} // Anonymous namespace

void Init() {
    using namespace ::Input;
    RegisterFactory<ButtonDevice>(kEngineName, std::make_shared<VitaButtonFactory>());
    RegisterFactory<AnalogDevice>(kEngineName, std::make_shared<VitaAnalogFactory>());
    RegisterFactory<MotionDevice>(kEngineName, std::make_shared<VitaMotionFactory>());
}

void Shutdown() {
    using namespace ::Input;
    UnregisterFactory<ButtonDevice>(kEngineName);
    UnregisterFactory<AnalogDevice>(kEngineName);
    UnregisterFactory<MotionDevice>(kEngineName);
}

void SetPilotButtons(unsigned int buttons) {
    g_pilot_buttons.store(buttons, std::memory_order_relaxed);
}

bool TakeGameMenuRequest() {
    return g_game_menu_request.exchange(false, std::memory_order_relaxed);
}

void SwallowButtons(unsigned int buttons) {
    g_swallowed.store(buttons, std::memory_order_relaxed);
}

/// Ver SetPilotTouch: x en los 16 bits altos, y en los bajos; -1 = sin toque.
std::atomic<int> g_pilot_touch{-1};

void SetPilotTouch(int x, int y) {
    g_pilot_touch.store(x < 0 || y < 0 ? -1 : ((x & 0xFFFF) << 16) | (y & 0xFFFF),
                        std::memory_order_relaxed);
}

bool PilotTouch(int& x, int& y) {
    const int packed = g_pilot_touch.load(std::memory_order_relaxed);
    if (packed < 0) {
        return false;
    }
    x = (packed >> 16) & 0xFFFF;
    y = packed & 0xFFFF;
    return true;
}

std::atomic<bool> g_pilot_active{false};

void SetPilotActive(bool active) {
    g_pilot_active.store(active, std::memory_order_relaxed);
}

bool PilotActive() {
    return g_pilot_active.load(std::memory_order_relaxed);
}

unsigned int PilotButtons() {
    return g_pilot_buttons.load(std::memory_order_relaxed);
}

void ApplyDefaultMapping() {
    auto& profile = Settings::values.current_input_profile;

    // La cruz de botones se mapea por posicion, no por nombre: el boton de
    // confirmar del 3DS (A) esta a la derecha, igual que el circulo de la Vita.
    profile.buttons[Settings::NativeButton::A] = ButtonParam(SCE_CTRL_CIRCLE);
    profile.buttons[Settings::NativeButton::B] = ButtonParam(SCE_CTRL_CROSS);
    profile.buttons[Settings::NativeButton::X] = ButtonParam(SCE_CTRL_TRIANGLE);
    profile.buttons[Settings::NativeButton::Y] = ButtonParam(SCE_CTRL_SQUARE);

    profile.buttons[Settings::NativeButton::Up] = ButtonParam(SCE_CTRL_UP);
    profile.buttons[Settings::NativeButton::Down] = ButtonParam(SCE_CTRL_DOWN);
    profile.buttons[Settings::NativeButton::Left] = ButtonParam(SCE_CTRL_LEFT);
    profile.buttons[Settings::NativeButton::Right] = ButtonParam(SCE_CTRL_RIGHT);

    profile.buttons[Settings::NativeButton::L] = ButtonParam(SCE_CTRL_LTRIGGER);
    profile.buttons[Settings::NativeButton::R] = ButtonParam(SCE_CTRL_RTRIGGER);

    profile.buttons[Settings::NativeButton::Start] = ButtonParam(SCE_CTRL_START);
    profile.buttons[Settings::NativeButton::Select] = ButtonParam(SCE_CTRL_SELECT);

    // A la Vita se le acaban los botones: ZL/ZR del New 3DS se quedan sin
    // asignar en vez de robarle sitio a algo que los juegos si usan.
    profile.buttons[Settings::NativeButton::ZL].clear();
    profile.buttons[Settings::NativeButton::ZR].clear();
    profile.buttons[Settings::NativeButton::Debug].clear();
    profile.buttons[Settings::NativeButton::Gpio14].clear();
    profile.buttons[Settings::NativeButton::Home].clear();
    profile.buttons[Settings::NativeButton::Power].clear();

    profile.analogs[Settings::NativeAnalog::CirclePad] = AnalogParam(AxisId::LeftStick);
    profile.analogs[Settings::NativeAnalog::CStick] = AnalogParam(AxisId::RightStick);

    Common::ParamPackage motion;
    motion.Set("engine", kEngineName);
    profile.motion_device = motion.Serialize();

    /**
     * EL TACTIL (0.3.1.5). EmuWindow_Vita lee el panel y se lo da al estado
     * tactil de EmuWindow, pero el HID del 3DS solo lo consulta a traves del
     * dispositivo que nombra el perfil, y aqui no se nombraba ninguno: vacio es
     * el dispositivo nulo, que nunca esta pulsado. En escritorio lo pone la
     * configuracion ("engine:emu_window", la fabrica que registra EmuWindow).
     */
    Common::ParamPackage touch;
    touch.Set("engine", "emu_window");
    profile.touch_device = touch.Serialize();
}

} // namespace VitaFrontend::Input
