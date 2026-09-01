/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Performance — CPU & Caches view: virtual CPU speed gauge (100% in
 *          the middle), approximate L1/L2 cache monitors (hit rate, sets x
 *          ways cache map) and a working-set view. Data comes from the
 *          cpu_time_*, cache_sim_* and mem_access_* modules.
 */
#include "qt_performance.hpp"
#include "ui_qt_performance.h"

extern "C" {
#include <86box/86box.h>
#include <86box/mem.h>
}

#include <QCheckBox>
#include <QGroupBox>
#include <QPainter>
#include <QPixmap>
#include <QProgressBar>
#include <QSpinBox>
#include <QTimer>

#include <algorithm>
#include <cstdio>

/* Three-stop heat colour: green (recent) -> orange (mid) -> red (old).
   t in [0,1], 0 = most recently used, 1 = least recently used. */
static QRgb
heatColor(double t)
{
    static const QColor g(0x4c, 0xaf, 0x50), o(0xff, 0x98, 0x00), r(0xe5, 0x39, 0x35);
    const QColor      from = (t < 0.5) ? g : o;
    const QColor      to   = (t < 0.5) ? o : r;
    const double      k    = (t < 0.5) ? (t * 2.0) : ((t - 0.5) * 2.0);
    return QColor((int) (from.red() + (to.red() - from.red()) * k),
                  (int) (from.green() + (to.green() - from.green()) * k),
                  (int) (from.blue() + (to.blue() - from.blue()) * k))
        .rgb();
}

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

    /* Fill length = |speed - 100|, measured from the centre:
       slower than real time fills left (red), faster fills right (green). */
    const double over = value - 100.0;
    const double w    = half_w * std::min(1.0, std::abs(over) / 100.0);
    QRectF       fill;
    QColor       fill_color;
    if (over < 0.0) {
        fill       = QRectF(mid_x - w, bar.top(), w, bar.height());
        fill_color = QColor(0xe5, 0x5c, 0x5c);
    } else if (over > 0.0) {
        fill       = QRectF(mid_x, bar.top(), w, bar.height());
        fill_color = QColor(0x4c, 0xaf, 0x50);
    } else {
        fill       = QRectF(mid_x - 1.0, bar.top(), 2.0, bar.height());
        fill_color = QColor(0x21, 0x96, 0xf3);
    }
    p.setBrush(fill_color);
    p.drawRoundedRect(fill, fill.height() / 2.0, fill.height() / 2.0);

    /* Centre line at 100%. */
    p.setPen(QPen(palette().color(QPalette::Dark), 1));
    p.drawLine(QPointF(mid_x, bar.top()), QPointF(mid_x, bar.bottom()));

    /* End scale labels only. */
    p.setPen(palette().color(QPalette::WindowText));
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() - 1.0);
    p.setFont(f);
    p.drawText(QRectF(bar.left(), bar.top(), 40, bar.height()), Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("0%"));
    p.drawText(QRectF(bar.right() - 40, bar.top(), 40, bar.height()), Qt::AlignVCenter | Qt::AlignRight, QStringLiteral("200%"));

    /* Beyond 200% the fill is clamped: mark it with ">>". */
    if (over > 100.0) {
        p.setPen(QColor(0x4c, 0xaf, 0x50));
        QFont mf = f;
        mf.setBold(true);
        p.setFont(mf);
        p.drawText(QRectF(bar.right() - 44, bar.top(), 40, bar.height()), Qt::AlignVCenter | Qt::AlignRight, QStringLiteral(">>"));
    }

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

    /* Enable the gated monitors while this window is open. */
    mem_access_set_enabled(1);
    cache_sim_set_enabled(1);

    last_guest_ns = cpu_time_guest_ns_get();
    last_real_ns  = cpu_time_real_ns_get();

    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &Performance::updateValues);
    connect(ui->refreshSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this](int ms) {
                if (ui->liveCheck->isChecked())
                    timer->start(ms);
            });
    connect(ui->liveCheck, &QCheckBox::toggled, this, [this](bool on) {
        if (on)
            timer->start(ui->refreshSpin->value());
        else
            timer->stop();
    });
    timer->start(ui->refreshSpin->value());

    updateValues();
}

Performance::~Performance()
{
    cache_sim_set_enabled(0);
    mem_access_set_enabled(0);
    delete ui;
}

void
Performance::renderCacheMap()
{
    const int sets  = cache_sim_sets_get();
    const int assoc = cache_sim_assoc_get();
    if (!sets || !assoc) {
        ui->l1Map->setText(QStringLiteral("—"));
        return;
    }

    uint8_t *map = new uint8_t[(size_t) sets * assoc];
    cache_sim_map_get(map);

    /* L1 is small, so render it as a true sets x ways grid. */
    if ((cacheMapImage.width() != sets) || (cacheMapImage.height() != assoc))
        cacheMapImage = QImage(sets, assoc, QImage::Format_RGB32);

    const QRgb empty_col = qRgb(0x55, 0x55, 0x55);
    for (int s = 0; s < sets; s++) {
        for (int w = 0; w < assoc; w++) {
            const uint8_t v = map[s * assoc + w];
            cacheMapImage.setPixel(s, w, v ? heatColor((double) (v - 1) / (double) (assoc - 1)) : empty_col);
        }
    }
    delete[] map;

    ui->l1Map->setPixmap(QPixmap::fromImage(cacheMapImage.scaled(ui->l1Map->size(),
                                                                 Qt::KeepAspectRatio,
                                                                 Qt::FastTransformation)));
}

