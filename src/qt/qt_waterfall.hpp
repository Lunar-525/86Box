#ifndef QT_WATERFALL_HPP
#define QT_WATERFALL_HPP

#include <QDialog>
#include <QImage>

class QTimer;

namespace Ui {
class Waterfall;
}

/* Bus Waterfall viewer: a scrolling spectrogram-like timeline of physical
   bus activity (0..16 MB on the X axis, time slices on the Y axis, newest
   at the top). Fed by the gated bus_act bitmap (mem.c); DMA/PIO streams
   appear as vertical "rivers" of activity. */
class Waterfall : public QDialog {
    Q_OBJECT

public:
    explicit Waterfall(QWidget *parent = nullptr);
    ~Waterfall();

    enum { COLS = 256, RULER_H = 10, DATA_ROWS = 246, TOTAL_H = 256 };

private slots:
    void refreshView();

private:
    Ui::Waterfall *ui;
    QTimer        *timer;
    QImage         image;
    uint8_t        wf[DATA_ROWS][COLS] = { { 0 } };
};

#endif // QT_WATERFALL_HPP
