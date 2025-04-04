#include "shortcutswidget.h"
#include "setshortcutwidget.h"
#include <QHeaderView>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QStringList>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QVector>
#if (QT_VERSION >= QT_VERSION_CHECK(5, 10, 0))
#include <QCursor>
#include <QGuiApplication>
#include <QRect>
#include <QScreen>
#endif
ShortcutsWidget::ShortcutsWidget(QWidget* parent)
  : QWidget(parent)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowIcon(QIcon(":img/app/flameshot.svg"));
    setWindowTitle(tr("Hot Keys"));
#if (QT_VERSION >= QT_VERSION_CHECK(5, 10, 0))
    QRect position = frameGeometry();
    QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    position.moveCenter(screen->availableGeometry().center());
    move(position.topLeft());
#endif
    m_layout = new QVBoxLayout(this);
    m_layout->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    m_shortcuts = m_config.shortcuts();
    initInfoTable();
    show();
}
const QVector<QStringList>& ShortcutsWidget::shortcuts()
{
    return m_shortcuts;
}
void ShortcutsWidget::initInfoTable()
{
    m_table = new QTableWidget(this);
    m_table->setToolTip(tr("Available shortcuts in the screen capture mode."));
    m_layout->addWidget(m_table);
    m_table->setColumnCount(2);
    m_table->setRowCount(m_shortcuts.size());
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setFocusPolicy(Qt::NoFocus);
    m_table->verticalHeader()->hide();
    QStringList names;
    names << tr("Description") << tr("Key");
    m_table->setHorizontalHeaderLabels(names);
    connect(m_table,
            SIGNAL(cellClicked(int, int)),
            this,
            SLOT(slotShortcutCellClicked(int, int)));
    for (int i = 0; i < shortcuts().size(); ++i) {
        const auto current_shortcut = m_shortcuts.at(i);
        const auto identifier = current_shortcut.at(0);
        const auto description = current_shortcut.at(1);
        const auto default_key_sequence = current_shortcut.at(2);
        m_table->setItem(i, 0, new QTableWidgetItem(description));
<<<<<<< HEAD
        const auto key_sequence = identifier.isEmpty()
                                    ? default_key_sequence
                                    : m_config.shortcut(identifier);
        QTableWidgetItem* item = new QTableWidgetItem(key_sequence);
||||||| d1274add
        QTableWidgetItem* item = new QTableWidgetItem(m_shortcuts.at(i).at(2));
=======
#if (defined(Q_OS_MAC) || defined(Q_OS_MAC64) || defined(Q_OS_MACOS) || \
     defined(Q_OS_MACX))
        QTableWidgetItem* item =
          new QTableWidgetItem(nativeOSHotKeyText(m_shortcuts.at(i).at(2)));
#else
        QTableWidgetItem* item = new QTableWidgetItem(m_shortcuts.at(i).at(2));
#endif
>>>>>>> b1ba67ac
        item->setTextAlignment(Qt::AlignCenter);
        m_table->setItem(i, 1, item);
        if (identifier.isEmpty()) {
            QFont font;
            font.setBold(true);
            item->setFont(font);
            item->setFlags(item->flags() ^ Qt::ItemIsEnabled);
            m_table->item(i, 1)->setFont(font);
        }
    }
    for (int x = 0; x < m_table->rowCount(); ++x) {
        for (int y = 0; y < m_table->columnCount(); ++y) {
            QTableWidgetItem* item = m_table->item(x, y);
            item->setFlags(item->flags() ^ Qt::ItemIsEditable);
        }
    }
    m_table->resizeColumnsToContents();
    m_table->resizeRowsToContents();
    m_table->horizontalHeader()->setMinimumSectionSize(200);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSizePolicy(QSizePolicy::Expanding,
                                               QSizePolicy::Expanding);
}
void ShortcutsWidget::slotShortcutCellClicked(int row, int col)
{
    if (col == 1) {
        if (Qt::ItemIsEnabled !=
            (Qt::ItemIsEnabled & m_table->item(row, col)->flags())) {
            return;
        }
        SetShortcutDialog* setShortcutDialog = new SetShortcutDialog();
        if (0 != setShortcutDialog->exec()) {
            QString shortcutName = m_shortcuts.at(row).at(0);
            QKeySequence shortcutValue = setShortcutDialog->shortcut();
            if (shortcutValue == QKeySequence(Qt::Key_Backspace)) {
                shortcutValue = QKeySequence("");
            }
            if (m_config.setShortcut(shortcutName, shortcutValue.toString())) {
#if (defined(Q_OS_MAC) || defined(Q_OS_MAC64) || defined(Q_OS_MACOS) || \
     defined(Q_OS_MACX))
                QTableWidgetItem* item = new QTableWidgetItem(
                  nativeOSHotKeyText(shortcutValue.toString()));
#else
                QTableWidgetItem* item =
                  new QTableWidgetItem(shortcutValue.toString());
#endif
                item->setTextAlignment(Qt::AlignCenter);
                item->setFlags(item->flags() ^ Qt::ItemIsEditable);
                m_table->setItem(row, col, item);
            }
        }
        delete setShortcutDialog;
    }
}
