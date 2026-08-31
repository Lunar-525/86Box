#ifndef QT_MEMORYMAP_HPP
#define QT_MEMORYMAP_HPP

#include <QDialog>
#include <QImage>

class QTimer;

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
