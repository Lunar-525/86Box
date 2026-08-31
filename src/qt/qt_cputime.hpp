#ifndef QT_CPUTIME_HPP
#define QT_CPUTIME_HPP

#include <QDialog>

#include <cstdint>

class QTimer;

namespace Ui {
class CPUTime;
}

class CPUTime : public QDialog {
    Q_OBJECT

public:
    explicit CPUTime(QWidget *parent = nullptr);
    ~CPUTime();

private slots:
    void updateValues();
    void on_resetButton_clicked();

private:
    Ui::CPUTime *ui;
    QTimer      *timer;

    /* Accumulator bases, so the "window" values are deltas between two
       refreshes and the "since opened" values are deltas from opening. */
    uint64_t last_guest_ns = 0;
    uint64_t last_real_ns  = 0;
    uint64_t base_guest_ns = 0;
    uint64_t base_real_ns  = 0;
};

#endif // QT_CPUTIME_HPP
