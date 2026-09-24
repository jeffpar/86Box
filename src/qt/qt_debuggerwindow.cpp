/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Internal Debugger Window (IDW).
 *
 *          A simple console: h (or Ctrl-C) halts the emulated CPU, and typed
 *          commands are passed to src/idw.c, which runs them on the CPU
 *          thread and sends its output back through idw_output().
 */
#include "qt_debuggerwindow.hpp"

#include <QApplication>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTextCursor>
#include <QVBoxLayout>
#include <QWindow>

#include <atomic>

extern "C" {
#include <86box/86box.h>
#include <86box/keyboard.h>
#include <86box/plat.h>
#include <86box/idw.h>
}

static std::atomic<DebuggerWindow *> instance { nullptr };

/* The physical Control key; Qt reports it as Meta on macOS, where Ctrl means Command. */
#ifdef Q_OS_MACOS
static constexpr Qt::KeyboardModifier breakModifier = Qt::MetaModifier;
#else
static constexpr Qt::KeyboardModifier breakModifier = Qt::ControlModifier;
#endif

extern "C" void
idw_output(const char *text)
{
    if (!instance.load())
        return;

    /* Called on the CPU thread, so hand the text to the UI thread. */
    QString str = QString::fromUtf8(text);
    QMetaObject::invokeMethod(
        qApp, [str] {
            if (DebuggerWindow *window = instance.load())
                window->appendOutput(str);
        },
        Qt::QueuedConnection);
}

void
DebuggerWindow::showWindow(QWidget *parent)
{
    DebuggerWindow *window = instance.load();
    if (!window) {
        window = new DebuggerWindow(parent);
        instance.store(window);
    }

    window->show();
    window->raise();
    window->activateWindow();
    window->input->setFocus();
}

/* True for the window, its widgets, and its native window, whose key events
   MainWindow's application-wide filter would otherwise send to the machine. */
bool
DebuggerWindow::isOwnObject(const QObject *obj)
{
    const DebuggerWindow *window = instance.load();

    if (!window || !obj)
        return false;
    if (obj == window->windowHandle())
        return true;
    for (; obj; obj = obj->parent()) {
        if (obj == window)
            return true;
    }
    return false;
}

DebuggerWindow::DebuggerWindow(QWidget *parent)
    : QWidget(parent, Qt::Window)
{
    const QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);

    setWindowTitle(tr("86Box Internal Debugger"));
    resize(760, 480);

    output = new QPlainTextEdit(this);
    output->setReadOnly(true);
    output->setFont(font);
    output->setLineWrapMode(QPlainTextEdit::NoWrap);
    output->setMaximumBlockCount(10000);
    output->setFocusPolicy(Qt::ClickFocus);

    input = new QLineEdit(this);
    input->setFont(font);
    input->setPlaceholderText(tr("Command (? for help)"));

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(output);
    layout->addWidget(input);

    connect(input, &QLineEdit::returnPressed, this, &DebuggerWindow::submitCommand);
    input->installEventFilter(this);
    output->installEventFilter(this);
}

DebuggerWindow::~DebuggerWindow()
{
    DebuggerWindow *self = this;
    instance.compare_exchange_strong(self, nullptr);
}

void
DebuggerWindow::changeEvent(QEvent *event)
{
    /* Keys held down in the machine's window won't see their release here. */
    if ((event->type() == QEvent::ActivationChange) && isActiveWindow())
        keyboard_all_up();

    QWidget::changeEvent(event);
}

void
DebuggerWindow::appendOutput(const QString &text)
{
    /* Append without moving the view's cursor, so any selection survives. */
    QTextCursor cursor(output->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text);
    output->verticalScrollBar()->setValue(output->verticalScrollBar()->maximum());
}

void
DebuggerWindow::submitCommand()
{
    QString line = input->text().trimmed();

    /* Enter on an empty line repeats the last command. */
    input->clear();
    if (line.isEmpty()) {
        if (history.isEmpty())
            return;
        line = history.last();
    }

    if (history.isEmpty() || (history.last() != line))
        history.append(line);
    historyPos = history.size();

    /* A bare t or p isn't echoed, so a series of them shows one line per step. */
    if (line.compare("t", Qt::CaseInsensitive) && line.compare("p", Qt::CaseInsensitive))
        appendOutput(QString("> %1\n").arg(line));
    if (dopause)
        appendOutput(tr("Emulation is paused (Action > Pause); the command will run when it resumes\n"));
    idw_command(line.toUtf8().constData());
}

bool
DebuggerWindow::eventFilter(QObject *obj, QEvent *event)
{
    if ((event->type() != QEvent::ShortcutOverride) && (event->type() != QEvent::KeyPress))
        return QWidget::eventFilter(obj, event);

    const auto *keyEvent = static_cast<QKeyEvent *>(event);
    const auto  mods     = keyEvent->modifiers() & ~Qt::KeypadModifier;

    if ((keyEvent->key() == Qt::Key_C) && (mods == breakModifier)) {
        /* Where Ctrl-C also means Copy, let it copy selected output. */
        if ((obj == output) && (breakModifier == Qt::ControlModifier) && output->textCursor().hasSelection())
            return QWidget::eventFilter(obj, event);

        /* Claim the key from the Copy shortcut, then act on the key press. */
        event->accept();
        if (event->type() == QEvent::KeyPress) {
            appendOutput("^C\n");
            if (dopause)
                appendOutput(tr("Emulation is paused (Action > Pause); the CPU will stop when it resumes\n"));
            idw_break();
        }
        return true;
    }

    /* Up and Down recall earlier commands. */
    if ((obj == input) && (event->type() == QEvent::KeyPress) && !history.isEmpty()) {
        if (keyEvent->key() == Qt::Key_Up) {
            historyPos = qMax(0, historyPos - 1);
            input->setText(history.at(historyPos));
            return true;
        }
        if (keyEvent->key() == Qt::Key_Down) {
            historyPos = qMin(static_cast<int>(history.size()), historyPos + 1);
            input->setText((historyPos < history.size()) ? history.at(historyPos) : QString());
            return true;
        }
    }

    return QWidget::eventFilter(obj, event);
}
