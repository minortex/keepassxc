/*
 * Copyright (C) 2025 KeePassXC Team <team@keepassxc.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 or (at your option)
 * version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "NixUtils.h"

#include "gui/MainWindow.h"
#include "gui/MessageBox.h"
#include "xdp_globalshortcuts.h"
#include "xdp_request.h"
#include "xdp_session.h"

#include "config-keepassx.h"
#include "core/Config.h"
#include "core/Global.h"

#include <QApplication>
#include <QDBusInterface>
#include <QDebug>
#include <QDir>
#include <QPointer>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QStyle>
#include <QTextStream>
#include <QTimer>
#include <qapplication.h>
#include <qdbusextratypes.h>
#include <qdbusmetatype.h>
#include <qobject.h>
#ifdef WITH_X11
#include <QGuiApplication>

#include "X11Funcs.h"
#include <X11/XKBlib.h>
#include <xcb/xproto.h>

namespace
{
    Display* dpy;
    Window rootWindow;
    bool x11ErrorOccurred = false;

    int x11ErrorHandler(Display*, XErrorEvent*)
    {
        x11ErrorOccurred = true;
        return 1;
    }
} // namespace
#endif

Q_GLOBAL_STATIC_WITH_ARGS(OrgFreedesktopPortalGlobalShortcutsInterface,
                          s_shortcutsInterface,
                          ("org.freedesktop.portal.Desktop",
                           "/org/freedesktop/portal/desktop",
                           QDBusConnection::sessionBus()));

using XdpShortcut = QPair<QString, QVariantMap>;
using XdpShortcuts = QList<XdpShortcut>;

QPointer<NixUtils> NixUtils::m_instance = nullptr;

NixUtils* NixUtils::instance()
{
    if (!m_instance) {
        m_instance = new NixUtils(qApp);
    }

    return m_instance;
}

NixUtils::NixUtils(QObject* parent)
    : OSUtilsBase(parent)
{
#ifdef WITH_X11
    if (auto* native = qGuiApp->nativeInterface<QNativeInterface::QX11Application>()) {
        dpy = native->display();
        rootWindow = DefaultRootWindow(dpy);
    }
#endif

    // notify about system color scheme changes
    QDBusConnection sessionBus = QDBusConnection::sessionBus();
    sessionBus.connect("org.freedesktop.portal.Desktop",
                       "/org/freedesktop/portal/desktop",
                       "org.freedesktop.portal.Settings",
                       "SettingChanged",
                       this,
                       SLOT(handleColorSchemeChanged(QString, QString, QDBusVariant)));

    QDBusMessage msg = QDBusMessage::createMethodCall(
        "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop", "org.freedesktop.portal.Settings", "Read");
    msg << QVariant("org.freedesktop.appearance") << QVariant("color-scheme");
    sessionBus.callWithCallback(msg, this, SLOT(handleColorSchemeRead(QDBusVariant)));
}

void NixUtils::initGlobalShortcutsSession()
{
    if (!externalGlobalShortcutsConfigurator() || !s_shortcutsInterface->isValid()) {
        return;
    }

    qDBusRegisterMetaType<XdpShortcut>();
    qDBusRegisterMetaType<XdpShortcuts>();

    connect(s_shortcutsInterface, &OrgFreedesktopPortalGlobalShortcutsInterface::Activated, this, [this]() {
        globalShortcutTriggered("autotype");
    });

    createGlobalShortcutsSession();
}

NixUtils::~NixUtils() = default;

bool NixUtils::isDarkMode() const
{
    // prefer freedesktop "org.freedesktop.appearance color-scheme" setting
    if (m_systemColorschemePrefExists) {
        return m_systemColorschemePref == ColorschemePref::PreferDark;
    }

    if (!qApp || !qApp->style()) {
        return false;
    }
    return qApp->style()->standardPalette().color(QPalette::Window).toHsl().lightness() < 110;
}

bool NixUtils::isStatusBarDark() const
{
    // TODO: implement
    return isDarkMode();
}

QString NixUtils::getAutostartDesktopFilename(bool createDirs) const
{
    QDir autostartDir;
    auto confHome = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    if (confHome.isEmpty()) {
        return {};
    }
    autostartDir.setPath(confHome + QStringLiteral("/autostart"));
    if (createDirs && !autostartDir.exists()) {
        autostartDir.mkpath(".");
    }

    return QFile(autostartDir.absoluteFilePath(qApp->property("KPXC_QUALIFIED_APPNAME").toString().append(".desktop")))
        .fileName();
}

bool NixUtils::isLaunchAtStartupEnabled() const
{
#ifndef KEEPASSXC_DIST_FLATPAK
    return QFile::exists(getAutostartDesktopFilename());
#else
    return config()->get(Config::GUI_LaunchAtStartup).toBool();
#endif
}

void NixUtils::setLaunchAtStartup(bool enable)
{
#ifndef KEEPASSXC_DIST_FLATPAK
    if (enable) {
        QFile desktopFile(getAutostartDesktopFilename(true));
        if (!desktopFile.open(QIODevice::WriteOnly)) {
            qWarning("Failed to create autostart desktop file.");
            return;
        }

        const QString appImagePath = QString::fromLocal8Bit(qgetenv("APPIMAGE"));
        const bool isAppImage = !appImagePath.isNull() && QFile::exists(appImagePath);
        const QString executeablePathOrName = isAppImage ? appImagePath : QApplication::applicationName().toLower();

        QTextStream stream(&desktopFile);
        stream << QStringLiteral("[Desktop Entry]") << '\n'
               << QStringLiteral("Name=") << QApplication::applicationDisplayName() << '\n'
               << QStringLiteral("GenericName=") << tr("Password Manager") << '\n'
               << QStringLiteral("Exec=") << executeablePathOrName << '\n'
               << QStringLiteral("TryExec=") << executeablePathOrName << '\n'
               << QStringLiteral("Icon=") << QApplication::applicationName().toLower() << '\n'
               << QStringLiteral("StartupWMClass=keepassxc") << '\n'
               << QStringLiteral("StartupNotify=false") << '\n'
               << QStringLiteral("Terminal=false") << '\n'
               << QStringLiteral("Type=Application") << '\n'
               << QStringLiteral("Version=1.0") << '\n'
               << QStringLiteral("Categories=Utility;Security;Qt;") << '\n'
               << QStringLiteral("MimeType=application/x-keepass2;") << '\n'
               << QStringLiteral("X-GNOME-Autostart-enabled=true") << '\n'
               << QStringLiteral("X-GNOME-Autostart-Delay=2") << '\n'
               << QStringLiteral("X-KDE-autostart-after=panel") << '\n'
               << QStringLiteral("X-LXQt-Need-Tray=true") << Qt::endl;
        desktopFile.close();
    } else if (isLaunchAtStartupEnabled()) {
        QFile::remove(getAutostartDesktopFilename());
    }
#else
    QDBusConnection sessionBus = QDBusConnection::sessionBus();
    QDBusMessage msg = QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop",
                                                      "/org/freedesktop/portal/desktop",
                                                      "org.freedesktop.portal.Background",
                                                      "RequestBackground");

    QMap<QString, QVariant> options;
    options["autostart"] = QVariant(enable);
    options["reason"] = QVariant("Launch KeePassXC at startup");
    int token = QRandomGenerator::global()->bounded(1000, 9999);
    options["handle_token"] = QVariant(QString("org/keepassxc/KeePassXC/%1").arg(token));

    msg << "" << options;

    QDBusMessage response = sessionBus.call(msg);

    QDBusObjectPath handle = response.arguments().at(0).value<QDBusObjectPath>();

    bool res = sessionBus.connect("org.freedesktop.portal.Desktop",
                                  handle.path(),
                                  "org.freedesktop.portal.Request",
                                  "Response",
                                  this,
                                  SLOT(launchAtStartupRequested(uint, QVariantMap)));

    if (!res) {
        qDebug() << "DBus Error: could not connect to org.freedesktop.portal.Request";
    }
#endif
}

void NixUtils::launchAtStartupRequested(uint response, const QVariantMap& results)
{
    if (response > 0) {
        qDebug() << "DBus Error: the request to autostart was cancelled.";
        return;
    }

    config()->set(Config::GUI_LaunchAtStartup, results["autostart"].value<bool>());
}

bool NixUtils::isCapslockEnabled()
{
#ifdef WITH_X11
    if (auto* native = qGuiApp->nativeInterface<QNativeInterface::QX11Application>()) {
        auto* display = native->display();
        if (!display) {
            return false;
        }

        auto platform = QGuiApplication::platformName();
        if (platform == "xcb") {
            unsigned state = 0;
            if (XkbGetIndicatorState(reinterpret_cast<Display*>(display), XkbUseCoreKbd, &state) == Success) {
                return ((state & 1u) != 0);
            }
        }
    }
#endif

    // TODO: Wayland

    return false;
}

void NixUtils::setUserInputProtection(bool enable)
{
    // Linux does not support this feature
    Q_UNUSED(enable)
}

void NixUtils::registerNativeEventFilter()
{
    qApp->installNativeEventFilter(this);
}

bool NixUtils::nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result)
{
    Q_UNUSED(result)
#ifdef WITH_X11
    if (eventType != QByteArrayLiteral("xcb_generic_event_t")) {
        return false;
    }

    auto* genericEvent = static_cast<xcb_generic_event_t*>(message);
    quint8 type = genericEvent->response_type & 0x7f;

    if (type == XCB_KEY_PRESS) {
        auto* keyPressEvent = static_cast<xcb_key_press_event_t*>(message);
        auto modifierMask = ControlMask | ShiftMask | Mod1Mask | Mod4Mask;
        return triggerGlobalShortcut(keyPressEvent->detail, keyPressEvent->state & modifierMask);
    }
#else
    Q_UNUSED(eventType)
    Q_UNUSED(message)
#endif
    return false;
}

bool NixUtils::triggerGlobalShortcut(uint keycode, uint modifiers)
{
#ifdef WITH_X11
    QHashIterator<QString, QSharedPointer<globalShortcut>> i(m_globalShortcuts);
    while (i.hasNext()) {
        i.next();
        if (i.value()->nativeKeyCode == keycode && i.value()->nativeModifiers == modifiers) {
            emit globalShortcutTriggered(i.key());
            return true;
        }
    }
#else
    Q_UNUSED(keycode)
    Q_UNUSED(modifiers)
#endif
    return false;
}

bool NixUtils::registerGlobalShortcut(const QString& name, Qt::Key key, Qt::KeyboardModifiers modifiers, QString* error)
{
#ifdef WITH_X11
    if (QApplication::platformName() != "xcb") {
        return true;
    }

    auto keycode = XKeysymToKeycode(dpy, qtToNativeKeyCode(key));
    auto modifierscode = qtToNativeModifiers(modifiers);

    // Check if this key combo is registered to another shortcut
    QHashIterator<QString, QSharedPointer<globalShortcut>> i(m_globalShortcuts);
    while (i.hasNext()) {
        i.next();
        if (i.value()->nativeKeyCode == keycode && i.value()->nativeModifiers == modifierscode && i.key() != name) {
            if (error) {
                *error = tr("Global shortcut already registered to %1").arg(i.key());
            }
            return false;
        }
    }

    unregisterGlobalShortcut(name);

    x11ErrorOccurred = false;
    auto prevHandler = XSetErrorHandler(x11ErrorHandler);

    XGrabKey(dpy, keycode, modifierscode, rootWindow, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, keycode, modifierscode | Mod2Mask, rootWindow, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, keycode, modifierscode | LockMask, rootWindow, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, keycode, modifierscode | Mod2Mask | LockMask, rootWindow, True, GrabModeAsync, GrabModeAsync);

    XSync(dpy, False);
    XSetErrorHandler(prevHandler);

    if (x11ErrorOccurred) {
        x11ErrorOccurred = false;
        if (error) {
            *error = tr("Could not register global shortcut");
        }
        return false;
    }

    auto gs = QSharedPointer<globalShortcut>::create();
    gs->nativeKeyCode = keycode;
    gs->nativeModifiers = modifierscode;
    m_globalShortcuts.insert(name, gs);
#else
    Q_UNUSED(name)
    Q_UNUSED(key)
    Q_UNUSED(modifiers)
    Q_UNUSED(error)
#endif
    return true;
}

bool NixUtils::unregisterGlobalShortcut(const QString& name)
{
#ifdef WITH_X11
    if (!m_globalShortcuts.contains(name)) {
        return false;
    }

    auto gs = m_globalShortcuts.value(name);
    XUngrabKey(dpy, gs->nativeKeyCode, gs->nativeModifiers, rootWindow);
    XUngrabKey(dpy, gs->nativeKeyCode, gs->nativeModifiers | Mod2Mask, rootWindow);
    XUngrabKey(dpy, gs->nativeKeyCode, gs->nativeModifiers | LockMask, rootWindow);
    XUngrabKey(dpy, gs->nativeKeyCode, gs->nativeModifiers | Mod2Mask | LockMask, rootWindow);

    m_globalShortcuts.remove(name);
#else
    Q_UNUSED(name)
#endif
    return true;
}

void NixUtils::handleColorSchemeRead(QDBusVariant value)
{
    value = qvariant_cast<QDBusVariant>(value.variant());
    setColorScheme(value);
}

void NixUtils::handleColorSchemeChanged(QString ns, QString key, QDBusVariant value)
{
    if (ns == "org.freedesktop.appearance" && key == "color-scheme") {
        setColorScheme(value);
    }
}

void NixUtils::setColorScheme(QDBusVariant value)
{
    m_systemColorschemePref = static_cast<ColorschemePref>(value.variant().toInt());
    m_systemColorschemePrefExists = true;
    emit interfaceThemeChanged();
}

quint64 NixUtils::getProcessStartTime() const
{
    QString processStatPath = QString("/proc/%1/stat").arg(QCoreApplication::applicationPid());
    QFile processStatFile(processStatPath);

    if (!processStatFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qDebug() << "nixutils: failed to open " << processStatPath;
        return 0;
    }

    QTextStream processStatStream(&processStatFile);
    QString processStatInfo = processStatStream.readLine();
    processStatFile.close();

    auto startIndex = processStatInfo.lastIndexOf(')');
    if (startIndex != -1) {
        auto tokens = QStringView{processStatInfo}.mid(startIndex + 2).split(' ');
        if (tokens.size() >= 20) {
            bool ok;
            auto time = tokens[19].toULongLong(&ok);
            if (!ok) {
                qDebug() << "nixutils: failed to convert " << tokens[19] << " to an integer in " << processStatPath;
                return 0;
            }
            return time;
        }
        qDebug() << "nixutils: failed to find at least 20 values in " << processStatPath;
        return 0;
    }

    qDebug() << "nixutils: failed to find ')' in " << processStatPath;
    return 0;
}

// Implements only 0.9+ requests where the path is known before making the call
QString NixUtils::portalRequest(const std::function<void(uint, const QVariantMap&)> handler)
{
    static uint next;
    auto bus = QDBusConnection::sessionBus();
    auto token = QString("request_%1_%2").arg(++next).arg(QRandomGenerator::system()->generate());
    auto sender = bus.baseService().remove(0, 1).replace(".", "_");

    QStringList path;
    path << "" << "org" << "freedesktop" << "portal" << "desktop" << "request" << sender << token;

    auto req = new OrgFreedesktopPortalRequestInterface("org.freedesktop.portal.Desktop", path.join('/'), bus, this);
    connect(req, &OrgFreedesktopPortalRequestInterface::Response, req, handler);
    connect(req, &OrgFreedesktopPortalRequestInterface::Response, req, &QObject::deleteLater);

    auto timer = new QTimer(req);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, req, [req]() {
        qWarning() << "NixUtils::portalRequest: timed out waiting for portal response, closing request";
        req->Close();
        req->deleteLater();
    });
    connect(req, &OrgFreedesktopPortalRequestInterface::Response, timer, &QTimer::stop);
    timer->start(30000);

    return token;
}

bool NixUtils::externalGlobalShortcutsConfigurator()
{
    // TODO: allow overriding this in config to use with X11 if desired
    return QApplication::platformName() == "wayland";
}

void NixUtils::bindShortcutsToCurrentSession()
{
    // Only attempt to restore bindings if previously configured
    if (!config()->get(Config::GUI_XDPGlobalShortcutsConfigured).toBool()) {
        return;
    }

    // Use ListShortcuts to check if shortcuts are already active (e.g. KDE session restoration), on GNOME we need to
    // rebind them every time
    auto listToken = portalRequest([this](uint listResponse, const QVariantMap& listResults) {
        if (listResponse != 0) {
            qWarning() << "NixUtils::bindShortcutsToCurrentSession ListShortcuts failed with response:" << listResponse;
            return;
        }

        auto existing = qdbus_cast<XdpShortcuts>(listResults.value("shortcuts"));
        if (!existing.isEmpty()) {
            return; // already active, nothing to do
        }

        if (!m_globalShortcutsSession) {
            qWarning() << "NixUtils::bindShortcutsToCurrentSession: session was closed, aborting bind";
            return;
        }

        callBindShortcuts();
    });

    auto listReply = s_shortcutsInterface->ListShortcuts(QDBusObjectPath(m_globalShortcutsSession->path()),
                                                         {{QLatin1String("handle_token"), listToken}});
    listReply.waitForFinished();
    if (listReply.isError()) {
        qWarning() << "NixUtils::bindShortcutsToCurrentSession ListShortcuts failed:" << listReply.error().message();
    }
}

void NixUtils::createGlobalShortcutsSession()
{
    auto handleToken = portalRequest([this](uint createSessionResponse, const QVariantMap& results) {
        if (createSessionResponse != 0) {
            qWarning() << "NixUtils::createGlobalShortcutsSession CreateSession got unexpected response from portal:"
                       << createSessionResponse;
            return;
        }

        m_globalShortcutsSession = new OrgFreedesktopPortalSessionInterface("org.freedesktop.portal.Desktop",
                                                                            results["session_handle"].toString(),
                                                                            QDBusConnection::sessionBus(),
                                                                            this);
        connect(m_globalShortcutsSession,
                &OrgFreedesktopPortalSessionInterface::Closed,
                m_globalShortcutsSession,
                &QObject::deleteLater);
        connect(m_globalShortcutsSession, &OrgFreedesktopPortalSessionInterface::Closed, this, [this]() {
            m_globalShortcutsSession = nullptr;
            QTimer::singleShot(1000, this, &NixUtils::createGlobalShortcutsSession);
        });

        bindShortcutsToCurrentSession();
    });

    auto sessionHandleToken = "keepassxc_" + QString::number(QRandomGenerator::global()->generate());
    auto reply = s_shortcutsInterface->CreateSession({
        {QLatin1String("session_handle_token"), sessionHandleToken},
        {QLatin1String("handle_token"), handleToken},
    });
    reply.waitForFinished();
    if (reply.isError()) {
        qWarning() << "Failed to create Global Shortcuts session" << reply.error().message();
    }
}

void NixUtils::configureGlobalShortcuts()
{
    if (!s_shortcutsInterface->isValid() || !m_globalShortcutsSession) {
        MessageBox::warning(getMainWindow(),
                            tr("KeePassXC - Global Shortcuts"),
                            tr("The XDG Desktop Portal for global shortcuts is not available on this system."));
        return;
    }

    // Use the config flag to determine if shortcuts have been configured before.
    // We cannot use ListShortcuts here because on GNOME, ListShortcuts always returns empty
    // even after BindShortcuts was called (newer GNOME auto-applies previously saved shortcuts
    // silently without populating the session's shortcuts array). Relying on ListShortcuts
    // would cause BindShortcuts to be called again, but the portal rejects it (bound=true),
    // resulting in no dialog being shown.
    if (config()->get(Config::GUI_XDPGlobalShortcutsConfigured).toBool()) {
        // Already configured: portal v2+ can open a reconfiguration dialog directly
        if (s_shortcutsInterface->version() >= 2) {
            auto handleToken = portalRequest([](uint response, const QVariantMap&) { Q_UNUSED(response); });
            auto reply = s_shortcutsInterface->ConfigureShortcuts(
                QDBusObjectPath(m_globalShortcutsSession->path()), "", {{QLatin1String("handle_token"), handleToken}});
            reply.waitForFinished();
            if (!reply.isError()) {
                return;
            }

            qWarning() << "NixUtils::configureGlobalShortcuts ConfigureShortcuts failed, falling through to dialog:"
                       << reply.error().message();
        }

        // Portal v1 (e.g. GNOME): no reconfiguration API, direct the user to system settings
        MessageBox::information(getMainWindow(),
                                tr("KeePassXC - Global Shortcuts"),
                                tr("Global shortcuts are already configured. "
                                   "To change them, open your system settings and navigate to the "
                                   "keyboard or application shortcuts section."));
        return;
    }

    // First-time setup: use BindShortcuts to present the user with a system dialog.
    // bindShortcutsToCurrentSession() skips BindShortcuts when the config flag is false,
    // so the session's bound flag is still false and this call will succeed.
    callBindShortcuts();
}

void NixUtils::callBindShortcuts()
{
    XdpShortcuts shortcuts = {
        {QLatin1String("autotype"), {{QStringLiteral("description"), tr("Trigger global Auto-Type")}}}};

    auto bindToken = portalRequest([](uint bindResponse, const QVariantMap&) {
        if (bindResponse != 0) {
            qWarning() << "NixUtils: BindShortcuts returned response" << bindResponse
                       << "(shortcut may still be active; known GNOME desktop portal bug in older versions)";
        }
        // Setting this unconditionally because if the config is out-of-sync with system settings this'll ensure we
        // rebind them on startup
        config()->set(Config::GUI_XDPGlobalShortcutsConfigured, true);
    });

    auto reply = s_shortcutsInterface->BindShortcuts(
        QDBusObjectPath(m_globalShortcutsSession->path()), shortcuts, "", {{QLatin1String("handle_token"), bindToken}});
    reply.waitForFinished();
    if (reply.isError()) {
        qWarning() << "NixUtils::callBindShortcuts BindShortcuts failed:" << reply.error().message();
    }
}
