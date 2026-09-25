#pragma once
#include <QAbstractButton>
#include <QColor>
#include <QDialog>
#include <QList>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QSettings;
class QSlider;
class QTreeWidget;

// Libellé personnel Gmail proposé dans les paramètres (boîte de démarrage, notifications)
struct LabelChoice {
    QString id, name;
    QColor color;
};

// Vignette cliquable qui dessine une miniature de la fenêtre (thème, densité, disposition)
class PreviewCard : public QAbstractButton
{
    Q_OBJECT
public:
    enum Kind { ThemeCard, DensityCard, LayoutCard };
    PreviewCard(Kind kind, const QString &value, const QString &label, QWidget *parent = nullptr);
    QString value() const { return m_value; }
    QSize sizeHint() const override { return {164, 138}; }

protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override { update(); }
    void leaveEvent(QEvent *) override { update(); }

private:
    Kind m_kind;
    QString m_value;
};

class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    SettingsDialog(QSettings &settings, const QString &email, const QList<LabelChoice> &labels = {},
                   QWidget *parent = nullptr);

    static QString autostartPath();

signals:
    void applied();
    void logoutRequested();
    void switchAccountRequested();
    void configureClientRequested();
    void testNotificationRequested();

private:
    QWidget *displayPage();
    QWidget *accountPage(const QString &email);
    QWidget *generalPage();
    QWidget *privacyPage();
    QButtonGroup *addCards(QLayout *layout, PreviewCard::Kind kind, const QList<QPair<QString, QString>> &options);
    void load();
    void save();
    void writeAutostart(bool enabled, bool minimized);

    QSettings &m_settings;
    QButtonGroup *m_theme, *m_density, *m_layout;
    QCheckBox *m_darkMessages, *m_showSnippet;
    QSlider *m_zoom;
    QLabel *m_zoomLabel;
    QLineEdit *m_senderName;
    QPlainTextEdit *m_signature;
    QCheckBox *m_closeToTray, *m_autostart, *m_startMinimized, *m_notifications;
    QComboBox *m_poll, *m_markRead, *m_remoteImages, *m_startFolder;
    QTreeWidget *m_notifyTree;
    QList<LabelChoice> m_labels;
    QListWidget *m_trusted;
    QLabel *m_addressCount;
    bool m_clearAddresses = false;
};
