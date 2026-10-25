#pragma once
#include "core/Updates.h"
#include <QDialog>
namespace sessions {
class UpdateDialog : public QDialog {
    Q_OBJECT
  public:
    enum Choice { Postpone = QDialog::Rejected, Download = QDialog::Accepted, Skip = 2 };
    explicit UpdateDialog(const ReleaseInfo& release, QWidget* parent = nullptr);
};
} // namespace sessions
