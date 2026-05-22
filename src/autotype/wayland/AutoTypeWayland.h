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

#ifndef KEEPASSXC_AUTOTYPEWAYLAND_H
#define KEEPASSXC_AUTOTYPEWAYLAND_H

#include <QApplication>
#include <QSet>
#include <QWidget>
#include <QtPlugin>
#include <functional>

#include "autotype/AutoTypePlatformPlugin.h"
#include "xdp_session.h"

#include <xkbcommon/xkbcommon.h>

class OrgFreedesktopPortalRequestInterface;

class AutoTypePlatformWayland : public QObject, public AutoTypePlatformInterface
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.keepassx.AutoTypePlatformWayland")
    Q_INTERFACES(AutoTypePlatformInterface)

signals:
    void sessionReady();

public:
    AutoTypePlatformWayland();
    ~AutoTypePlatformWayland() override;
    bool isAvailable() override;
    QStringList windowTitles() override;
    WId activeWindow() override;
    QString activeWindowTitle() override;
    bool raiseWindow(WId window) override;
    void unload() override;
    void prepareForAutoType() override;

    AutoTypeExecutor* createExecutor() override;
    AutoTypeAction::Result sendKey(const AutoTypeKey*);
    void waitForSession();
    void closeSession();
    const QString errorString() const;

private:
    friend class AutoTypeExecutorWayland;

    struct KeyDesc
    {
        xkb_keysym_t sym;
        xkb_keycode_t keycode;
        xkb_mod_mask_t modMask;
    };

    QString request(const std::function<void(const QVariantMap&)> handler);
    void tryStartSession();
    void selectDevices();
    void startSession();
    void buildKeymap();
    bool lookupKeysym(xkb_keysym_t keysym, xkb_keycode_t* keycode, xkb_mod_mask_t* modMask) const;
    AutoTypeAction::Result sendKeycode(int keycode, uint state);

    bool m_sessionStarting = false;
    QString m_restoreToken;
    OrgFreedesktopPortalSessionInterface* m_remoteDesktopSession = nullptr;
    QString m_error = QString();
    xkb_context* m_xkbContext = nullptr;
    xkb_keymap* m_xkbKeymap = nullptr;
    QList<KeyDesc> m_keymap;
};

class AutoTypeExecutorWayland : public AutoTypeExecutor
{
public:
    explicit AutoTypeExecutorWayland(AutoTypePlatformWayland* platform);

    AutoTypeAction::Result execBegin(const AutoTypeBegin* action) override;
    AutoTypeAction::Result execType(const AutoTypeKey* action) override;
    AutoTypeAction::Result execClearField(const AutoTypeClearField* action) override;
    AutoTypeAction::Result execEnd(const AutoTypeEnd* action) override;

private:
    AutoTypePlatformWayland* const m_platform;
};

#endif // KEEPASSXC_AUTOTYPEWAYLAND_H
