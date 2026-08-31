#ifndef QT_CHIPSET_HPP
#define QT_CHIPSET_HPP

#include <QDialog>

#include <array>
#include <cstdint>

class QLabel;
class QProgressBar;
class QTimer;
class QVBoxLayout;

namespace Ui {
class Chipset;
}

/* Chipset (bridge) activity viewer: shows per-IRQ-line interrupt activity
   (8259 PIC), per-channel DMA activity (8237) and per-slot PCI config-space
   access counts. Counters are incremented at the event sites only while this
   window is open (see chipset_mon_* in 86box.c). */
class Chipset : public QDialog {
    Q_OBJECT

public:
    explicit Chipset(QWidget *parent = nullptr);
    ~Chipset();

private slots:
    void refreshView();

private:
    void addRow(QVBoxLayout *layout, const QString &name, int idx,
                std::array<QProgressBar *, 32> &bars, std::array<QLabel *, 32> &vals,
                const char *color);

    Ui::Chipset *ui;
    QTimer      *timer;

    std::array<QProgressBar *, 32> irqBars{};
    std::array<QLabel *, 32>       irqVals{};
    std::array<QProgressBar *, 32> dmaBars{};
    std::array<QLabel *, 32>       dmaVals{};
    std::array<QProgressBar *, 32> pciBars{};
    std::array<QLabel *, 32>       pciVals{};

    std::array<uint32_t, 16> lastIrq{};
    std::array<uint32_t, 8>  lastDma{};
    std::array<uint32_t, 32> lastPci{};
    bool                     first = true;
};

#endif // QT_CHIPSET_HPP
