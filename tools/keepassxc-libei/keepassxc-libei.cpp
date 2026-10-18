#include <QDBusAbstractInterface>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusUnixFileDescriptor>
#include <QVariant>

#include <libei.h>
#include <libnotify/notify.h>
#include <xkbcommon/xkbcommon.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cxxabi.h>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
    using AsyncCall = QDBusPendingCall (*)(const QDBusConnection*, const QDBusMessage&, int);
    using BlockingCall = QDBusMessage (*)(const QDBusConnection*, const QDBusMessage&, QDBus::CallMode, int);

    constexpr auto portalService = "org.freedesktop.portal.Desktop";
    constexpr auto portalPath = "/org/freedesktop/portal/desktop";
    constexpr auto portalInterface = "org.freedesktop.portal.RemoteDesktop";
    constexpr auto keyMethod = "NotifyKeyboardKeysym";

    // Whether the window that will receive the injected text is a native
    // Wayland client or an X11/XWayland client. The two need different
    // delivery: Wayland accepts EIS UTF-8 text, X11 only understands keycodes.
    enum class TargetKind
    {
        Unknown,
        Wayland,
        X11,
    };

    struct State
    {
        std::mutex mutex;
        struct ei* context = nullptr;
        struct ei_device* device = nullptr;
        struct ei_device* textDevice = nullptr;
        bool deviceEmulating = false;
        bool textDeviceEmulating = false;
        TargetKind targetKind = TargetKind::Unknown;
        Display* x11Display = nullptr;
        int x11SpareKeycode = 0;
        QString session;
        uint32_t sequence = 1;
        uint32_t heldModifiers = 0;
        xkb_keymap* keymap = nullptr;
        int keymapFd = -1;
        void* keymapData = nullptr;
        size_t keymapSize = 0;
        std::chrono::steady_clock::time_point lastInput;
        bool hasInput = false;
        bool notified = false;
    };

    State state;

    void log(const char* message)
    {
        if (std::getenv("KEEPASSXC_LIBEI_DEBUG")) {
            std::fprintf(stderr, "keepassxc-libei: %s\n", message);
        }
    }

    class NotificationQueue
    {
    public:
        NotificationQueue()
            : worker([this] { run(); })
        {
        }

        void push(std::string message)
        {
            {
                std::lock_guard lock(mutex);
                messages.push_back(std::move(message));
            }
            ready.notify_one();
        }

    private:
        void run()
        {
            if (!notify_init("KeePassXC")) {
                log("desktop notification initialization failed");
                return;
            }

            while (true) {
                std::string message;
                {
                    std::unique_lock lock(mutex);
                    ready.wait(lock, [this] { return !messages.empty(); });
                    message = std::move(messages.front());
                    messages.pop_front();
                }

                auto* notification = notify_notification_new("KeePassXC", message.c_str(), "keepassxc");
                notify_notification_set_timeout(notification, 1500);
                GError* error = nullptr;
                if (!notify_notification_show(notification, &error)) {
                    log("desktop notification failed");
                    if (error) {
                        g_error_free(error);
                    }
                }
                g_object_unref(notification);
            }
        }

        std::mutex mutex;
        std::condition_variable ready;
        std::deque<std::string> messages;
        std::thread worker;
    };

    void notifyInjected(const char* path, const QString& detail)
    {
        static auto* queue = new NotificationQueue;
        const auto message = QStringLiteral("%1：%2").arg(QString::fromUtf8(path), detail);
        queue->push(message.toStdString());
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

    struct SymbolSearch
    {
        const char* wanted;
        void* address = nullptr;
    };

    int findQtSymbol(struct dl_phdr_info* info, size_t, void* data)
    {
        auto* search = static_cast<SymbolSearch*>(data);
        if (!info->dlpi_name || !std::strstr(info->dlpi_name, "libQt6DBus.so")) {
            return 0;
        }

        const ElfW(Dyn)* dynamic = nullptr;
        for (int i = 0; i < info->dlpi_phnum; ++i) {
            if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) {
                dynamic = reinterpret_cast<const ElfW(Dyn)*>(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
                break;
            }
        }
        if (!dynamic) {
            return 0;
        }

        const char* strings = nullptr;
        const ElfW(Sym)* symbols = nullptr;
        const uint32_t* gnuHash = nullptr;
        size_t symbolCount = 0;
        for (auto entry = dynamic; entry->d_tag != DT_NULL; ++entry) {
            if (entry->d_tag == DT_STRTAB) {
                strings = reinterpret_cast<const char*>(entry->d_un.d_ptr);
            } else if (entry->d_tag == DT_SYMTAB) {
                symbols = reinterpret_cast<const ElfW(Sym)*>(entry->d_un.d_ptr);
            } else if (entry->d_tag == DT_GNU_HASH) {
                gnuHash = reinterpret_cast<const uint32_t*>(entry->d_un.d_ptr);
            }
        }
        if (!strings || !symbols || !gnuHash) {
            return 0;
        }

        // GNU hash layout: nbuckets, symoffset, bloom_size, bloom_shift,
        // bloom filter, buckets, then symbol chains.
        const auto bucketCount = gnuHash[0];
        const auto symbolOffset = gnuHash[1];
        const auto bloomSize = gnuHash[2];
        const auto* buckets = gnuHash + 4 + bloomSize * (sizeof(ElfW(Addr)) / sizeof(uint32_t));
        const auto* chains = buckets + bucketCount;
        uint32_t lastSymbol = symbolOffset;
        for (uint32_t bucket = 0; bucket < bucketCount; ++bucket) {
            lastSymbol = std::max(lastSymbol, buckets[bucket]);
        }
        if (lastSymbol >= symbolOffset) {
            uint32_t symbol = lastSymbol;
            while ((chains[symbol - symbolOffset] & 1u) == 0) {
                ++symbol;
            }
            symbolCount = static_cast<size_t>(symbol) + 1;
        }

        for (size_t i = 0; i < symbolCount; ++i) {
            const auto& symbol = symbols[i];
            if (symbol.st_shndx == SHN_UNDEF || symbol.st_value == 0) {
                continue;
            }
            int status = 0;
            char* demangled = abi::__cxa_demangle(strings + symbol.st_name, nullptr, nullptr, &status);
            const bool match = status == 0 && demangled && std::strcmp(demangled, search->wanted) == 0;
            std::free(demangled);
            if (match) {
                search->address = reinterpret_cast<void*>(info->dlpi_addr + symbol.st_value);
                return 1;
            }
        }
        return 0;
    }

    template <typename T> T resolveQt(const char* demangled)
    {
        SymbolSearch search{demangled};
        dl_iterate_phdr(findQtSymbol, &search);
        return reinterpret_cast<T>(search.address);
    }

    bool isNotify(const QDBusMessage& message)
    {
        return message.type() == QDBusMessage::MethodCallMessage && message.service() == portalService
               && message.interface() == portalInterface && message.member() == keyMethod
               && message.arguments().size() == 4;
    }

    void resetDevice()
    {
        const bool sameDevice = state.device && state.device == state.textDevice;
        if (state.device) {
            if (state.deviceEmulating) {
                ei_device_stop_emulating(state.device);
            }
            ei_device_unref(state.device);
            state.device = nullptr;
        }
        if (state.textDevice) {
            if (state.textDeviceEmulating && !sameDevice) {
                ei_device_stop_emulating(state.textDevice);
            }
            ei_device_unref(state.textDevice);
            state.textDevice = nullptr;
        }
        state.deviceEmulating = false;
        state.textDeviceEmulating = false;
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
        state.heldModifiers = 0;
        state.targetKind = TargetKind::Unknown;
        if (state.x11Display) {
            XCloseDisplay(state.x11Display);
            state.x11Display = nullptr;
        }
        state.x11SpareKeycode = 0;
    }

    bool waitForDevice()
    {
        const int fd = ei_get_fd(state.context);
        if (fd < 0) {
            return false;
        }

        for (int attempt = 0; attempt < 100; ++attempt) {
            const bool keyboardReady = state.device && state.deviceEmulating;
            const bool textReady = !state.textDevice || state.textDeviceEmulating;
            if (keyboardReady && state.textDevice && textReady) {
                break;
            }

            pollfd pollFd{fd, POLLIN, 0};
            const int pollResult = poll(&pollFd, 1, 100);
            if (pollResult < 0 && errno != EINTR) {
                return false;
            }
            ei_dispatch(state.context);
            while (auto* event = ei_get_event(state.context)) {
                const auto type = ei_event_get_type(event);
                if (std::getenv("KEEPASSXC_LIBEI_DEBUG")) {
                    std::fprintf(stderr, "keepassxc-libei: EIS event %s\n", ei_event_type_to_string(type));
                }
                if (type == EI_EVENT_SEAT_ADDED) {
                    if (auto* seat = ei_event_get_seat(event)) {
                        if (std::getenv("KEEPASSXC_LIBEI_DEBUG")) {
                            std::fprintf(stderr,
                                         "keepassxc-libei: EIS seat capabilities keyboard=%d text=%d\n",
                                         ei_seat_has_capability(seat, EI_DEVICE_CAP_KEYBOARD),
                                         ei_seat_has_capability(seat, EI_DEVICE_CAP_TEXT));
                        }
                        ei_seat_bind_capabilities(seat, EI_DEVICE_CAP_KEYBOARD, EI_DEVICE_CAP_TEXT, nullptr);
                    }
                } else if (type == EI_EVENT_DEVICE_ADDED) {
                    auto* device = ei_event_get_device(event);
                    if (device && std::getenv("KEEPASSXC_LIBEI_DEBUG")) {
                        std::fprintf(stderr,
                                     "keepassxc-libei: EIS device keyboard=%d text=%d\n",
                                     ei_device_has_capability(device, EI_DEVICE_CAP_KEYBOARD),
                                     ei_device_has_capability(device, EI_DEVICE_CAP_TEXT));
                    }
                    if (device && ei_device_has_capability(device, EI_DEVICE_CAP_TEXT) && !state.textDevice) {
                        state.textDevice = ei_device_ref(device);
                        log("compositor provided an EIS text device");
                    }
                    if (device && ei_device_has_capability(device, EI_DEVICE_CAP_KEYBOARD) && !state.device) {
                        state.device = ei_device_ref(device);
                        log("compositor provided an EIS keyboard device");
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
                } else if (type == EI_EVENT_DEVICE_RESUMED) {
                    auto* resumed = ei_event_get_device(event);
                    bool started = false;
                    if (resumed == state.device && !state.deviceEmulating) {
                        ei_device_start_emulating(resumed, state.sequence++);
                        state.deviceEmulating = true;
                        started = true;
                    }
                    if (resumed == state.textDevice && !state.textDeviceEmulating) {
                        if (!started) {
                            ei_device_start_emulating(resumed, state.sequence++);
                        }
                        state.textDeviceEmulating = true;
                    }
                } else if (type == EI_EVENT_DEVICE_PAUSED) {
                    auto* paused = ei_event_get_device(event);
                    if (paused == state.device) {
                        state.deviceEmulating = false;
                    }
                    if (paused == state.textDevice) {
                        state.textDeviceEmulating = false;
                    }
                } else if (type == EI_EVENT_DEVICE_REMOVED) {
                    auto* removed = ei_event_get_device(event);
                    if (removed == state.device) {
                        ei_device_unref(state.device);
                        state.device = nullptr;
                        state.deviceEmulating = false;
                    }
                    if (removed == state.textDevice) {
                        ei_device_unref(state.textDevice);
                        state.textDevice = nullptr;
                        state.textDeviceEmulating = false;
                    }
                } else if (type == EI_EVENT_DISCONNECT) {
                    ei_event_unref(event);
                    return false;
                }
                ei_event_unref(event);
            }
            // The keyboard device is mandatory; some compositors do not yet
            // expose the optional text device. Give the compositor one idle
            // round trip to announce it, then continue with keyboard events.
            if (state.device && state.deviceEmulating && !state.textDevice && pollResult == 0) {
                break;
            }
        }
        // Text devices are optional in older compositors. Keep the keyboard
        // path usable when KWin does not advertise EI_DEVICE_CAP_TEXT.
        return state.device && state.deviceEmulating;
    }

bool connectEis(const QDBusConnection& connection, const QString& session)
{
    auto call = resolveQt<BlockingCall>("QDBusConnection::call(QDBusMessage const&, QDBus::CallMode, int) const");
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

        // ConnectToEIS is one-shot per portal session, even if libei setup
        // fails afterward. Remember the session so callers do not reconnect.
        state.session = session;
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
        if (fd < 0) {
            log("failed to duplicate the EIS file descriptor");
            reset();
            state.session = session;
            return false;
        }
        const int setupResult = ei_setup_backend_fd(state.context, fd);
        if (setupResult < 0) {
            log("ei_setup_backend_fd failed");
            reset();
            state.session = session;
            return false;
        }
        if (!waitForDevice()) {
            log("no EIS keyboard device arrived before timeout or disconnect");
            reset();
            state.session = session;
            return false;
        }
    return true;
}

void prepareInputMethod()
{
    if (std::getenv("KEEPASSXC_LIBEI_KEEP_INPUT_METHOD")) {
        return;
    }
    const auto result = std::system("env -u LD_PRELOAD fcitx5-remote -c");
    if (result != 0) {
        log("fcitx5-remote -c failed");
    } else {
        log("input method disabled");
    }
}

    // XWayland keeps X input focus on a 1x1 dummy window (position -1,-1, no
    // WM_CLASS) whenever a native Wayland surface holds keyboard focus. When an
    // X11 client is focused, XGetInputFocus returns that real window instead.
    // That difference is how we tell the two apart without any compositor API.
    TargetKind detectTargetKind()
    {
        const char* displayName = std::getenv("DISPLAY");
        if (!displayName || !*displayName) {
            return TargetKind::Wayland;
        }

        Display* display = XOpenDisplay(displayName);
        if (!display) {
            log("could not open the X display to detect the target window");
            return TargetKind::Unknown;
        }

        Window focus = None;
        int revert = 0;
        XGetInputFocus(display, &focus, &revert);

        TargetKind kind = TargetKind::Unknown;
        if (focus == None || focus == PointerRoot || focus == InputFocus) {
            kind = TargetKind::Wayland;
        } else {
            XWindowAttributes attributes{};
            const bool hasAttributes = XGetWindowAttributes(display, focus, &attributes) != 0;
            XClassHint classHint{};
            const bool hasClass = XGetClassHint(display, focus, &classHint) != 0;
            if (hasClass) {
                if (classHint.res_name) {
                    XFree(classHint.res_name);
                }
                if (classHint.res_class) {
                    XFree(classHint.res_class);
                }
            }
            if (!hasAttributes) {
                kind = TargetKind::Wayland;
            } else if (hasClass) {
                kind = TargetKind::X11;
            } else if (attributes.width <= 1 && attributes.height <= 1) {
                // The XWayland focus proxy: a tiny unmanaged window.
                kind = TargetKind::Wayland;
            } else {
                kind = TargetKind::X11;
            }
        }

        XCloseDisplay(display);
        return kind;
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

    // X11 clients only receive keyboard events, and the compositor's EIS text
    // path loses anything outside the active keymap (see README). Bypass EIS
    // entirely for X11 targets: temporarily bind a spare keycode to the target
    // keysym, tap it with XTEST, then unbind. Unlike the compositor path the
    // remap is synchronous, so non-ASCII characters arrive intact.
    bool ensureX11Display()
    {
        if (state.x11Display) {
            return true;
        }
        const char* displayName = std::getenv("DISPLAY");
        if (!displayName || !*displayName) {
            return false;
        }
        Display* display = XOpenDisplay(displayName);
        if (!display) {
            log("could not open the X display for XTEST injection");
            return false;
        }

        // Find a keycode with no keysyms bound so remapping it is harmless.
        // Prefer the highest codes: low codes map to reserved evdev codes that
        // XWayland drops. X11 keycodes are evdev codes plus 8, so keep the
        // spare above keycode 8 (evdev 0, KEY_RESERVED).
        int minKeycode = 0;
        int maxKeycode = 0;
        int perKeycode = 0;
        XDisplayKeycodes(display, &minKeycode, &maxKeycode);
        KeySym* mapping = XGetKeyboardMapping(display, minKeycode, maxKeycode - minKeycode + 1, &perKeycode);
        int spare = 0;
        if (mapping) {
            for (int code = maxKeycode; code > 8 && !spare; --code) {
                bool used = false;
                for (int i = 0; i < perKeycode; ++i) {
                    if (mapping[(code - minKeycode) * perKeycode + i] != NoSymbol) {
                        used = true;
                        break;
                    }
                }
                if (!used) {
                    spare = code;
                }
            }
            XFree(mapping);
        }
        if (!spare) {
            log("no spare X11 keycode available for XTEST injection");
            XCloseDisplay(display);
            return false;
        }

        state.x11Display = display;
        state.x11SpareKeycode = spare;
        if (std::getenv("KEEPASSXC_LIBEI_DEBUG")) {
            std::fprintf(stderr, "keepassxc-libei: XTEST spare keycode=%d\n", spare);
        }
        return true;
    }

    bool sendKeyX11(uint32_t keysym, bool pressed)
    {
        if (!ensureX11Display()) {
            return false;
        }
        Display* display = state.x11Display;

        if (const auto modifier = modifierKeycode(keysym)) {
            // X keycodes are evdev codes plus 8; modifierKeycode returns evdev.
            const KeyCode keycode = static_cast<KeyCode>(modifier + 8);
            XTestFakeKeyEvent(display, keycode, pressed ? True : False, CurrentTime);
            XFlush(display);
            uint32_t bit = 0;
            switch (keysym) {
            case XKB_KEY_Control_L:
            case XKB_KEY_Control_R:
                bit = 1;
                break;
            case XKB_KEY_Alt_L:
            case XKB_KEY_Alt_R:
            case XKB_KEY_Meta_L:
            case XKB_KEY_Meta_R:
                bit = 2;
                break;
            case XKB_KEY_Shift_L:
            case XKB_KEY_Shift_R:
                bit = 4;
                break;
            default:
                break;
            }
            if (pressed) {
                state.heldModifiers |= bit;
            } else {
                state.heldModifiers &= ~bit;
            }
            if (!state.notified) {
                state.notified = true;
                notifyInjected("X11 XTEST", QStringLiteral("自动输入已生效"));
            }
            return true;
        }

        // Bind the spare keycode to the target keysym for the duration of the
        // tap. XSync makes the remap visible before the fake key event.
        // The portal delivers a press and a release for every keysym; emit the
        // single tap only on the press so the character is not duplicated.
        if (!pressed) {
            return true;
        }
        const KeySym sym = static_cast<KeySym>(keysym);

        // If the keysym already exists in the keymap, use its real keycode and
        // synthesize Shift for the shifted level. This keeps layout-aware
        // characters (uppercase letters, shifted symbols) correct.
        const KeyCode realKeycode = XKeysymToKeycode(display, sym);
        bool needsShift = false;
        bool foundReal = false;
        if (realKeycode != 0) {
            if (XkbKeycodeToKeysym(display, realKeycode, 0, 0) == sym) {
                foundReal = true;
            } else if (XkbKeycodeToKeysym(display, realKeycode, 0, 1) == sym) {
                foundReal = true;
                needsShift = true;
            }
        }

        if (foundReal) {
            const KeyCode shiftKeycode = needsShift ? XKeysymToKeycode(display, XK_Shift_L) : 0;
            if (shiftKeycode) {
                XTestFakeKeyEvent(display, shiftKeycode, True, CurrentTime);
            }
            XTestFakeKeyEvent(display, realKeycode, True, CurrentTime);
            XTestFakeKeyEvent(display, realKeycode, False, CurrentTime);
            if (shiftKeycode) {
                XTestFakeKeyEvent(display, shiftKeycode, False, CurrentTime);
            }
            XSync(display, False);
            if (std::getenv("KEEPASSXC_LIBEI_DEBUG")) {
                std::fprintf(stderr,
                             "keepassxc-libei: XTEST keysym=0x%x keycode=%d shift=%d\n",
                             keysym,
                             realKeycode,
                             needsShift ? 1 : 0);
            }
            return true;
        }

        KeySym syms[1] = {sym};
        XChangeKeyboardMapping(display, state.x11SpareKeycode, 1, syms, 1);
        XSync(display, False);

        XTestFakeKeyEvent(display, static_cast<KeyCode>(state.x11SpareKeycode), True, CurrentTime);
        XSync(display, False);
        XTestFakeKeyEvent(display, static_cast<KeyCode>(state.x11SpareKeycode), False, CurrentTime);
        XSync(display, False);

        // Give X clients a moment to refresh their cached keymap from the
        // MappingNotify before the spare keycode is reverted. Without this the
        // client may still translate with the previous mapping.
        usleep(2000);
        KeySym none[1] = {NoSymbol};
        XChangeKeyboardMapping(display, state.x11SpareKeycode, 1, none, 1);
        XSync(display, False);

        if (std::getenv("KEEPASSXC_LIBEI_DEBUG")) {
            std::fprintf(stderr, "keepassxc-libei: XTEST keysym=0x%x\n", keysym);
        }
        return true;
    }

    bool sendKey(const QDBusConnection& connection, const QDBusMessage& message, QString& error)
    {
        const auto args = message.arguments();
        const auto sessionObject = args.at(0).value<QDBusObjectPath>();
        const auto session = sessionObject.path().isEmpty() ? args.at(0).toString() : sessionObject.path();
        const auto keysym = args.at(2).toUInt();
        const auto pressed = args.at(3).toUInt() != 0;

        if (std::getenv("KEEPASSXC_LIBEI_DEBUG")) {
            const auto sessionUtf8 = session.toUtf8();
            std::fprintf(stderr,
                         "keepassxc-libei: NotifyKeyboardKeysym session=%s keysym=0x%x pressed=%d\n",
                         sessionUtf8.constData(),
                         keysym,
                         pressed);
        }

        if (session.isEmpty()) {
            error = QStringLiteral("NotifyKeyboardKeysym has an invalid session path");
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
    TargetKind sampledKind = state.targetKind;
    if (newInput) {
        // Sample the focused window once per auto-type sequence. The main
        // window is already hidden by the time the first keysym arrives, so the
        // focus has settled on the target application.
        sampledKind = detectTargetKind();
        state.notified = false;
    }
    state.lastInput = now;
    state.hasInput = true;

        if (std::getenv("KEEPASSXC_LIBEI_DEBUG")) {
            std::fprintf(stderr,
                         "keepassxc-libei: target kind=%s%s\n",
                         sampledKind == TargetKind::Wayland ? "wayland"
                             : sampledKind == TargetKind::X11 ? "x11"
                                                              : "unknown",
                         newInput ? " (sampled)" : " (cached)");
        }

        // X11/XWayland targets bypass EIS entirely: XTEST with a synchronous
        // keycode remap delivers any keysym, which the compositor's EIS text
        // path cannot (see README). The input method still intercepts the keys,
        // so disable it for the sequence.
        if (sampledKind == TargetKind::X11) {
            state.targetKind = sampledKind;
            if (newInput) {
                prepareInputMethod();
            }
            if (!sendKeyX11(keysym, pressed)) {
                error = QStringLiteral("XTEST injection failed for keysym 0x%1").arg(keysym, 0, 16);
                return false;
            }
            if (!state.notified) {
                state.notified = true;
                notifyInjected("X11 XTEST", QStringLiteral("自动输入已生效"));
            }
            return true;
        }

        // A native Wayland client receives EIS UTF-8 text directly, so the
        // input method must stay out of the way.
        if (state.session != session) {
            reset();
            if (!connectEis(connection, session)) {
                error = QStringLiteral("ConnectToEIS failed for portal session %1").arg(session);
                return false;
            }
        }
        if (newInput) {
            state.targetKind = sampledKind;
        }

        if (!state.device || !state.deviceEmulating) {
            error = QStringLiteral("EIS keyboard device is not available for portal session %1").arg(session);
            log("EIS setup already failed for this portal session; cannot retry ConnectToEIS");
            return false;
        }

        if (const auto keycode = modifierKeycode(keysym)) {
            if (std::getenv("KEEPASSXC_LIBEI_DEBUG")) {
                std::fprintf(stderr,
                             "keepassxc-libei: keyboard key keycode=%u pressed=%d source=modifier\n",
                             keycode,
                             pressed);
            }
            ei_device_keyboard_key(state.device, keycode, pressed);
            ei_device_frame(state.device, ei_now(state.context));
            uint32_t bit = 0;
            switch (keysym) {
            case XKB_KEY_Control_L:
            case XKB_KEY_Control_R:
                bit = 1;
                break;
            case XKB_KEY_Alt_L:
            case XKB_KEY_Alt_R:
            case XKB_KEY_Meta_L:
            case XKB_KEY_Meta_R:
                bit = 2;
                break;
            case XKB_KEY_Shift_L:
            case XKB_KEY_Shift_R:
                bit = 4;
                break;
            default:
                break;
            }
            if (pressed) {
                state.heldModifiers |= bit;
            } else {
                state.heldModifiers &= ~bit;
            }
            return true;
        }

        // EIS text devices accept Unicode independently of the active keyboard
        // layout. Use them for printable characters when no shortcut modifier
        // is held; keep key events for shortcuts and non-text keys.
        const auto unicode = xkb_keysym_to_utf32(keysym);
        // X11/XWayland clients cannot consume EIS text: the compositor has to
        // translate it back into keycodes, which drops anything outside the
        // active keymap. Send text only to native Wayland targets.
        if (state.targetKind == TargetKind::Wayland && state.textDevice && state.textDeviceEmulating
            && unicode >= 0x20 && unicode != 0x7f && !(state.heldModifiers & 3)) {
            if (pressed) {
                const char32_t codepoints[] = {static_cast<char32_t>(unicode)};
                const auto utf8 = QString::fromUcs4(codepoints, 1).toUtf8();
                if (std::getenv("KEEPASSXC_LIBEI_DEBUG")) {
                    std::fprintf(stderr,
                                 "keepassxc-libei: text utf8=\"%s\" codepoint=U+%04X\n",
                                 utf8.constData(),
                                 unicode);
                }
                ei_device_text_utf8_with_length(state.textDevice, utf8.constData(), utf8.size());
                ei_device_frame(state.textDevice, ei_now(state.context));
                if (!state.notified) {
                    state.notified = true;
                    notifyInjected("Wayland EIS UTF-8", QStringLiteral("自动输入已生效"));
                }
            }
            return true;
        }

        const auto keycode = findKeycode(keysym);
        if (!keycode) {
            error = QStringLiteral("Keysym 0x%1 is not present in the EIS keyboard keymap").arg(keysym, 0, 16);
            log("keysym is absent from the portal keymap");
            return false;
        }
        const auto modifiers = findRequiredModifiers(keysym);
        if (std::getenv("KEEPASSXC_LIBEI_DEBUG")) {
            std::fprintf(stderr,
                         "keepassxc-libei: keyboard key keycode=%u pressed=%d modifiers=",
                         keycode,
                         pressed);
            for (const auto modifier : modifiers) {
                std::fprintf(stderr, "%u,", modifier);
            }
            std::fprintf(stderr, "\n");
        }
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
        if (!state.notified) {
            state.notified = true;
            notifyInjected("Wayland EIS 键盘", QStringLiteral("自动输入已生效"));
        }
        return true;
    }

    bool intercept(const QDBusConnection& connection, const QDBusMessage& message, QDBusMessage& reply)
    {
        if (!isNotify(message)) {
            return false;
        }

        // Once EIS is selected, consume the portal call even when setup or
        // translation fails. Retrying through the portal can duplicate input
        // or violate the session's EIS connection state.
        QString error;
        if (sendKey(connection, message, error)) {
            reply = message.createReply();
        } else {
            reply = message.createErrorReply(QDBusError::Failed, error);
        }
        return true;
    }
} // namespace

QDBusPendingCall QDBusConnection::asyncCall(const QDBusMessage& message, int timeout) const
{
    QDBusMessage reply;
    if (intercept(*this, message, reply)) {
        return QDBusPendingCall::fromCompletedCall(reply);
    }
    static auto real = resolveQt<AsyncCall>("QDBusConnection::asyncCall(QDBusMessage const&, int) const");
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

    QDBusMessage reply;
    if (intercept(connection(), message, reply)) {
        log("consumed NotifyKeyboardKeysym");
        return QDBusPendingCall::fromCompletedCall(reply);
    }

    static auto real = resolveQt<AbstractAsyncCall>(
        "QDBusAbstractInterface::asyncCallWithArgumentList(QString const&, QList<QVariant> const&)");
    return real
               ? real(this, method, args)
               : QDBusPendingCall::fromError(QDBusError(QDBusError::InternalError, QStringLiteral("hook unavailable")));
}

QDBusMessage QDBusConnection::call(const QDBusMessage& message, QDBus::CallMode mode, int timeout) const
{
    QDBusMessage reply;
    if (intercept(*this, message, reply)) {
        return reply;
    }
    static auto real = resolveQt<BlockingCall>("QDBusConnection::call(QDBusMessage const&, QDBus::CallMode, int) const");
    return real ? real(this, message, mode, timeout) : QDBusMessage::createError(QDBusError::InternalError, {});
}