void
Performance::renderL2Map()
{
    const int sets = cache_sim_l2_active() ? (cache_sim_l2_size_get() / cache_sim_l2_assoc_get() / 32) : 0;
    if (!sets) {
        ui->l2Map->setText(QStringLiteral("—"));
        return;
    }

    uint8_t *heat = new uint8_t[(size_t) sets];
    cache_sim_l2_set_heat_get(heat);

    /* L2 is large (thousands of sets); reflow it into a compact 2D "map".
       Each cell = one set, coloured by global access recency:
       h=255 -> recently touched (green), h=1 -> long-untouched (red). */
    const int cols = 128;
    const int rows = (sets + cols - 1) / cols;
    if ((l2MapImage.width() != cols) || (l2MapImage.height() != rows))
        l2MapImage = QImage(cols, rows, QImage::Format_RGB32);

    const QRgb empty_col = qRgb(0x55, 0x55, 0x55);
    for (int s = 0; s < sets; s++) {
        const uint8_t h = heat[s];
        const int     x = s % cols;
        const int     y = s / cols;
        if (y < rows)
            l2MapImage.setPixel(x, y, h ? heatColor(1.0 - (double) (h - 1) / 254.0) : empty_col);
    }
    delete[] heat;

    ui->l2Map->setPixmap(QPixmap::fromImage(l2MapImage.scaled(ui->l2Map->size(),
                                                              Qt::KeepAspectRatio,
                                                              Qt::FastTransformation)));
}

