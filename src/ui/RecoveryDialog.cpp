#include "RecoveryDialog.h"
#include "core/Recovery.h"
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace sessions {
RecoveryDialog::RecoveryDialog(RecoveryManager* recovery, QWidget* parent) : QDialog(parent), m_recovery(recovery) {
    setObjectName("recoveryDialog"); setWindowTitle("Recover Set"); resize(760, 390); setMinimumSize(600, 280);
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(18,18,18,18); layout->setSpacing(12);
    auto* hint = new QLabel("Recover opens an unsaved copy. The original set stays unchanged.", this); hint->setWordWrap(true); layout->addWidget(hint);
    m_list = new QTreeWidget(this); m_list->setObjectName("recoveryList"); m_list->setRootIsDecorated(false); m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setHeaderLabels({"Set", "Snapshot", "Original / status"}); m_list->setColumnWidth(0,180); m_list->setColumnWidth(1,160); m_list->header()->setStretchLastSection(true); layout->addWidget(m_list,1);
    auto* buttons = new QDialogButtonBox(this);
    m_delete = buttons->addButton("Delete…", QDialogButtonBox::DestructiveRole); m_delete->setObjectName("recoveryDelete");
    auto* later = buttons->addButton("Later", QDialogButtonBox::RejectRole); later->setObjectName("recoveryLater");
    m_open = buttons->addButton("Recover", QDialogButtonBox::AcceptRole); m_open->setObjectName("recoveryOpen"); m_open->setDefault(true); layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::accepted,this,&QDialog::accept); connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);
    connect(m_list,&QTreeWidget::itemSelectionChanged,this,[this]{auto* item=m_list->currentItem();const bool selected=item&&!item->data(0,Qt::UserRole).toString().isEmpty();m_delete->setEnabled(selected);m_open->setEnabled(selected&&item->data(0,Qt::UserRole+1).toString().isEmpty());});
    connect(m_list,&QTreeWidget::itemDoubleClicked,this,[this]{if(m_open->isEnabled())accept();});
    connect(m_delete,&QPushButton::clicked,this,[this]{
        if(QMessageBox::question(this,"Delete Recovery","Delete all recovery snapshots for this session?",QMessageBox::Yes|QMessageBox::Cancel,QMessageBox::Cancel)!=QMessageBox::Yes)return;
        QString error;if(!m_recovery->discard(selectedFile(),&error))QMessageBox::warning(this,"Delete Recovery",error);refresh();
    });
    refresh();
}
QString RecoveryDialog::selectedFile() const { const auto* item=m_list->currentItem();return item?item->data(0,Qt::UserRole).toString():QString(); }
void RecoveryDialog::refresh() {
    m_list->clear(); QTreeWidgetItem* preferred=nullptr;
    for(const auto& entry:m_recovery->candidates()){
        auto* item=new QTreeWidgetItem(m_list,{entry.name,entry.time.toLocalTime().toString("yyyy-MM-dd HH:mm:ss"),entry.error.isEmpty()?(entry.originalPath.isEmpty()?"Untitled set":entry.originalPath):entry.error});
        item->setData(0,Qt::UserRole,entry.file);item->setData(0,Qt::UserRole+1,entry.error);item->setToolTip(2,item->text(2));if(!preferred&&entry.error.isEmpty())preferred=item;
    }
    if(m_list->topLevelItemCount()==0){auto* empty=new QTreeWidgetItem(m_list,{"No recovery snapshots"});empty->setFlags(Qt::NoItemFlags);}
    if(!preferred)preferred=m_list->topLevelItem(0);m_list->setCurrentItem(preferred);
    m_open->setEnabled(preferred&&!preferred->data(0,Qt::UserRole).toString().isEmpty()&&preferred->data(0,Qt::UserRole+1).toString().isEmpty());
    m_delete->setEnabled(preferred&&!preferred->data(0,Qt::UserRole).toString().isEmpty());
}
}
