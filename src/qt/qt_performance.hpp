#ifndef QT_PERFORMANCE_HPP
#define QT_PERFORMANCE_HPP

#include <QDialog>
#include <QElapsedTimer>
#include <QImage>
#include <QString>
#include <QWidget>

#include <cstdint>

class QPaintEvent;
class QTimer;

/* Horizontal gauge with 100% in the middle: below 100% fills to the left
   (slower than real time), above 100% fills to the right (faster than real
   time). Values beyond 200% clamp the fill and draw a ">>" marker. */
class CenterGauge : public QWidget {
public:
    explicit CenterGauge(QWidget *parent = nullptr);

    void setValue(double value); /* 100.0 = center */
    void setText(const QString &text);

protected:
    void paintEvent(QPaintEvent *event) override;
    QSize sizeHint() const override;

private:
    double  value = 100.0;
    QString text;
};

namespace Ui {
class Performance;
}

/* CPU speed gauge, approximate L1/L2 cache monitors and working-set view,
   fed by the gated simulators. */
class Performance : public QDialog {
    Q_OBJECT

public:
    explicit Performance(QWidget *parent = nullptr);
    ~Performance();

private slots:
    void updateValues();

private:
    void renderCacheMap();
    void renderL2Map();

    Ui::Performance *ui;
    QTimer          *timer;

    /* CPU speed. */
    uint64_t last_guest_ns = 0;
    uint64_t last_real_ns  = 0;

    /* L1 cache. */
    QImage   cacheMapImage;
    uint64_t last_hits   = 0;
    uint64_t last_misses = 0;

    /* L2 cache. */
    QImage   l2MapImage;
    uint64_t last_l2_hits   = 0;
    uint64_t last_l2_misses = 0;
};

#endif // QT_PERFORMANCE_HPP
