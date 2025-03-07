#include "screenshotsaver.h"
#include "src/utils/confighandler.h"
#include "src/utils/filenamehandler.h"
#include "src/utils/systemnotification.h"
#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QImageWriter>
#include <QMessageBox>
ScreenshotSaver::ScreenshotSaver() {}
void
ScreenshotSaver::saveToClipboard(const QPixmap& capture)
{
  if (ConfigHandler().saveAfterCopyValue()) {
    if (!ConfigHandler().saveAfterCopyPathValue().isEmpty()) {
      saveToFilesystem(capture, ConfigHandler().saveAfterCopyPathValue());
    }
  } else {
    SystemNotification().sendMessage(QObject::tr("Capture saved to clipboard"));
  }
  QApplication::clipboard()->setPixmap(capture);
}
bool
ScreenshotSaver::saveToFilesystem(const QPixmap& capture, const QString& path)
{
  QString completePath = FileNameHandler().generateAbsolutePath(path);
  completePath += QLatin1String(".png");
  bool ok = capture.save(completePath);
  QString saveMessage;
  QString notificationPath = completePath;
  if (ok) {
    ConfigHandler().setSavePath(path);
    saveMessage = QObject::tr("Capture saved as ") + completePath;
  } else {
    saveMessage = QObject::tr("Error trying to save as ") + completePath;
    notificationPath = "";
  }
  SystemNotification().sendMessage(saveMessage, notificationPath);
  return ok;
}
bool
ScreenshotSaver::saveToFilesystemGUI(const QPixmap& capture)
{
  bool ok = false;
  while (!ok) {
    QString savePath = QFileDialog::getSaveFileName(
      nullptr,
      QString(),
      FileNameHandler().absoluteSavePath() + ".png",
      QLatin1String("Portable Network Graphic file (PNG) (*.png);;BMP file "
                    "(*.bmp);;JPEG file (*.jpg)"));
    if (savePath.isNull()) {
      break;
    }
    if (!savePath.endsWith(QLatin1String(".png"), Qt::CaseInsensitive) &&
        !savePath.endsWith(QLatin1String(".bmp"), Qt::CaseInsensitive) &&
        !savePath.endsWith(QLatin1String(".jpg"), Qt::CaseInsensitive)) {
      savePath += QLatin1String(".png");
    }
    ok = capture.save(savePath);
    if (ok) {
      QString pathNoFile =
        savePath.left(savePath.lastIndexOf(QLatin1String("/")));
      ConfigHandler().setSavePath(pathNoFile);
      QString msg = QObject::tr("Capture saved as ") + savePath;
      SystemNotification().sendMessage(msg, savePath);
    } else {
      QString msg = QObject::tr("Error trying to save as ") + savePath;
      QMessageBox saveErrBox(
        QMessageBox::Warning, QObject::tr("Save Error"), msg);
      saveErrBox.setWindowIcon(QIcon(":img/app/flameshot.svg"));
      saveErrBox.exec();
    }
  }
  return ok;
}
