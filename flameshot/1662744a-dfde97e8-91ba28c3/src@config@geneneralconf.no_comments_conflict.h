       
#include <QWidget>
class QVBoxLayout;
class QCheckBox;
class QPushButton;
class QLabel;
class QLineEdit;
class GeneneralConf : public QWidget
{
  Q_OBJECT
public:
  explicit GeneneralConf(QWidget* parent = nullptr);
public slots:
  void updateComponents();
private slots:
<<<<<<< HEAD
  void showHelpChanged(bool checked);
  void showDesktopNotificationChanged(bool checked);
  void showTrayIconChanged(bool checked);
  void autostartChanged(bool checked);
  void closeAfterCaptureChanged(bool checked);
  void importConfiguration();
  void exportFileConfiguration();
  void resetConfiguration();
||||||| 91ba28c3
   void showHelpChanged(bool checked);
   void showDesktopNotificationChanged(bool checked);
   void showTrayIconChanged(bool checked);
   void autostartChanged(bool checked);
   void closeAfterCaptureChanged(bool checked);
   void importConfiguration();
   void exportFileConfiguration();
   void resetConfiguration();
=======
   void showHelpChanged(bool checked);
   void showDesktopNotificationChanged(bool checked);
   void showTrayIconChanged(bool checked);
   void autostartChanged(bool checked);
   void closeAfterCaptureChanged(bool checked);
   void saveAfterCopyChanged(bool checked);
   void changeSavePath();
   void importConfiguration();
   void exportFileConfiguration();
   void resetConfiguration();
>>>>>>> dfde97e8
private:
<<<<<<< HEAD
  QVBoxLayout* m_layout;
  QCheckBox* m_sysNotifications;
  QCheckBox* m_showTray;
  QCheckBox* m_helpMessage;
  QCheckBox* m_autostart;
  QCheckBox* m_closeAfterCapture;
  QCheckBox* m_copyAndCloseAfterUpload;
  QPushButton* m_importButton;
  QPushButton* m_exportButton;
  QPushButton* m_resetButton;
||||||| 91ba28c3
    QVBoxLayout *m_layout;
    QCheckBox *m_sysNotifications;
    QCheckBox *m_showTray;
    QCheckBox *m_helpMessage;
    QCheckBox *m_autostart;
    QCheckBox *m_closeAfterCapture;
    QCheckBox *m_copyAndCloseAfterUpload;
    QPushButton *m_importButton;
    QPushButton *m_exportButton;
    QPushButton *m_resetButton;
=======
    QVBoxLayout *m_layout;
    QCheckBox *m_sysNotifications;
    QCheckBox *m_showTray;
    QCheckBox *m_helpMessage;
    QCheckBox *m_autostart;
    QCheckBox *m_closeAfterCapture;
    QCheckBox *m_copyAndCloseAfterUpload;
    QPushButton *m_importButton;
    QPushButton *m_exportButton;
    QPushButton *m_resetButton;
    QCheckBox *m_saveAfterCopy;
    QLineEdit *m_savePath;
    QPushButton *m_changeSaveButton;
>>>>>>> dfde97e8
<<<<<<< HEAD
  void initShowHelp();
  void initShowDesktopNotification();
  void initShowTrayIcon();
  void initConfingButtons();
  void initAutostart();
  void initCloseAfterCapture();
  void initCopyAndCloseAfterUpload();
||||||| 91ba28c3
    void initShowHelp();
    void initShowDesktopNotification();
    void initShowTrayIcon();
    void initConfingButtons();
    void initAutostart();
    void initCloseAfterCapture();
    void initCopyAndCloseAfterUpload();
=======
    void initShowHelp();
    void initShowDesktopNotification();
    void initShowTrayIcon();
    void initConfingButtons();
    void initAutostart();
    void initCloseAfterCapture();
    void initCopyAndCloseAfterUpload();
    void initSaveAfterCopy();
>>>>>>> dfde97e8
};
