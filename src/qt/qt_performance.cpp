/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Performance indicator dialog - combines a virtual CPU speed
 *          gauge (100% in the middle) with a memory usage panel (host
 *          process RSS/VM and emulated machine RAM/ROM, color coded).
 *
 *          Data comes from the cpu_time_* and mem_usage_* accessors in
 *          86box.c.
 */
#include "qt_performance.hpp"
#include "ui_qt_performance.h"

extern "C" {
#include <86box/86box.h>
}

#include <QPainter>
#include <QTimer>

#include <algorithm>
#include <cstdio>

/* ------------------------------------------------------------------ */
/* CenterGauge: horizontal bar, 100% in the middle.                    */
/* ------------------------------------------------------------------ */

CenterGauge::CenterGauge(QWidget *parent)
    : QWidget(parent)
{
    setMinimumHeight(44);
}

void
CenterGauge::setValue(double v)
{
    if (v != value) {
        value = v;
        update();
    }
}

void
CenterGauge::setText(const QString &t)
{
    if (t != text) {
        text = t;
        update();
    }
}

QSize
CenterGauge::sizeHint() const
{
    return QSize(280, 44);
}

void
CenterGauge::paintEvent(QPaintEvent *event)
{
    (void) event;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    /* Reserve the top strip for the bold value text, bar below it. */
    const QRectF bar(rect().adjusted(0, 18, 0, -4));
    const double mid_x  = bar.left() + bar.width() / 2.0;
    const double half_w = bar.width() / 2.0;

    /* Background trough. */
    p.setPen(Qt::NoPen);
    p.setBrush(palette().color(QPalette::Mid));
    p.drawRoundedRect(bar, bar.height() / 2.0, bar.height() / 2.0);

    /*
     * Fill length = |speed - 100|, measured from the centre.
     *   speed < 100  ->  fill to the LEFT  (slower than real time)
     *   speed > 100  ->  fill to the RIGHT (faster than real time)
     *   speed == 100 ->  thin centre marker
     * Taking |speed-100| (not speed/100) means the gauge is symmetric and the
     * fill is exactly proportional to the amount above/below real time.
     */
    const double over = value - 100.0; /* in [-100, +100] surplus */
    const double w    = half_w * std::min(1.0, std::abs(over) / 100.0);
    QRectF       fill;
    QColor       fill_color;
    if (over < 0.0) {
        fill       = QRectF(mid_x - w, bar.top(), w, bar.height());
        fill_color = QColor(0xe5, 0x5c, 0x5c); /* red  : slower than real time */
    } else if (over > 0.0) {
        fill       = QRectF(mid_x, bar.top(), w, bar.height());
        fill_color = QColor(0x4c, 0xaf, 0x50); /* green: faster than real time */
    } else {
        fill       = QRectF(mid_x - 1.0, bar.top(), 2.0, bar.height());
        fill_color = QColor(0x21, 0x96, 0xf3); /* blue : exactly real time */
    }
    p.setBrush(fill_color);
    p.drawRoundedRect(fill, fill.height() / 2.0, fill.height() / 2.0);

    /* Centre line at 100%. */
    p.setPen(QPen(palette().color(QPalette::Dark), 1));
    p.drawLine(QPointF(mid_x, bar.top()), QPointF(mid_x, bar.bottom()));

    /* End scale labels only (they never overlap the bar content). */
    p.setPen(palette().color(QPalette::WindowText));
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() - 1.0);
    p.setFont(f);
    p.drawText(QRectF(bar.left(), bar.top(), 40, bar.height()), Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("0%"));
    p.drawText(QRectF(bar.right() - 40, bar.top(), 40, bar.height()), Qt::AlignVCenter | Qt::AlignRight, QStringLiteral("200%"));

    /* Value text, bold, centred in the reserved top strip. */
    if (!text.isEmpty()) {
        QFont tf = font();
        tf.setBold(true);
        p.setFont(tf);
        p.drawText(QRectF(0, 0, width(), 18), Qt::AlignCenter, text);
    }
}

/* ------------------------------------------------------------------ */
/* Performance dialog.                                                 */
/* ------------------------------------------------------------------ */

Performance::Performance(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::Performance)
{
    ui->setupUi(this);

    last_guest_ns = cpu_time_guest_ns_get();
    last_real_ns  = cpu_time_real_ns_get();

    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &Performance::updateValues);
    connect(ui->refreshSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this](int ms) { timer->start(ms); });
    timer->start(ui->refreshSpin->value());

    updateValues();
}

Performance::~Performance()
{
    delete ui;
}

void
Performance::updateValues()
{
    char tmp[128];

    /* --- CPU speed gauge + last refresh summary --- */
    const uint64_t guest_ns = cpu_time_guest_ns_get();
    const uint64_t real_ns  = cpu_time_real_ns_get();

    if ((guest_ns < last_guest_ns) || (real_ns < last_real_ns)) {
        last_guest_ns = guest_ns;
        last_real_ns  = real_ns;
    }

    const uint64_t window_guest = guest_ns - last_guest_ns;
    const uint64_t window_real  = real_ns - last_real_ns;

    last_guest_ns = guest_ns;
    last_real_ns  = real_ns;

    if (window_guest && window_real) {
        const double speed = (double) window_guest / (double) window_real * 100.0;
        const double pct   = (double) window_real / (double) window_guest * 100.0;

        ui->cpuGauge->setValue(speed);
        snprintf(tmp, sizeof(tmp), "%.1f%%", speed);
        ui->cpuGauge->setText(QString::fromLatin1(tmp));

        snprintf(tmp, sizeof(tmp),
                 "Last refresh: %.1f ms simulated, %.2f ms real time (%.1f%% of simulated time), speed %.1f%%",
                 (double) window_guest / 1000000.0, (double) window_real / 1000000.0, pct, speed);
        ui->cpuInfoLabel->setText(QString::fromLatin1(tmp));
    } else {
        ui->cpuGauge->setValue(100.0);
        ui->cpuGauge->setText(QStringLiteral("—"));
        ui->cpuInfoLabel->setText(tr("Last refresh: emulation idle or paused"));
    }
}
