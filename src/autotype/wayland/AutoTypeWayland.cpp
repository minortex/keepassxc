/*
 *  Copyright (C) 2024 KeePassXC Team <team@keepassxc.org>
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

#include "AutoTypeWayland.h"

#include "autotype/AutoTypeAction.h"
#include "core/Tools.h"
#include "gui/osutils/nixutils/X11Funcs.h"

#include <QDBusMessage>
#include <QDebug>
#include <QRandomGenerator>

#include <xkbcommon/xkbcommon.h>

// evdev modifier keycodes (linux/input-event-codes.h)
constexpr int EVDEV_KEY_LEFTCTRL = 29;
constexpr int EVDEV_KEY_LEFTSHIFT = 42;
constexpr int EVDEV_KEY_LEFTALT = 56;
constexpr int EVDEV_KEY_LEFTMETA = 125;
// XKB keycodes = evdev keycodes + 8
constexpr int XKB_EVDEV_OFFSET = 8;

QString generateToken()
{
    static uint next = 0;
    return QString("keepassxc_%1_%2").arg(next++).arg(QRandomGenerator::system()->generate());
}

AutoTypePlatformWayland::AutoTypePlatformWayland()
    : m_bus(QDBusConnection::sessionBus())
    , m_remote_desktop("org.freedesktop.portal.Desktop",
                       "/org/freedesktop/portal/desktop",
                       "org.freedesktop.portal.RemoteDesktop",
                       m_bus,
                       this)
{
    m_bus.connect("org.freedesktop.portal.Desktop",
                  "",
                  "org.freedesktop.portal.Request",
                  "Response",
                  this,
                  SLOT(portalResponse(uint, QVariantMap, QDBusMessage)));

    createSession();
}

AutoTypePlatformWayland::~AutoTypePlatformWayland()
{
    if (m_xkb_keymap) {
        xkb_keymap_unref(m_xkb_keymap);
    }
    if (m_xkb_context) {
        xkb_context_unref(m_xkb_context);
    }
}

void AutoTypePlatformWayland::createSession()
{
    QString requestHandle = generateToken();

    m_handlers.insert(requestHandle,
                      [this](uint _response, QVariantMap _result) { handleCreateSession(_response, _result); });

    m_remote_desktop.call("CreateSession",
                          QVariantMap{{"handle_token", requestHandle}, {"session_handle_token", generateToken()}});
}

void AutoTypePlatformWayland::handleCreateSession(uint response, QVariantMap result)
{
    qDebug() << "Got response and result" << response << result;
    if (response == 0) {
        m_session_handle = QDBusObjectPath(result["session_handle"].toString());

        QString selectDevicesRequestHandle = generateToken();
        m_handlers.insert(selectDevicesRequestHandle,
                          [this](uint _response, QVariantMap _result) { handleSelectDevices(_response, _result); });

        QVariantMap selectDevicesOptions{
            {"handle_token", selectDevicesRequestHandle},
            {"types", uint(1)},
            {"persist_mode", uint(2)},
        };

        // TODO: Store restore token in database/some other persistent data so the dialog doesn't appear every launch
        if (!m_restore_token.isEmpty()) {
            selectDevicesOptions.insert("restore_token", m_restore_token);
        }

        m_remote_desktop.call("SelectDevices", QVariant::fromValue(m_session_handle), selectDevicesOptions);

        QString startRequestHandle = generateToken();
        m_handlers.insert(startRequestHandle,
                          [this](uint _response, QVariantMap _result) { handleStart(_response, _result); });

        QVariantMap startOptions{
            {"handle_token", startRequestHandle},
        };

        // TODO: Pass window identifier here instead of empty string if we want the dialog to appear on top of the
        // application window, need to be able to get active window and handle from Wayland
        m_remote_desktop.call("Start", QVariant::fromValue(m_session_handle), "", startOptions);
    }
}

void AutoTypePlatformWayland::handleSelectDevices(uint response, QVariantMap result)
{
    Q_UNUSED(result)
    qDebug() << "Select Devices: " << response << result;
}

void AutoTypePlatformWayland::handleStart(uint response, QVariantMap result)
{
    qDebug() << "Start: " << response << result;
    if (response == 0) {
        m_session_started = true;
        m_restore_token = result["restore_token"].toString();
    }
}

void AutoTypePlatformWayland::portalResponse(uint response, QVariantMap results, QDBusMessage message)
{
    Q_UNUSED(response)
    Q_UNUSED(results)
    qDebug() << "Received message: " << message;
    auto index = message.path().lastIndexOf("/");
    auto handle = message.path().right(message.path().length() - index - 1);
    if (m_handlers.contains(handle)) {
        m_handlers.take(handle)(response, results);
    }
}

void AutoTypePlatformWayland::buildKeymap()
{
    if (m_xkb_keymap) {
        xkb_keymap_unref(m_xkb_keymap);
        m_xkb_keymap = nullptr;
    }
    if (m_xkb_context) {
        xkb_context_unref(m_xkb_context);
        m_xkb_context = nullptr;
    }

    m_xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!m_xkb_context) {
        qWarning() << "Failed to create xkb_context";
        return;
    }

    // Uses system defaults (respects XKB_DEFAULT_RULES/MODEL/LAYOUT/VARIANT/OPTIONS env vars)
    m_xkb_keymap = xkb_keymap_new_from_names(m_xkb_context, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!m_xkb_keymap) {
        qWarning() << "Failed to create xkb_keymap";
        return;
    }

    m_keymap.clear();
    xkb_keycode_t min_kc = xkb_keymap_min_keycode(m_xkb_keymap);
    xkb_keycode_t max_kc = xkb_keymap_max_keycode(m_xkb_keymap);

    for (xkb_keycode_t kc = min_kc; kc <= max_kc; kc++) {
        xkb_layout_index_t num_layouts = xkb_keymap_num_layouts_for_key(m_xkb_keymap, kc);
        for (xkb_layout_index_t layout = 0; layout < num_layouts; layout++) {
            xkb_level_index_t num_levels = xkb_keymap_num_levels_for_key(m_xkb_keymap, kc, layout);
            for (xkb_level_index_t level = 0; level < num_levels; level++) {
                const xkb_keysym_t* syms;
                int num_syms = xkb_keymap_key_get_syms_by_level(m_xkb_keymap, kc, layout, level, &syms);

                xkb_mod_mask_t mod_mask = 0;
                xkb_keymap_key_get_mods_for_level(m_xkb_keymap, kc, layout, level, &mod_mask, 1);

                for (int s = 0; s < num_syms; s++) {
                    m_keymap.append({syms[s], kc, layout, mod_mask});
                }
            }
        }
    }
}

bool AutoTypePlatformWayland::lookupKeysym(xkb_keysym_t keysym, xkb_keycode_t* keycode, xkb_mod_mask_t* mod_mask)
{
    for (const auto& key : m_keymap) {
        if (key.sym == keysym) {
            *keycode = key.keycode;
            *mod_mask = key.mod_mask;
            return true;
        }
    }
    return false;
}

AutoTypeAction::Result AutoTypePlatformWayland::sendKey(xkb_keysym_t keysym, Qt::KeyboardModifiers modifiers)
{
    if (!m_session_started || m_session_handle.path().isEmpty()) {
        return AutoTypeAction::Result::Failed(
            tr("Wayland RemoteDesktop portal session is not ready."));
    }

    if (!m_xkb_keymap || m_keymap.isEmpty()) {
        return AutoTypeAction::Result::Failed(tr("Wayland keyboard layout is not available."));
    }

    xkb_keycode_t keycode;
    xkb_mod_mask_t mod_mask;

    if (!lookupKeysym(keysym, &keycode, &mod_mask)) {
        qWarning() << "Unable to find keycode for keysym:" << keysym;
        return AutoTypeAction::Result::Failed(
            QString("Unable to find keycode for keysym: %1").arg(keysym));
    }

    int evdev_keycode = keycode - XKB_EVDEV_OFFSET;
    if (evdev_keycode < 0) {
        return AutoTypeAction::Result::Failed(
            QString("Invalid Wayland keycode for keysym: %1").arg(keysym));
    }

    // Determine which modifier keycodes to press based on keymap lookup and explicit modifiers
    xkb_mod_index_t shift_idx = xkb_keymap_mod_get_index(m_xkb_keymap, XKB_MOD_NAME_SHIFT);
    xkb_mod_index_t ctrl_idx = xkb_keymap_mod_get_index(m_xkb_keymap, XKB_MOD_NAME_CTRL);
    xkb_mod_index_t alt_idx = xkb_keymap_mod_get_index(m_xkb_keymap, XKB_MOD_NAME_ALT);
    xkb_mod_index_t meta_idx = xkb_keymap_mod_get_index(m_xkb_keymap, XKB_MOD_NAME_LOGO);

    QVector<int> mod_keycodes;

    if ((shift_idx != XKB_MOD_INVALID && (mod_mask & (1 << shift_idx)))
        || modifiers.testFlag(Qt::ShiftModifier)) {
        mod_keycodes.append(EVDEV_KEY_LEFTSHIFT);
    }
    if ((ctrl_idx != XKB_MOD_INVALID && (mod_mask & (1 << ctrl_idx)))
        || modifiers.testFlag(Qt::ControlModifier)) {
        mod_keycodes.append(EVDEV_KEY_LEFTCTRL);
    }
    if ((alt_idx != XKB_MOD_INVALID && (mod_mask & (1 << alt_idx)))
        || modifiers.testFlag(Qt::AltModifier)) {
        mod_keycodes.append(EVDEV_KEY_LEFTALT);
    }
    if ((meta_idx != XKB_MOD_INVALID && (mod_mask & (1 << meta_idx)))
        || modifiers.testFlag(Qt::MetaModifier)) {
        mod_keycodes.append(EVDEV_KEY_LEFTMETA);
    }

    // Press modifiers (each is a separate D-Bus call, providing synchronization)
    for (int mk : mod_keycodes) {
        m_remote_desktop.call(
            "NotifyKeyboardKeycode", QVariant::fromValue(m_session_handle), QVariantMap(), mk, uint(1));
    }

    // Press and release key
    m_remote_desktop.call(
        "NotifyKeyboardKeycode", QVariant::fromValue(m_session_handle), QVariantMap(), evdev_keycode, uint(1));
    m_remote_desktop.call(
        "NotifyKeyboardKeycode", QVariant::fromValue(m_session_handle), QVariantMap(), evdev_keycode, uint(0));

    // Release modifiers (reverse order)
    for (int i = mod_keycodes.size() - 1; i >= 0; i--) {
        m_remote_desktop.call(
            "NotifyKeyboardKeycode", QVariant::fromValue(m_session_handle), QVariantMap(), mod_keycodes[i], uint(0));
    }

    return AutoTypeAction::Result::Ok();
}

bool AutoTypePlatformWayland::isAvailable()
{
    return true;
}

void AutoTypePlatformWayland::unload()
{
}

QString AutoTypePlatformWayland::activeWindowTitle()
{
    return {};
}

WId AutoTypePlatformWayland::activeWindow()
{
    return 0;
}

AutoTypeExecutor* AutoTypePlatformWayland::createExecutor()
{
    return new AutoTypeExecutorWayland(this);
}

bool AutoTypePlatformWayland::raiseWindow(WId window)
{
    Q_UNUSED(window)
    return false;
}

QStringList AutoTypePlatformWayland::windowTitles()
{
    return {};
}

AutoTypeExecutorWayland::AutoTypeExecutorWayland(AutoTypePlatformWayland* platform)
    : m_platform(platform)
{
}

AutoTypeAction::Result AutoTypeExecutorWayland::execBegin(const AutoTypeBegin* action)
{
    Q_UNUSED(action)
    m_platform->buildKeymap();
    if (!m_platform->isSessionStarted()) {
        return AutoTypeAction::Result::Failed(
            QObject::tr("Wayland RemoteDesktop portal session is not ready."));
    }
    if (!m_platform->hasKeymap()) {
        return AutoTypeAction::Result::Failed(QObject::tr("Wayland keyboard layout is not available."));
    }
    return AutoTypeAction::Result::Ok();
}

AutoTypeAction::Result AutoTypeExecutorWayland::execType(const AutoTypeKey* action)
{
    AutoTypeAction::Result result = AutoTypeAction::Result::Ok();
    if (action->key != Qt::Key_unknown) {
        result = m_platform->sendKey(qtToNativeKeyCode(action->key), action->modifiers);
    } else {
        result = m_platform->sendKey(qcharToNativeKeyCode(action->character), action->modifiers);
    }

    if (result.isOk()) {
        Tools::sleep(execDelayMs);
    }

    return result;
}

AutoTypeAction::Result AutoTypeExecutorWayland::execClearField(const AutoTypeClearField* action)
{
    Q_UNUSED(action)
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
