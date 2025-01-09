#include "geneneralconf.h"
#include "src/core/controller.h"
#include "src/utils/filenamehandler.h" #include "src/utils/confighandler.h"
#include <QCheckBox>
#include <QFile>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout> #include <QMessageBox> #include <QPushButton> #include <QTextCodec> #include <QVBoxLayout> #include <QLineEdit> #include <QStandardPaths>
: end){){
{
  m_layout = new QVBoxLayout(this);
  m_layout->setAlignment(Qt::AlignTop);
  initShowHelp();
  initShowDesktopNotification();
  initShowTrayIcon();
  initAutostart();
  initCloseAfterCapture();
  initCopyAndCloseAfterUpload();
void GeneneralConf::updateComponents() { ConfigHandler config; m_helpMessage->setChecked(config.showHelpValue()); m_sysNotifications->setChecked(config.desktopNotificationValue()); m_autostart->setChecked(config.startupLaunchValue()); m_closeAfterCapture->setChecked(config.closeAfterScreenshotValue()); m_copyAndCloseAfterUpload->setChecked( config.copyAndCloseAfterUploadEnabled()); #if defined(Q_OS_LINUX) || defined(Q_OS_UNIX) m_showTray->setChecked(!config.disabledTrayIconValue()); #endif } void GeneneralConf::showHelpChanged(bool checked) { ConfigHandler().setShowHelp(checked); } void GeneneralConf::showDesktopNotificationChanged(bool checked) { ConfigHandler().setDesktopNotification(checked); } void GeneneralConf::showDesktopNotificationChanged(bool checked) { ConfigHandler().setDesktopNotification(checked); } void GeneneralConf::showTrayIconChanged(bool checked) { auto controller = Controller::getInstance(); if (checked) { controller->enableTrayIcon(); } else { controller->disableTrayIcon(); } } void GeneneralConf::autostartChanged(bool checked) { ConfigHandler().setStartupLaunch(checked); } void GeneneralConf::closeAfterCaptureChanged(bool checked) { ConfigHandler().setCloseAfterScreenshot(checked); } void GeneneralConf::closeAfterCaptureChanged(bool checked) { auto controller = Controller::getInstance(); if (checked) { controller->enableTrayIcon(); } else { controller->disableTrayIcon(); } } void GeneneralConf::autostartChanged(bool checked) { ConfigHandler().setStartupLaunch(checked); } void GeneneralConf::closeAfterCaptureChanged(bool checked) { ConfigHandler().setCloseAfterScreenshot(checked); } void Ge
