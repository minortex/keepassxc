#include <QApplication>
#include <QLineEdit>
#include <QTextStream>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);

    QLineEdit field;
    field.setWindowTitle(QStringLiteral("libei receiver (%1)").arg(app.platformName()));
    field.resize(640, 80);
    field.show();
    field.setFocus();

    QTextStream out(stdout);
    out << "X11 receiver ready (platform=" << app.platformName() << "). Type into this window." << Qt::endl;
    out.flush();

    QObject::connect(&field, &QLineEdit::textChanged, [&out](const QString& text) {
        out << "received: " << text << Qt::endl;
        out.flush();
    });

    return app.exec();
}
