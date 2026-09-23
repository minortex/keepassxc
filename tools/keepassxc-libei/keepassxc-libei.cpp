#include <QDBusAbstractInterface>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusUnixFileDescriptor>
#include <QVariant>

#include <libei.h>
#include <xkbcommon/xkbcommon.h>

#include <dlfcn.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace
{
    using AsyncCall = QDBusPendingCall (*)(const QDBusConnection*, const QDBusMessage&, int);
    using BlockingCall = QDBusMessage (*)(const QDBusConnection*, const QDBusMessage&, QDBus::CallMode, int);

    constexpr auto portalService = "org.freedesktop.portal.Desktop";
    constexpr auto portalPath = "/org/freedesktop/portal/desktop";
    constexpr auto portalInterface = "org.freedesktop.portal.RemoteDesktop";
    constexpr auto keyMethod = "NotifyKeyboardKeysym";

    struct State
    {
        std::mutex mutex;
        struct ei* context = nullptr;
        struct ei_device* device = nullptr;
        QString session;
        uint32_t sequence = 1;
        xkb_keymap* keymap = nullptr;
        int keymapFd = -1;
        void* keymapData = nullptr;
    size_t keymapSize = 0;
    std::chrono::steady_clock::time_point lastInput;
    bool hasInput = false;
};

    State state;

    void log(const char* message)
    {
        if (std::getenv("KEEPASSXC_LIBEI_DEBUG")) {
            std::fprintf(stderr, "keepassxc-libei: %s\n", message);
        }
    }

    void logCall(const QDBusAbstractInterface* interface, const QString& method, size_t argumentCount)
    {
        if (std::getenv("KEEPASSXC_LIBEI_DEBUG")) {
            const auto service = interface->service().toLocal8Bit();
            const auto path = interface->path().toLocal8Bit();
            const auto name = interface->interface().toLocal8Bit();
            const auto member = method.toLocal8Bit();
            std::fprintf(stderr,
                         "keepassxc-libei: QtDBus call service=%s path=%s interface=%s method=%s args=%zu\n",
                         service.constData(),
                         path.constData(),
                         name.constData(),
                         member.constData(),
                         argumentCount);
        }
    }

    __attribute__((constructor)) void loaded()
    {
        if (std::getenv("WAYLAND_DISPLAY") && !std::getenv("QT_QPA_PLATFORM")) {
            setenv("QT_QPA_PLATFORM", "wayland", 0);
            log("selected Qt Wayland platform");
        }
        log("loaded");
    }

    template <typename T> T resolve(const char* symbol)
    {
        return reinterpret_cast<T>(dlsym(RTLD_NEXT, symbol));
    }

    bool isNotify(const QDBusMessage& message)
    {
        return message.type() == QDBusMessage::MethodCallMessage && message.service() == portalService
               && message.interface() == portalInterface && message.member() == keyMethod
               && message.arguments().size() == 4;
    }

    void resetDevice()
    {
        if (state.device) {
            ei_device_stop_emulating(state.device);
            ei_device_unref(state.device);
            state.device = nullptr;
        }
        if (state.keymap) {
            xkb_keymap_unref(state.keymap);
            state.keymap = nullptr;
        }
        if (state.keymapData && state.keymapSize) {
            munmap(state.keymapData, state.keymapSize);
        }
        if (state.keymapFd >= 0) {
            close(state.keymapFd);
        }
        state.keymapData = nullptr;
        state.keymapSize = 0;
        state.keymapFd = -1;
    }

    void reset()
    {
        resetDevice();
        if (state.context) {
            ei_unref(state.context);
            state.context = nullptr;
        }
        state.session.clear();
    }

    bool waitForDevice()
    {
        const int fd = ei_get_fd(state.context);
        if (fd < 0) {
            return false;
        }

        for (int attempt = 0; attempt < 100 && !state.device; ++attempt) {
            pollfd pollFd{fd, POLLIN, 0};
            if (poll(&pollFd, 1, 100) < 0 && errno != EINTR) {
                return false;
            }
            ei_dispatch(state.context);
            while (auto* event = ei_get_event(state.context)) {
                const auto type = ei_event_get_type(event);
                if (type == EI_EVENT_SEAT_ADDED) {
                    if (auto* seat = ei_event_get_seat(event)) {
                        ei_seat_bind_capabilities(seat, EI_DEVICE_CAP_KEYBOARD, nullptr);
                    }
                } else if (type == EI_EVENT_DEVICE_ADDED) {
                    auto* device = ei_event_get_device(event);
                    if (device && ei_device_has_capability(device, EI_DEVICE_CAP_KEYBOARD)) {
                        state.device = ei_device_ref(device);
                        auto* keymap = ei_device_keyboard_get_keymap(state.device);
                        if (keymap) {
                            state.keymapFd = dup(ei_keymap_get_fd(keymap));
                            state.keymapSize = ei_keymap_get_size(keymap);
                            state.keymapData =
                                mmap(nullptr, state.keymapSize, PROT_READ, MAP_PRIVATE, state.keymapFd, 0);
                            if (state.keymapData != MAP_FAILED) {
                                auto* xkbContext = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
                                state.keymap = xkb_keymap_new_from_string(xkbContext,
                                                                          static_cast<const char*>(state.keymapData),
                                                                          XKB_KEYMAP_FORMAT_TEXT_V1,
                                                                          XKB_KEYMAP_COMPILE_NO_FLAGS);
                                xkb_context_unref(xkbContext);
                            } else {
                                state.keymapData = nullptr;
                            }
                        }
                    }
                } else if (type == EI_EVENT_DEVICE_RESUMED && ei_event_get_device(event) == state.device) {
                    ei_device_start_emulating(state.device, state.sequence++);
                } else if (type == EI_EVENT_DISCONNECT) {
                    ei_event_unref(event);
                    return false;
                }
                ei_event_unref(event);
            }
        }
        return state.device != nullptr;
    }

bool connectEis(const QDBusConnection& connection, const QString& session)
{
    auto call = resolve<BlockingCall>("_ZNK15QDBusConnection4callERK12QDBusMessageN5QDBus8CallModeEi");
    if (!call) {
        log("QtDBus call symbol is unavailable");
        return false;
    }

    auto request = QDBusMessage::createMethodCall(QString::fromLatin1(portalService),
                                                  QString::fromLatin1(portalPath),
                                                  QString::fromLatin1(portalInterface),
                                                  QStringLiteral("ConnectToEIS"));
    request << QDBusObjectPath(session) << QVariantMap{};
    const auto reply = call(&connection, request, QDBus::Block, 10000);
        if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty()) {
            if (reply.type() == QDBusMessage::ErrorMessage) {
                const auto errorName = reply.errorName().toLocal8Bit();
                const auto errorMessage = reply.errorMessage().toLocal8Bit();
                std::fprintf(stderr,
                             "keepassxc-libei: ConnectToEIS error name=%s message=%s\n",
                             errorName.constData(),
                             errorMessage.constData());
            } else {
                log("ConnectToEIS returned an empty reply");
            }
            log("ConnectToEIS failed");
            return false;
        }

        const auto descriptor = reply.arguments().first().value<QDBusUnixFileDescriptor>();
        if (!descriptor.isValid()) {
            log("ConnectToEIS returned an invalid file descriptor");
            return false;
        }

        state.context = ei_new_sender(nullptr);
        if (!state.context) {
            log("failed to create libei sender context");
            return false;
        }
        ei_configure_name(state.context, "KeePassXC auto-type");
        const int fd = dup(descriptor.fileDescriptor());
        if (fd < 0 || ei_setup_backend_fd(state.context, fd) < 0 || !waitForDevice()) {
            log("failed to initialize libei backend or keyboard device");
            reset();
            return false;
        }
        state.session = session;
    return true;
}

