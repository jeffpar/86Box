#ifndef QT_DEBUGGERWINDOW_HPP
#define QT_DEBUGGERWINDOW_HPP

#include <QStringList>
#include <QWidget>

class QLineEdit;
class QPlainTextEdit;

/* The Internal Debugger Window (IDW); see src/idw.c for the commands. */
class DebuggerWindow final : public QWidget {
    Q_OBJECT

public:
    static void showWindow(QWidget *parent);
    static bool isOwnObject(const QObject *obj);
    ~DebuggerWindow() override;

public slots:
    void appendOutput(const QString &text);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    explicit DebuggerWindow(QWidget *parent);
    void submitCommand();

    QPlainTextEdit *output;
    QLineEdit      *input;
    QStringList     history;
    int             historyPos = 0;
};

#endif // QT_DEBUGGERWINDOW_HPP
