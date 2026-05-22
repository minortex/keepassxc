/*
 *  Copyright (C) 2026 KeePassXC Team <team@keepassxc.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 or (at your option)
 *  version 3 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "core/Config.h"
#include "xdp_remotedesktop.h"
#include "xdp_session.h"

#include "AutoTypeWayland.h"
#include "autotype/AutoTypeAction.h"
#include "core/Tools.h"
#include "gui/osutils/nixutils/NixUtils.h"

#include <QEventLoop>
#include <QGuiApplication>
#include <QRandomGenerator>

#include <xkbcommon/xkbcommon.h>

constexpr int EVDEV_KEY_LEFTCTRL = 29;
constexpr int EVDEV_KEY_LEFTSHIFT = 42;
constexpr int EVDEV_KEY_LEFTALT = 56;
constexpr int EVDEV_KEY_LEFTMETA = 125;
constexpr int XKB_EVDEV_OFFSET = 8;

static xkb_keysym_t qtKeyToXkbKeysym(Qt::Key key)
{
    switch (key) {
    case Qt::Key_Tab:
        return XKB_KEY_Tab;
    case Qt::Key_Enter:
        return XKB_KEY_Return;
    case Qt::Key_Space:
        return XKB_KEY_space;
    case Qt::Key_Up:
        return XKB_KEY_Up;
    case Qt::Key_Down:
        return XKB_KEY_Down;
    case Qt::Key_Left:
        return XKB_KEY_Left;
    case Qt::Key_Right:
        return XKB_KEY_Right;
    case Qt::Key_Insert:
        return XKB_KEY_Insert;
    case Qt::Key_Delete:
        return XKB_KEY_Delete;
    case Qt::Key_Home:
        return XKB_KEY_Home;
    case Qt::Key_End:
        return XKB_KEY_End;
    case Qt::Key_PageUp:
        return XKB_KEY_Page_Up;
    case Qt::Key_PageDown:
        return XKB_KEY_Page_Down;
    case Qt::Key_Backspace:
        return XKB_KEY_BackSpace;
    case Qt::Key_Pause:
        return XKB_KEY_Break;
    case Qt::Key_CapsLock:
        return XKB_KEY_Caps_Lock;
    case Qt::Key_Escape:
        return XKB_KEY_Escape;
    case Qt::Key_Help:
        return XKB_KEY_Help;
    case Qt::Key_NumLock:
        return XKB_KEY_Num_Lock;
    case Qt::Key_Print:
        return XKB_KEY_Print;
    case Qt::Key_ScrollLock:
        return XKB_KEY_Scroll_Lock;
    case Qt::Key_Shift:
        return XKB_KEY_Shift_L;
    case Qt::Key_Control:
        return XKB_KEY_Control_L;
    case Qt::Key_Alt:
        return XKB_KEY_Alt_L;
    default:
        if (key >= Qt::Key_F1 && key <= Qt::Key_F16) {
            return XKB_KEY_F1 + (key - Qt::Key_F1);
        } else if (key >= Qt::Key_Space && key <= Qt::Key_AsciiTilde) {
            return key & 0xff;
        } else {
            return XKB_KEY_NoSymbol;
        }
    }
}

Q_GLOBAL_STATIC_WITH_ARGS(OrgFreedesktopPortalRemoteDesktopInterface,
                          s_remoteDesktopInterface,
                          ("org.freedesktop.portal.Desktop",
                           "/org/freedesktop/portal/desktop",
                           QDBusConnection::sessionBus()));

AutoTypePlatformWayland::AutoTypePlatformWayland()
{
}

AutoTypePlatformWayland::~AutoTypePlatformWayland()
{
    if (m_xkbKeymap) {
        xkb_keymap_unref(m_xkbKeymap);
    }
    if (m_xkbContext) {
        xkb_context_unref(m_xkbContext);
    }
}

void AutoTypePlatformWayland::prepareForAutoType()
{
    tryStartSession();
}

void AutoTypePlatformWayland::closeSession()
{
    if (m_remoteDesktopSession != nullptr) {
        m_remoteDesktopSession->Close();
        m_remoteDesktopSession = nullptr;
    }
}

void AutoTypePlatformWayland::waitForSession()
{
    tryStartSession();

    if (m_sessionStarting) {
        QEventLoop loop;
        connect(this, &AutoTypePlatformWayland::sessionReady, &loop, &QEventLoop::quit);
        loop.exec();
    }
}

const QString AutoTypePlatformWayland::errorString() const
{
    return m_error;
}

void AutoTypePlatformWayland::tryStartSession()
{
    if (m_sessionStarting || m_remoteDesktopSession != nullptr) {
        return;
    }

    m_sessionStarting = true;
    m_error = QString();

    // create the remote desktop session
    auto handleToken = request([this](const QVariantMap& results) {
        m_remoteDesktopSession = new OrgFreedesktopPortalSessionInterface("org.freedesktop.portal.Desktop",
                                                                          results["session_handle"].toString(),
                                                                          QDBusConnection::sessionBus(),
                                                                          this);
        connect(m_remoteDesktopSession, &OrgFreedesktopPortalSessionInterface::Closed, this, [this]() {
            if (m_error.isEmpty()) {
                m_error = tr("Session closed");
            }
            m_remoteDesktopSession = nullptr;
            if (m_sessionStarting) {
                m_sessionStarting = false;
                emit sessionReady();
            }
        });
        connect(m_remoteDesktopSession,
                &OrgFreedesktopPortalSessionInterface::Closed,
                m_remoteDesktopSession,
                &QObject::deleteLater);
        selectDevices();
    });
    auto sessionHandleToken = "keepassxc_" + QString::number(QRandomGenerator::system()->generate());
    auto reply = s_remoteDesktopInterface->CreateSession({
        {QLatin1String("session_handle_token"), sessionHandleToken},
        {QLatin1String("handle_token"), handleToken},
    });
    reply.waitForFinished();
    if (reply.isError()) {
        m_error = "Failed to create remote desktop session:" + reply.error().message();
        m_sessionStarting = false;
        emit sessionReady();
    }
}

QString AutoTypePlatformWayland::request(const std::function<void(const QVariantMap&)> handler)
{
    return nixUtils()->portalRequest([this, handler](uint response, const QVariantMap& result) {
        switch (response) {
        case 0:
            handler(result);
            return;
        case 1:
            m_error = tr("User cancelled the interaction");
            break;
        default:
            m_error = tr("User interaction was canceled for unknown reason");
            break;
        }

        m_sessionStarting = false;
        if (m_remoteDesktopSession != nullptr) {
            m_remoteDesktopSession->Close();
            m_remoteDesktopSession = nullptr;
        }
        emit sessionReady();
    });
}

void AutoTypePlatformWayland::selectDevices()
{
    auto handleToken = request([this](const QVariantMap&) { startSession(); });

    auto persistMode = config()->get(Config::AutoTypeDesktopPortalPersistMode).toUInt();
    auto options = QVariantMap{
        {QLatin1String("handle_token"), handleToken},
        {QLatin1String("types"), uint(1)},
        {QLatin1String("persist_mode"), persistMode},
    };

    auto restoreToken = config()->get(Config::AutoTypeDesktopPortalRestoreToken).toString();
    if (persistMode < 2 && !restoreToken.isEmpty()) {
        config()->set(Config::AutoTypeDesktopPortalRestoreToken, "");
    } else if (!restoreToken.isEmpty()) {
        options["restore_token"] = restoreToken;
    } else if (!m_restoreToken.isEmpty()) {
        options["restore_token"] = m_restoreToken;
    }

    auto reply = s_remoteDesktopInterface->SelectDevices(QDBusObjectPath(m_remoteDesktopSession->path()), options);
    reply.waitForFinished();
    if (reply.isError()) {
        m_error = "Failed to select remote desktop devices: " + reply.error().message();
        m_sessionStarting = false;
        if (m_remoteDesktopSession != nullptr) {
            m_remoteDesktopSession->Close();
            m_remoteDesktopSession = nullptr;
        }
        emit sessionReady();
    }
}

void AutoTypePlatformWayland::startSession()
{
    auto handleToken = request([this](const QVariantMap& results) {
        auto persistMode = config()->get(Config::AutoTypeDesktopPortalPersistMode).toUInt();
        if (persistMode >= 2) {
            m_restoreToken = "";
            config()->set(Config::AutoTypeDesktopPortalRestoreToken, results["restore_token"].toString());
        } else if (persistMode == 1) {
            m_restoreToken = results["restore_token"].toString();
        } else {
            m_restoreToken = "";
            config()->set(Config::AutoTypeDesktopPortalRestoreToken, "");
        }
        m_error = QString();
        m_sessionStarting = false;
        emit sessionReady();
    });

    auto reply = s_remoteDesktopInterface->Start(QDBusObjectPath(m_remoteDesktopSession->path()),
                                                 "",
                                                 {
                                                     {QLatin1String("handle_token"), handleToken},
                                                 });
    reply.waitForFinished();
    if (reply.isError()) {
        m_error = "Failed to start remote desktop session: " + reply.error().message();
        m_sessionStarting = false;
        emit sessionReady();
    }
}

bool AutoTypePlatformWayland::isAvailable()
{
    if (!s_remoteDesktopInterface->isValid()) {
        qWarning() << "XDG Remote Desktop portal is not available, Auto-Type disabled";
        return false;
    }
    if ((s_remoteDesktopInterface->availableDeviceTypes() & 1) == 0) {
        qWarning() << "XDG Remote Desktop portal does not support keyboard input, Auto-Type disabled";
        return false;
    }
    return true;
}

void AutoTypePlatformWayland::unload()
{
    closeSession();
}

QStringList AutoTypePlatformWayland::windowTitles()
{
    return QStringList{};
}

WId AutoTypePlatformWayland::activeWindow()
{
    // return a different value if the focus changes to our own window to stop sequence
    if (qApp->activeWindow()) {
        return -2;
    }

    // return non-zero value to avoid minimizing our own window if it's not in focus
    return -1;
}

QString AutoTypePlatformWayland::activeWindowTitle()
{
    return QString("");
}

bool AutoTypePlatformWayland::raiseWindow(WId window)
{
    Q_UNUSED(window);

    return true;
}

void AutoTypePlatformWayland::buildKeymap()
{
    if (m_xkbKeymap) {
        xkb_keymap_unref(m_xkbKeymap);
        m_xkbKeymap = nullptr;
    }
    if (m_xkbContext) {
        xkb_context_unref(m_xkbContext);
        m_xkbContext = nullptr;
    }
    m_keymap.clear();
    m_error = QString();

    m_xkbContext = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!m_xkbContext) {
        m_error = tr("Failed to create Wayland keyboard context");
        return;
    }

    m_xkbKeymap = xkb_keymap_new_from_names(m_xkbContext, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!m_xkbKeymap) {
        m_error = tr("Failed to create Wayland keyboard map");
        return;
    }

    m_keymap.clear();
    const auto minKeycode = xkb_keymap_min_keycode(m_xkbKeymap);
    const auto maxKeycode = xkb_keymap_max_keycode(m_xkbKeymap);

    for (auto keycode = minKeycode; keycode <= maxKeycode; ++keycode) {
        const auto layoutCount = xkb_keymap_num_layouts_for_key(m_xkbKeymap, keycode);
        for (xkb_layout_index_t layout = 0; layout < layoutCount; ++layout) {
            const auto levelCount = xkb_keymap_num_levels_for_key(m_xkbKeymap, keycode, layout);
            for (xkb_level_index_t level = 0; level < levelCount; ++level) {
                const xkb_keysym_t* syms = nullptr;
                const auto symCount = xkb_keymap_key_get_syms_by_level(m_xkbKeymap, keycode, layout, level, &syms);

                xkb_mod_mask_t modMask = 0;
                xkb_keymap_key_get_mods_for_level(m_xkbKeymap, keycode, layout, level, &modMask, 1);

                for (int symIndex = 0; symIndex < symCount; ++symIndex) {
                    m_keymap.append({syms[symIndex], keycode, modMask});
                }
            }
        }
    }
}

bool AutoTypePlatformWayland::lookupKeysym(xkb_keysym_t keysym, xkb_keycode_t* keycode, xkb_mod_mask_t* modMask) const
{
    for (const auto& key : m_keymap) {
        if (key.sym == keysym) {
            *keycode = key.keycode;
            *modMask = key.modMask;
            return true;
        }
    }
    return false;
}

AutoTypeAction::Result AutoTypePlatformWayland::sendKeycode(int keycode, uint state)
{
    auto reply = s_remoteDesktopInterface->NotifyKeyboardKeycode(
        QDBusObjectPath(m_remoteDesktopSession->path()), {}, keycode, state);
    reply.waitForFinished();
    if (reply.isError()) {
        return AutoTypeAction::Result::Failed(reply.error().message());
    }
    return AutoTypeAction::Result::Ok();
}

AutoTypeAction::Result AutoTypePlatformWayland::sendKey(const AutoTypeKey* action)
{
    xkb_keysym_t keysym;
    if (action->key != Qt::Key_unknown) {
        keysym = qtKeyToXkbKeysym(action->key);
        if (keysym == XKB_KEY_NoSymbol) {
            return AutoTypeAction::Result::Failed(tr("No symbol found for key: '%1'").arg(action->key));
        }
    } else {
        keysym = xkb_utf32_to_keysym(action->character.unicode());
        if (keysym == XKB_KEY_NoSymbol) {
            return AutoTypeAction::Result::Failed(tr("No symbol found for character: '%1'").arg(action->character));
        }
    }

    if (!m_remoteDesktopSession) {
        return AutoTypeAction::Result::Failed(tr("Remote desktop session is not ready"));
    }
    if (!m_xkbKeymap || m_keymap.isEmpty()) {
        return AutoTypeAction::Result::Failed(tr("Wayland keyboard map is not available"));
    }

    xkb_keycode_t keycode = 0;
    xkb_mod_mask_t modMask = 0;
    if (!lookupKeysym(keysym, &keycode, &modMask)) {
        return AutoTypeAction::Result::Failed(tr("No keycode found for symbol: '%1'").arg(keysym));
    }

    const auto evdevKeycode = static_cast<int>(keycode) - XKB_EVDEV_OFFSET;
    if (evdevKeycode < 0) {
        return AutoTypeAction::Result::Failed(tr("Invalid keycode found for symbol: '%1'").arg(keysym));
    }

    const auto shiftIndex = xkb_keymap_mod_get_index(m_xkbKeymap, XKB_MOD_NAME_SHIFT);
    const auto ctrlIndex = xkb_keymap_mod_get_index(m_xkbKeymap, XKB_MOD_NAME_CTRL);
    const auto altIndex = xkb_keymap_mod_get_index(m_xkbKeymap, XKB_MOD_NAME_ALT);
    const auto metaIndex = xkb_keymap_mod_get_index(m_xkbKeymap, XKB_MOD_NAME_LOGO);

    QVector<int> modKeys;
    if ((shiftIndex != XKB_MOD_INVALID && (modMask & (xkb_mod_mask_t(1) << shiftIndex)))
        || (action->modifiers & Qt::ShiftModifier)) {
        modKeys.append(EVDEV_KEY_LEFTSHIFT);
    }
    if ((ctrlIndex != XKB_MOD_INVALID && (modMask & (xkb_mod_mask_t(1) << ctrlIndex)))
        || (action->modifiers & Qt::ControlModifier)) {
        modKeys.append(EVDEV_KEY_LEFTCTRL);
    }
    if ((altIndex != XKB_MOD_INVALID && (modMask & (xkb_mod_mask_t(1) << altIndex)))
        || (action->modifiers & Qt::AltModifier)) {
        modKeys.append(EVDEV_KEY_LEFTALT);
    }
    if ((metaIndex != XKB_MOD_INVALID && (modMask & (xkb_mod_mask_t(1) << metaIndex)))
        || (action->modifiers & Qt::MetaModifier)) {
        modKeys.append(EVDEV_KEY_LEFTMETA);
    }

    AutoTypeAction::Result result = AutoTypeAction::Result::Ok();
    QVector<int> pressedModifiers;
    for (auto modifier : modKeys) {
        result = sendKeycode(modifier, uint(1));
        if (!result.isOk()) {
            for (auto i = pressedModifiers.size() - 1; i >= 0; --i) {
                sendKeycode(pressedModifiers[i], uint(0));
            }
            return result;
        }
        pressedModifiers.append(modifier);
    }

    result = sendKeycode(evdevKeycode, uint(1));
    if (!result.isOk()) {
        for (auto i = pressedModifiers.size() - 1; i >= 0; --i) {
            sendKeycode(pressedModifiers[i], uint(0));
        }
        return result;
    }

    result = sendKeycode(evdevKeycode, uint(0));
    if (!result.isOk()) {
        for (auto i = pressedModifiers.size() - 1; i >= 0; --i) {
            sendKeycode(pressedModifiers[i], uint(0));
        }
        return result;
    }

    for (auto i = pressedModifiers.size() - 1; i >= 0; --i) {
        result = sendKeycode(pressedModifiers[i], uint(0));
        if (!result.isOk()) {
            return result;
        }
    }

    return AutoTypeAction::Result::Ok();
}

AutoTypeExecutor* AutoTypePlatformWayland::createExecutor()
{
    return new AutoTypeExecutorWayland(this);
}

AutoTypeExecutorWayland::AutoTypeExecutorWayland(AutoTypePlatformWayland* platform)
    : m_platform(platform)
{
}

AutoTypeAction::Result AutoTypeExecutorWayland::execBegin(const AutoTypeBegin* action)
{
    Q_UNUSED(action);

    m_platform->waitForSession();

    auto error = m_platform->errorString();
    if (!error.isEmpty()) {
        return AutoTypeAction::Result::Failed(error);
    }

    m_platform->buildKeymap();
    error = m_platform->errorString();
    if (!error.isEmpty()) {
        return AutoTypeAction::Result::Failed(error);
    }

    return AutoTypeAction::Result::Ok();
}

AutoTypeAction::Result AutoTypeExecutorWayland::execType(const AutoTypeKey* action)
{
    auto result = m_platform->sendKey(action);

    if (result.isOk()) {
        Tools::sleep(execDelayMs);
    }

    return result;
}

AutoTypeAction::Result AutoTypeExecutorWayland::execClearField(const AutoTypeClearField* action)
{
    Q_UNUSED(action);
    AutoTypeKey home(Qt::Key_Home);
    auto result = execType(&home);
    if (!result.isOk()) {
        return result;
    }

    AutoTypeKey end(Qt::Key_End, Qt::ShiftModifier);
    result = execType(&end);
    if (!result.isOk()) {
        return result;
    }

    AutoTypeKey backspace(Qt::Key_Backspace);
    result = execType(&backspace);
    if (!result.isOk()) {
        return result;
    }

    return AutoTypeAction::Result::Ok();
}

AutoTypeAction::Result AutoTypeExecutorWayland::execEnd(const AutoTypeEnd* action)
{
    Q_UNUSED(action);
    if (!config()->get(Config::AutoTypeDesktopPortalPersistConnection).toBool()) {
        m_platform->closeSession();
    }
    return AutoTypeAction::Result::Ok();
}