void prepareInputMethod()
{
    const auto result = std::system("fcitx5-remote -c");
    if (result != 0) {
        log("fcitx5-remote -c failed");
    } else {
        log("input method disabled");
    }
}

    uint32_t findKeycode(uint32_t keysym)
    {
        // Modifier keysyms are sent as separate portal events. Keep their
        // evdev codes explicit so that the modifier state is applied before the
        // following shifted key event, independent of keymap layout ordering.
        switch (keysym) {
        case XKB_KEY_Shift_L:
            return 42; // KEY_LEFTSHIFT
        case XKB_KEY_Shift_R:
            return 54; // KEY_RIGHTSHIFT
        case XKB_KEY_Control_L:
            return 29; // KEY_LEFTCTRL
        case XKB_KEY_Control_R:
            return 97; // KEY_RIGHTCTRL
        case XKB_KEY_Alt_L:
            return 56; // KEY_LEFTALT
        case XKB_KEY_Alt_R:
            return 100; // KEY_RIGHTALT
        case XKB_KEY_Meta_L:
            return 125; // KEY_LEFTMETA
        case XKB_KEY_Meta_R:
            return 126; // KEY_RIGHTMETA
        default:
            break;
        }

        if (!state.keymap) {
            return 0;
        }
        const auto min = xkb_keymap_min_keycode(state.keymap);
        const auto max = xkb_keymap_max_keycode(state.keymap);
        const auto layouts = xkb_keymap_num_layouts(state.keymap);
        for (xkb_keycode_t code = min; code <= max; ++code) {
            for (xkb_layout_index_t layout = 0; layout < layouts; ++layout) {
                const auto levels = xkb_keymap_num_levels_for_key(state.keymap, code, layout);
                for (xkb_level_index_t level = 0; level < levels; ++level) {
                    const xkb_keysym_t* syms = nullptr;
                    if (xkb_keymap_key_get_syms_by_level(state.keymap, code, layout, level, &syms) > 0
                        && syms[0] == keysym) {
                        // XKB keycodes are evdev scan codes plus 8. libei's
                        // keyboard API takes the underlying evdev scan code.
                        return code >= 8 ? code - 8 : 0;
                    }
                }
            }
        }
        return 0;
    }

    uint32_t modifierKeycode(uint32_t keysym)
    {
        switch (keysym) {
        case XKB_KEY_Shift_L:
            return 42;
        case XKB_KEY_Shift_R:
            return 54;
        case XKB_KEY_Control_L:
            return 29;
        case XKB_KEY_Control_R:
            return 97;
        case XKB_KEY_Alt_L:
            return 56;
        case XKB_KEY_Alt_R:
            return 100;
        case XKB_KEY_Meta_L:
            return 125;
        case XKB_KEY_Meta_R:
            return 126;
        default:
            return 0;
        }
    }

    std::vector<uint32_t> findRequiredModifiers(uint32_t keysym)
    {
        std::vector<uint32_t> result;
        if (!state.keymap) {
            return result;
        }

        const auto min = xkb_keymap_min_keycode(state.keymap);
        const auto max = xkb_keymap_max_keycode(state.keymap);
        const auto layouts = xkb_keymap_num_layouts(state.keymap);

        for (xkb_keycode_t code = min; code <= max; ++code) {
            for (xkb_layout_index_t layout = 0; layout < layouts; ++layout) {
                const auto levels = xkb_keymap_num_levels_for_key(state.keymap, code, layout);
                for (xkb_level_index_t level = 0; level < levels; ++level) {
                    const xkb_keysym_t* syms = nullptr;
                    if (xkb_keymap_key_get_syms_by_level(state.keymap, code, layout, level, &syms) <= 0
                        || syms[0] != keysym) {
                        continue;
                    }

                    xkb_mod_mask_t masks[8]{};
                    const auto count =
                        xkb_keymap_key_get_mods_for_level(state.keymap, code, layout, level, masks, std::size(masks));
                    std::vector<uint32_t> best;
                    size_t bestCount = SIZE_MAX;
                    for (size_t i = 0; i < count; ++i) {
                        std::vector<uint32_t> candidate;
                        bool supported = true;
                        for (xkb_mod_index_t mod = 0; mod < xkb_keymap_num_mods(state.keymap); ++mod) {
                            if (!(masks[i] & (xkb_mod_mask_t(1) << mod))) {
                                continue;
                            }
                            const auto* name = xkb_keymap_mod_get_name(state.keymap, mod);
                            if (!name) {
                                supported = false;
                                break;
                            }
                            if (std::strcmp(name, "Shift") == 0) {
                                candidate.push_back(42);
                            } else if (std::strcmp(name, "Control") == 0) {
                                candidate.push_back(29);
                            } else if (std::strcmp(name, "Mod1") == 0) {
                                candidate.push_back(56);
                            } else if (std::strcmp(name, "Mod4") == 0) {
                                candidate.push_back(125);
                            } else if (std::strcmp(name, "Mod5") == 0) {
                                candidate.push_back(100);
                            } else {
                                supported = false;
                                break;
                            }
                        }
                        if (supported && candidate.size() < bestCount) {
                            bestCount = candidate.size();
                            best = std::move(candidate);
                        }
                    }
                    if (bestCount != SIZE_MAX) {
                        return best;
                    }
                }
            }
        }
        return result;
    }

    bool sendKey(const QDBusConnection& connection, const QDBusMessage& message)
    {
        const auto args = message.arguments();
        const auto sessionObject = args.at(0).value<QDBusObjectPath>();
        const auto session = sessionObject.path().isEmpty() ? args.at(0).toString() : sessionObject.path();
        const auto keysym = args.at(2).toUInt();
        const auto pressed = args.at(3).toUInt() != 0;

        if (session.isEmpty()) {
            log("NotifyKeyboardKeysym has an invalid session path");
            return false;
    }

    std::lock_guard lock(state.mutex);
    unsigned rearmGapMs = 1000;
    if (const auto* value = std::getenv("KEEPASSXC_LIBEI_INPUT_METHOD_REARM_GAP_MS")) {
        char* end = nullptr;
        const auto parsed = std::strtoul(value, &end, 10);
        if (end != value && *end == '\0') {
            rearmGapMs = static_cast<unsigned>(std::min<unsigned long>(parsed, 60000));
        }
    }
    const auto now = std::chrono::steady_clock::now();
    const auto newInput = !state.hasInput
        || std::chrono::duration_cast<std::chrono::milliseconds>(now - state.lastInput).count() >= rearmGapMs;
    if (newInput) {
        prepareInputMethod();
    }
    state.lastInput = now;
    state.hasInput = true;

    if (state.session != session) {
            reset();
            if (!connectEis(connection, session)) {
                return false;
            }
        }
        if (!state.device) {
            log("libei keyboard device is not available");
            return false;
        }

        if (const auto keycode = modifierKeycode(keysym)) {
            ei_device_keyboard_key(state.device, keycode, pressed);
            ei_device_frame(state.device, ei_now(state.context));
            return true;
        }

        const auto keycode = findKeycode(keysym);
        if (!keycode) {
            log("keysym is absent from the portal keymap");
            return false;
        }
        const auto modifiers = findRequiredModifiers(keysym);
        if (pressed) {
            for (const auto modifier : modifiers) {
                ei_device_keyboard_key(state.device, modifier, true);
                ei_device_frame(state.device, ei_now(state.context));
            }
        }

        ei_device_keyboard_key(state.device, keycode, pressed);
        ei_device_frame(state.device, ei_now(state.context));

        if (!pressed) {
            for (auto it = modifiers.rbegin(); it != modifiers.rend(); ++it) {
                ei_device_keyboard_key(state.device, *it, false);
                ei_device_frame(state.device, ei_now(state.context));
            }
        }
        return true;
    }

    bool intercept(const QDBusConnection& connection, const QDBusMessage& message)
    {
        if (!isNotify(message)) {
            return false;
        }

        // Once the EIS backend is selected, never send the same event through the
        // portal's keysym API as a fallback. Doing so can duplicate input or
        // violate the portal's exclusive EIS input flow.
        sendKey(connection, message);
        return true;
    }
} // namespace

