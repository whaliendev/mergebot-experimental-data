#include "configwindow.h"
#include "src/config/filenameeditor.h"
#include "src/config/geneneralconf.h"
#include "src/config/shortcutswidget.h"
#include "src/config/strftimechooserwidget.h"
#include "src/config/uploadstorageconfig.h"
#include "src/config/visualseditor.h"
#include "src/utils/colorutils.h"
#include "src/utils/confighandler.h"
#include "src/utils/globalvalues.h"
#include "src/utils/pathinfo.h"
#include "src/widgets/capture/capturetoolbutton.h"
#include <QFileSystemWatcher>
#include <QLabel>
#include <QIcon>
#include <QKeyEvent>
#include <QVBoxLayout>
ConfigWindow::ConfigWindow(QWidget* parent)
  : QTabWidget(parent)
{
    setAttribute(Qt::WA_DeleteOnClose);
<<<<<<< HEAD
    setMinimumSize(GlobalValues::buttonBaseSize() * 14,
                   GlobalValues::buttonBaseSize() * 12);
    setWindowIcon(QIcon(":img/app/flameshot.svg"));
||||||| 2bc3d826
    const int size = GlobalValues::buttonBaseSize() * 12;
    setMinimumSize(size, size);
    setWindowIcon(QIcon(":img/app/flameshot.svg"));
=======
    const int size = GlobalValues::buttonBaseSize() * 12;
    setMinimumSize(size, size);
    setWindowIcon(QIcon(":img/app/org.flameshot.Flameshot.svg"));
>>>>>>> ac7142c4
    setWindowTitle(tr("Configuration"));
    auto changedSlot = [this](QString s) {
        QStringList files = m_configWatcher->files();
        if (!files.contains(s)) {
            this->m_configWatcher->addPath(s);
        }
        emit updateChildren();
    };
    m_configWatcher = new QFileSystemWatcher(this);
    m_configWatcher->addPath(ConfigHandler().configFilePath());
    connect(
      m_configWatcher, &QFileSystemWatcher::fileChanged, this, changedSlot);
    QColor background = this->palette().window().color();
    bool isDark = ColorUtils::colorIsDark(background);
    QString modifier =
      isDark ? PathInfo::whiteIconPath() : PathInfo::blackIconPath();
    m_visuals = new VisualsEditor();
    addTab(m_visuals, QIcon(modifier + "graphics.svg"), tr("Interface"));
    m_filenameEditor = new FileNameEditor();
    addTab(m_filenameEditor,
           QIcon(modifier + "name_edition.svg"),
           tr("Filename Editor"));
    m_generalConfig = new GeneneralConf();
    addTab(m_generalConfig, QIcon(modifier + "config.svg"), tr("General"));
    m_shortcuts = new ShortcutsWidget();
    addTab(m_shortcuts, QIcon(modifier + "shortcut.svg"), tr("Shortcuts"));
    m_uploadStorageConfig = new UploadStorageConfig();
    addTab(m_uploadStorageConfig,
           QIcon(modifier + "cloud-upload.svg"),
           tr("Storage"));
    connect(this,
            &ConfigWindow::updateChildren,
            m_filenameEditor,
            &FileNameEditor::updateComponents);
    connect(this,
            &ConfigWindow::updateChildren,
            m_visuals,
            &VisualsEditor::updateComponents);
    connect(this,
            &ConfigWindow::updateChildren,
            m_generalConfig,
            &GeneneralConf::updateComponents);
}
void ConfigWindow::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Escape) {
        close();
    }
}
