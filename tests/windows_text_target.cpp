#include <QApplication>
#include <QTextCursor>
#include <QTextEdit>
#include <QTimer>

int main(int argument_count, char* arguments[]) {
    QApplication application(argument_count, arguments);

    QTextEdit editor;
    editor.setWindowTitle(QStringLiteral("SenseVoice Qt TSF target"));
    editor.setPlainText(QStringLiteral("开头待替换结尾"));
    editor.resize(480, 180);
    editor.show();
    editor.raise();
    editor.activateWindow();
    editor.setFocus(Qt::OtherFocusReason);

    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(2);
    cursor.setPosition(5, QTextCursor::KeepAnchor);
    editor.setTextCursor(cursor);

    const QString expected = QStringLiteral("开头第一次注入，第二次继续结尾");
    auto* poll = new QTimer(&application);
    QObject::connect(poll, &QTimer::timeout, &application, [&application, &editor, expected] {
        if (editor.toPlainText() == expected) application.exit(0);
    });
    poll->start(20);
    QTimer::singleShot(10'000, &application, [&application] { application.exit(1); });
    return application.exec();
}
