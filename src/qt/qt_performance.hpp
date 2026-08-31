#ifndef QT_PERFORMANCE_HPP
#define QT_PERFORMANCE_HPP

#include <QDialog>
#include <QString>
#include <QWidget>

#include <cstdint>

class QPaintEvent;
class QTimer;

/* Horizontal gauge with 100% in the middle: below 100% fills to the left
   (slower than real time), above 100% fills to the right (faster than real
   time). */
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

class Performance : public QDialog {
    Q_OBJECT

public:
    explicit Performance(QWidget *parent = nullptr);
    ~Performance();

private slots:
    void updateValues();

private:
    Ui::Performance *ui;
    QTimer          *timer;

    uint64_t last_guest_ns = 0;
    uint64_t last_real_ns  = 0;
};

#endif // QT_PERFORMANCE_HPP
