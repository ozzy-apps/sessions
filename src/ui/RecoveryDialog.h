#pragma once
#include <QDialog>
class QTreeWidget;
class QPushButton;
namespace sessions {
class RecoveryManager;
class RecoveryDialog : public QDialog {
public:
    RecoveryDialog(RecoveryManager* recovery, QWidget* parent = nullptr);
    QString selectedFile() const;
private:
    void refresh();
    RecoveryManager* m_recovery;
    QTreeWidget* m_list;
    QPushButton* m_open;
    QPushButton* m_delete;
};
}