QDBusPendingCall QDBusConnection::asyncCall(const QDBusMessage& message, int timeout) const
{
    if (intercept(*this, message)) {
        return QDBusPendingCall::fromCompletedCall(message.createReply());
    }
    static auto real = resolve<AsyncCall>("_ZNK15QDBusConnection9asyncCallERK12QDBusMessagei");
    return real
               ? real(this, message, timeout)
               : QDBusPendingCall::fromError(QDBusError(QDBusError::InternalError, QStringLiteral("hook unavailable")));
}

using AbstractAsyncCall = QDBusPendingCall (*)(QDBusAbstractInterface*, const QString&, const QList<QVariant>&);

QDBusPendingCall QDBusAbstractInterface::asyncCallWithArgumentList(const QString& method, const QList<QVariant>& args)
{
    logCall(this, method, args.size());
    QDBusMessage message = QDBusMessage::createMethodCall(service(), path(), interface(), method);
    for (const auto& arg : args) {
        message << arg;
    }

    if (intercept(connection(), message)) {
        log("consumed NotifyKeyboardKeysym");
        return QDBusPendingCall::fromCompletedCall(message.createReply());
    }

    static auto real = resolve<AbstractAsyncCall>(
        "_ZN22QDBusAbstractInterface25asyncCallWithArgumentListERK7QStringRK5QListI8QVariantE");
    return real
               ? real(this, method, args)
               : QDBusPendingCall::fromError(QDBusError(QDBusError::InternalError, QStringLiteral("hook unavailable")));
}

QDBusMessage QDBusConnection::call(const QDBusMessage& message, QDBus::CallMode mode, int timeout) const
{
    if (intercept(*this, message)) {
        return message.createReply();
    }
    static auto real = resolve<BlockingCall>("_ZNK15QDBusConnection4callERK12QDBusMessageN5QDBus8CallModeEi");
    return real ? real(this, message, mode, timeout) : QDBusMessage::createError(QDBusError::InternalError, {});
}
