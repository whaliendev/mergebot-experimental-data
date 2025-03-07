// Copyright(c) 2017-2019 Alejandro Sirgo Rica & Contributors
//
// This file is part of Flameshot.
//
//     Flameshot is free software: you can redistribute it and/or modify
//     it under the terms of the GNU General Public License as published by
//     the Free Software Foundation, either version 3 of the License, or
//     (at your option) any later version.
//
//     Flameshot is distributed in the hope that it will be useful,
//     but WITHOUT ANY WARRANTY; without even the implied warranty of
//     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//     GNU General Public License for more details.
//
//     You should have received a copy of the GNU General Public License
//     along with Flameshot.  If not, see <http://www.gnu.org/licenses/>.

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
