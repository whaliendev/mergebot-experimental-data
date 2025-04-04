       
#include <QWidget>
class QVBoxLayout
class QCheckBox;
class QPushButton;
class QLabel;
class QLineEdit;
class GeneneralConf : public QWidget {
    Q_OBJECT
    public:
    explicit GeneneralConf(QWidget *parent = nullptr);
public:
    void updateComponents();
private:
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
    void initShowHelp();
    void initShowDesktopNotification();
    void initShowTrayIcon();
    void initConfingButtons();
    void initAutostart();
    void initCloseAfterCapture();
    void initSaveAfterCopy();
    void initCopyAndCloseAfterUpload();
};