void
Performance::updateValues()
{
    char tmp[128];

    /* Keep the access marks current (working set). */
    mem_access_ensure_sized();
    mem_access_scan_ptes();
    uint8_t *heat = mem_access_heat_get();
    const uint32_t pages = mem_access_pages_get();
    if (heat) {
        for (uint32_t i = 0; i < pages; i++)
            heat[i] = (uint8_t) ((heat[i] * 225u) >> 8);
    }

    /* --- CPU speed gauge + refresh summary --- */
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

        snprintf(tmp, sizeof(tmp), "%.2f ms", (double) window_guest / 1000000.0);
        ui->simValue->setText(QString::fromLatin1(tmp));
        snprintf(tmp, sizeof(tmp), "%.2f ms", (double) window_real / 1000000.0);
        ui->realValue->setText(QString::fromLatin1(tmp));
        snprintf(tmp, sizeof(tmp), "%.1f%% of simulated time", pct);
        ui->pctValue->setText(QString::fromLatin1(tmp));
    } else {
        ui->cpuGauge->setValue(100.0);
        ui->cpuGauge->setText(QStringLiteral("—"));
        ui->simValue->setText(QStringLiteral("—"));
        ui->realValue->setText(QStringLiteral("—"));
        ui->pctValue->setText(tr("emulation idle or paused"));
    }

    /* --- L1 cache --- */
    const int size  = cache_sim_size_get();
    const int assoc = cache_sim_assoc_get();
    const int line  = cache_sim_line_get();

    if (!size) {
        ui->l1GeoValue->setText(tr("CPU has no L1 cache"));
        ui->l1HitValue->setText(QStringLiteral("—"));
        ui->l1HitBar->setValue(0);
        ui->l1WCountsValue->setText(QStringLiteral("—"));
        ui->l1TCountsValue->setText(QStringLiteral("—"));
        ui->l1Map->setText(QStringLiteral("—"));
        ui->l2GeoValue->setText(tr("CPU has no modelled L2"));
        ui->l2HitValue->setText(QStringLiteral("—"));
        ui->l2HitBar->setValue(0);
        ui->l2WCountsValue->setText(QStringLiteral("—"));
        ui->l2TCountsValue->setText(QStringLiteral("—"));
        ui->l2Map->setText(QStringLiteral("—"));
        ui->wsValue->setText(QStringLiteral("—"));
        ui->wsL1Value->setText(QStringLiteral("—"));
        ui->wsBar->setValue(0);
        ui->ws2Value->setText(QStringLiteral("—"));
        ui->ws2Bar->setValue(0);
    } else {
        snprintf(tmp, sizeof(tmp), "%d KB, %d-way, %d B line", size / 1024, assoc, line);
        ui->l1GeoValue->setText(QString::fromLatin1(tmp));

        const uint64_t h  = cache_sim_hits_get();
        const uint64_t m  = cache_sim_misses_get();
        const uint64_t wh = h - last_hits;
        const uint64_t wm = m - last_misses;
        last_hits          = h;
        last_misses        = m;

        if (wh + wm) {
            const double rate = (double) wh / (double) (wh + wm) * 100.0;
            snprintf(tmp, sizeof(tmp), "%.2f%%", rate);
            ui->l1HitValue->setText(QString::fromLatin1(tmp));
            ui->l1HitBar->setValue((int) rate);
        } else {
            ui->l1HitValue->setText(tr("idle"));
            ui->l1HitBar->setValue(0);
        }
        snprintf(tmp, sizeof(tmp), "%llu / %llu",
                 (unsigned long long) wh, (unsigned long long) wm);
        ui->l1WCountsValue->setText(QString::fromLatin1(tmp));
        snprintf(tmp, sizeof(tmp), "%llu / %llu",
                 (unsigned long long) h, (unsigned long long) m);
        ui->l1TCountsValue->setText(QString::fromLatin1(tmp));

        renderCacheMap();

        uint32_t ws_pages = 0;
        if (heat) {
            for (uint32_t i = 0; i < pages; i++)
                if (heat[i])
                    ws_pages++;
        }
        const uint64_t ws_kb = (uint64_t) ws_pages * 4;
        snprintf(tmp, sizeof(tmp), "%llu KB", (unsigned long long) ws_kb);
        ui->wsValue->setText(QString::fromLatin1(tmp));

        const int l1_kb      = std::max(1, size / 1024);
        const uint64_t l1pct = ws_kb * 100 / (uint64_t) l1_kb;
        snprintf(tmp, sizeof(tmp), "%llu KB (%llu%% - %s)", (unsigned long long) ws_kb,
                 (unsigned long long) l1pct, (l1pct <= 100) ? "fits" : "too big");
        ui->wsL1Value->setText(QString::fromLatin1(tmp));
        ui->wsBar->setValue((int) std::min<uint64_t>(100, l1pct));
        ui->wsBar->setStyleSheet(QString::fromLatin1(
            "QProgressBar::chunk { background-color: %1; }")
            .arg((l1pct <= 100) ? QStringLiteral("#4CAF50") : QStringLiteral("#E53935")));

        /* --- L2 --- */
        if (!cache_sim_l2_active()) {
            ui->l2GeoValue->setText(tr("CPU has no modelled L2"));
            ui->l2HitValue->setText(QStringLiteral("—"));
            ui->l2HitBar->setValue(0);
            ui->l2WCountsValue->setText(QStringLiteral("—"));
            ui->l2TCountsValue->setText(QStringLiteral("—"));
            ui->l2Map->setText(QStringLiteral("—"));
            ui->ws2Value->setText(QStringLiteral("—"));
            ui->ws2Bar->setValue(0);
        } else {
            const int l2_size  = cache_sim_l2_size_get();
            const int l2_assoc = cache_sim_l2_assoc_get();
            snprintf(tmp, sizeof(tmp), "%d KB, %d-way, 32 B line", l2_size / 1024, l2_assoc);
            ui->l2GeoValue->setText(QString::fromLatin1(tmp));

            const uint64_t l2h  = cache_sim_l2_hits_get();
            const uint64_t l2m  = cache_sim_l2_misses_get();
            const uint64_t wl2h = l2h - last_l2_hits;
            const uint64_t wl2m = l2m - last_l2_misses;
            last_l2_hits          = l2h;
            last_l2_misses        = l2m;

            if (wl2h + wl2m) {
                const double rate = (double) wl2h / (double) (wl2h + wl2m) * 100.0;
                snprintf(tmp, sizeof(tmp), "%.2f%%", rate);
                ui->l2HitValue->setText(QString::fromLatin1(tmp));
                ui->l2HitBar->setValue((int) rate);
            } else {
                ui->l2HitValue->setText(tr("idle"));
                ui->l2HitBar->setValue(0);
            }
            snprintf(tmp, sizeof(tmp), "%llu / %llu",
                     (unsigned long long) wl2h, (unsigned long long) wl2m);
            ui->l2WCountsValue->setText(QString::fromLatin1(tmp));
            snprintf(tmp, sizeof(tmp), "%llu / %llu",
                     (unsigned long long) l2h, (unsigned long long) l2m);
            ui->l2TCountsValue->setText(QString::fromLatin1(tmp));

            renderL2Map();

            const int l2_kb      = std::max(1, l2_size / 1024);
            const uint64_t l2pct = ws_kb * 100 / (uint64_t) l2_kb;
            snprintf(tmp, sizeof(tmp), "%llu KB (%llu%% - %s)", (unsigned long long) ws_kb,
                     (unsigned long long) l2pct, (l2pct <= 100) ? "fits" : "too big");
            ui->ws2Value->setText(QString::fromLatin1(tmp));
            ui->ws2Bar->setValue((int) std::min<uint64_t>(100, l2pct));
            ui->ws2Bar->setStyleSheet(QString::fromLatin1(
                "QProgressBar::chunk { background-color: %1; }")
                .arg((l2pct <= 100) ? QStringLiteral("#4CAF50") : QStringLiteral("#E53935")));
        }
    }
}
