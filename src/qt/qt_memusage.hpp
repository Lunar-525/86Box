#ifndef QT_MEMUSAGE_HPP
#define QT_MEMUSAGE_HPP

#include <QDialog>

class QTimer;

namespace Ui {
class MemUsage;
}

class MemUsage : public QDialog {
    Q_OBJECT

public:
    explicit MemUsage(QWidget *parent = nullptr);
    ~MemUsage();

private slots:
    void updateValues();

private:
    Ui::MemUsage *ui;
    QTimer      *timer;
};

#endif // QT_MEMUSAGE_HPP
