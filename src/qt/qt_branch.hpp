#ifndef QT_BRANCH_HPP
#define QT_BRANCH_HPP

#include <QDialog>
#include <QImage>

class QTimer;

namespace Ui {
class BranchPred;
}

/* Branch predictor (BHT) viewer (approximate): prediction accuracy, taken
   rate and a 2-bit-counter map, fed by the gated branch marks in the Jcc
   macros (cpu.c / x86_ops_jump.h). Gated on this window being open. */
class BranchPred : public QDialog {
    Q_OBJECT

public:
    explicit BranchPred(QWidget *parent = nullptr);
    ~BranchPred();

private slots:
    void refreshView();

private:
    void renderMap();

    Ui::BranchPred *ui;
    QTimer         *timer;
    QImage          mapImage;

    uint64_t last_total     = 0;
    uint64_t last_correct   = 0;
    uint64_t last_incorrect = 0;
};

#endif // QT_BRANCH_HPP
