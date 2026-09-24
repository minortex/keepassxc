#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QDBusPendingCallWatcher>
#include <QEventLoop>
#include <QTextStream>
#include <QThread>
#include <QVariantMap>

#include <xkbcommon/xkbcommon.h>

namespace
{
    constexpr auto service = "org.freedesktop.portal.Desktop";
    constexpr auto path = "/org/freedesktop/portal/desktop";
    constexpr auto interface = "org.freedesktop.portal.RemoteDesktop";
    constexpr auto requestInterface = "org.freedesktop.portal.Request";
    constexpr uint keyboardDevice = 1;

    class RequestWaiter : public QObject
    {
        Q_OBJECT
    public:
        explicit RequestWaiter(QObject* parent = nullptr)
            : QObject(parent)
        {
        }

        bool wait(const QString& requestPath, uint& responseCode, QVariantMap& results)
        {
            m_loop = new QEventLoop(this);
            const bool connected = QDBusConnection::sessionBus().connect(QString::fromLatin1(service),
                                                                          requestPath,
                                                                          QString::fromLatin1(requestInterface),
                                                                          QStringLiteral("Response"),
                                                                          this,
                                                                          SLOT(handleResponse(uint,QVariantMap)));
            if (!connected) {
                delete m_loop;
                m_loop = nullptr;
                return false;
            }
            m_loop->exec();
            QDBusConnection::sessionBus().disconnect(QString::fromLatin1(service),
                                                      requestPath,
                                                      QString::fromLatin1(requestInterface),
                                                      QStringLiteral("Response"),
                                                      this,
                                                      SLOT(handleResponse(uint,QVariantMap)));
            responseCode = m_responseCode;
            results = m_results;
            delete m_loop;
            m_loop = nullptr;
            return true;
        }

    private slots:
        void handleResponse(uint code, const QVariantMap& results)
        {
            m_responseCode = code;
            m_results = results;
            if (m_loop) {
                m_loop->quit();
            }
        }

    private:
        QEventLoop* m_loop = nullptr;
        uint m_responseCode = 1;
        QVariantMap m_results;
    };

    bool portalRequest(const QString& method, const QVariantList& arguments, QVariantMap& results)
    {
        auto message = QDBusMessage::createMethodCall(QString::fromLatin1(service),
                                                      QString::fromLatin1(path),
                                                      QString::fromLatin1(interface),
                                                      method);
        message.setArguments(arguments);
        auto reply = QDBusConnection::sessionBus().call(message, QDBus::Block, 10000);
        if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty()) {
            QTextStream(stderr) << method << " failed: " << reply.errorMessage() << Qt::endl;
            return false;
        }

        const auto requestPath = reply.arguments().first().value<QDBusObjectPath>().path();
        RequestWaiter waiter;
        uint responseCode = 1;
        if (!waiter.wait(requestPath, responseCode, results) || responseCode != 0) {
            QTextStream(stderr) << method << " request was not accepted (response " << responseCode << ")"
                                << Qt::endl;
            return false;
        }
        return true;
    }

    bool notifyKeysym(const QString& session, uint keysym, bool pressed)
    {
        auto message = QDBusMessage::createMethodCall(QString::fromLatin1(service),
                                                      QString::fromLatin1(path),
                                                      QString::fromLatin1(interface),
                                                      QStringLiteral("NotifyKeyboardKeysym"));
        message << QDBusObjectPath(session) << QVariantMap{} << keysym << uint(pressed ? 1 : 0);
        const auto reply = QDBusConnection::sessionBus().call(message, QDBus::Block, 10000);
        if (reply.type() == QDBusMessage::ErrorMessage) {
            QTextStream(stderr) << "NotifyKeyboardKeysym failed: " << reply.errorMessage() << Qt::endl;
            return false;
        }
        return true;
    }
}

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 2) {
        QTextStream(stderr) << "Usage: " << args.value(0) << " <text>" << Qt::endl;
        return 2;
    }

    const auto codepoints = args.at(1).toUcs4();
    if (codepoints.isEmpty()) {
        QTextStream(stderr) << "Text must not be empty." << Qt::endl;
        return 2;
    }

    QVariantMap created;
    if (!portalRequest(QStringLiteral("CreateSession"),
                       {QVariantMap{{QStringLiteral("handle_token"), QStringLiteral("keepassxc_test")},
                                    {QStringLiteral("session_handle_token"), QStringLiteral("keepassxc_test_session")}}},
                       created)) {
        return 1;
    }
    auto sessionValue = created.value(QStringLiteral("session_handle"));
    while (sessionValue.metaType() == QMetaType::fromType<QDBusVariant>()) {
        sessionValue = sessionValue.value<QDBusVariant>().variant();
    }
    const auto session = sessionValue.metaType() == QMetaType::fromType<QDBusObjectPath>()
        ? sessionValue.value<QDBusObjectPath>().path()
        : sessionValue.toString();
    if (session.isEmpty()) {
        QTextStream error(stderr);
        error << "Portal did not return a session handle. Results contained:";
        for (auto it = created.cbegin(); it != created.cend(); ++it) {
            error << " " << it.key() << " (" << it.value().metaType().name() << ")";
        }
        error << Qt::endl;
        return 1;
    }

    QVariantMap ignored;
    if (!portalRequest(QStringLiteral("SelectDevices"), {QDBusObjectPath(session),
                                                           QVariantMap{{QStringLiteral("handle_token"),
                                                                        QStringLiteral("keepassxc_test_select")},
                                                                       {QStringLiteral("types"), keyboardDevice}},
                                                           }, ignored)
        || !portalRequest(QStringLiteral("Start"), {QDBusObjectPath(session),
                                                     QString(),
                                                     QVariantMap{{QStringLiteral("handle_token"),
                                                                  QStringLiteral("keepassxc_test_start")}},
                                                     }, ignored)) {
        return 1;
    }

    QTextStream(stdout) << "Portal session is ready. Focus the target text field; typing starts in 3 seconds."
                        << Qt::endl;
    QThread::sleep(3);

    for (const auto codepoint : codepoints) {
        const auto keysym = xkb_utf32_to_keysym(codepoint);
        if (keysym == XKB_KEY_NoSymbol) {
            QTextStream(stderr) << "No keysym for U+" << QString::number(codepoint, 16).toUpper() << Qt::endl;
            return 1;
        }
        if (!notifyKeysym(session, keysym, true) || !notifyKeysym(session, keysym, false)) {
            return 1;
        }
        QThread::msleep(50);
    }

    return 0;
}

#include "test-autotype.moc"
