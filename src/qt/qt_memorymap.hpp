#ifndef QT_MEMORYMAP_HPP
#define QT_MEMORYMAP_HPP

#include <QDialog>
#include <QImage>
#include <QWidget>

class QPaintEvent;
class QTimer;

/* Detailed diagram of the first 1MB of guest RAM: content brightness with a
   heat overlay, classic x86 region bands (conventional / video / adapter ROM
   / BIOS) and address ticks. */
class MemDetailStrip : public QWidget {
public:
    explicit MemDetailStrip(QWidget *parent = nullptr);
    void setHeatEnabled(bool enabled);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    bool heat_on = true;
};

namespace Ui {
class MemoryMap;
}

/* Memory Map viewer: renders the guest physical RAM (ram[]) as a grayscale
   image (one cell per 4KB page) and overlays a decaying access heat map.
   Heat sources (auto-switched): guest page-table Accessed/Dirty bits while
   paging is enabled, per-access write marks in the interpreter while in real
   mode. Access tracking is enabled only while this window is open. */
class MemoryMap : public QDialog {
    Q_OBJECT

public:
    explicit MemoryMap(QWidget *parent = nullptr);
    ~MemoryMap();

private slots:
    void refreshView();
    void on_resetButton_clicked();

private:
    void renderImage();
    void updateStatus();

    Ui::MemoryMap *ui;
    QTimer        *timer;
    QImage         image;
};

#endif // QT_MEMORYMAP_HPP
