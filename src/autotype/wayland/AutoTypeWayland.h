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

#pragma once

#include <QDBusInterface>

#include <xkbcommon/xkbcommon.h>

#include "autotype/AutoTypePlatformPlugin.h"

class AutoTypePlatformWayland : public QObject, public AutoTypePlatformInterface
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.keepassx.AutoTypePlatformWaylnd")
    Q_INTERFACES(AutoTypePlatformInterface)

public:
    AutoTypePlatformWayland();
    ~AutoTypePlatformWayland() override;
    bool isAvailable() override;
    void unload() override;
    QStringList windowTitles() override;
    WId activeWindow() override;
    QString activeWindowTitle() override;
    bool raiseWindow(WId window) override;
    AutoTypeExecutor* createExecutor() override;

    AutoTypeAction::Result sendKey(xkb_keysym_t keysym, Qt::KeyboardModifiers modifiers = Qt::NoModifier);
    void createSession();
    void buildKeymap();
    bool isSessionStarted() const
    {
        return m_session_started;
    }
    bool hasKeymap() const
    {
        return m_xkb_keymap && !m_keymap.isEmpty();
    }

private slots:
    void portalResponse(uint response, QVariantMap results, QDBusMessage message);

private:
    struct KeyDesc {
        xkb_keysym_t sym;
        xkb_keycode_t keycode;
        xkb_layout_index_t layout;
        xkb_mod_mask_t mod_mask;
    };

    bool lookupKeysym(xkb_keysym_t keysym, xkb_keycode_t* keycode, xkb_mod_mask_t* mod_mask);

    bool m_loaded;
    QDBusConnection m_bus;
    QMap<QString, std::function<void(uint, QVariantMap)>> m_handlers;
    QDBusInterface m_remote_desktop;
    QDBusObjectPath m_session_handle;
    QString m_restore_token;
    bool m_session_started = false;

    struct xkb_context* m_xkb_context = nullptr;
    struct xkb_keymap* m_xkb_keymap = nullptr;
    QList<KeyDesc> m_keymap;

    void handleCreateSession(uint response, QVariantMap results);
    void handleSelectDevices(uint response, QVariantMap results);
    void handleStart(uint response, QVariantMap results);
};

class AutoTypeExecutorWayland : public AutoTypeExecutor
{
public:
    explicit AutoTypeExecutorWayland(AutoTypePlatformWayland* platform);

    AutoTypeAction::Result execBegin(const AutoTypeBegin* action) override;
    AutoTypeAction::Result execType(const AutoTypeKey* action) override;
    AutoTypeAction::Result execClearField(const AutoTypeClearField* action) override;

private:
    AutoTypePlatformWayland* const m_platform;
};
