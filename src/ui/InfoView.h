#pragma once
// InfoView: the bottom-left help panel. Any widget with the "info" dynamic
// property ("Title|Body text") shows its help here on hover. InfoView
// installs itself as an application event filter to catch Enter events.

#include <QWidget>

class QLabel;

namespace sessions {

class InfoView : public QWidget {
    Q_OBJECT
public:
    explicit InfoView(QWidget* parent = nullptr);
    ~InfoView() override;
    void show(const QString& title, const QString& body);
    // Shows a message in the window's Info View (what a drop did, why it couldn't); none: nothing.
    static void notify(const QString& title, const QString& body);
    void showDefault();

    // Convenience: attach hover help to any widget.
    static void attach(QWidget* w, const QString& title, const QString& body);

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    QLabel* m_title;
    QLabel* m_body;
};

} // namespace sessions
