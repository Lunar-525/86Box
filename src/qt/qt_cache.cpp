/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          CPU L1 cache viewer (approximate): hit rate, a sets x ways cache
 *          map and working-set vs L1-size comparison, fed by the gated cache
 *          simulator in mem.c (cache_sim_*).
 */
#include "qt_cache.hpp"
#include "ui_qt_cache.h"

extern "C" {
#include <86box/mem.h>
}

#include <QCheckBox>
#include <QPixmap>
#include <QProgressBar>
#include <QSpinBox>
#include <QTimer>

#include <algorithm>
#include <cstdio>

Cache::Cache(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::Cache)
{
    ui->setupUi(this);

    /* Enable both the access marks (real-mode feed) and the cache sim. */
    mem_access_set_enabled(1);
    cache_sim_set_enabled(1);

    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &Cache::refreshView);
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

    refreshView();
}

Cache::~Cache()
{
    cache_sim_set_enabled(0);
    mem_access_set_enabled(0);
    delete ui;
}

void
Cache::renderMap()
{
    const int sets  = cache_sim_sets_get();
    const int assoc = cache_sim_assoc_get();
    if (!sets || !assoc) {
        ui->cacheMap->setText(QStringLiteral("—"));
        return;
    }

    uint8_t *map = new uint8_t[(size_t) sets * assoc];
    cache_sim_map_get(map);

    if ((mapImage.width() != sets) || (mapImage.height() != assoc))
        mapImage = QImage(sets, assoc, QImage::Format_RGB32);

    for (int s = 0; s < sets; s++) {
        for (int w = 0; w < assoc; w++) {
            const uint8_t v = map[s * assoc + w];
            QRgb col;
            if (v == 0)
                col = qRgb(0x22, 0x22, 0x22); /* invalid line */
            else {
                /* recency: 1 (MRU) = green .. assoc (LRU) = red */
                const double t = (double) (v - 1) / (double) (assoc - 1);
                col = qRgb((int) (255 * t), (int) (255 * (1.0 - t)), 0);
            }
            mapImage.setPixel(s, w, col);
        }
    }

    delete[] map;

    ui->cacheMap->setPixmap(QPixmap::fromImage(mapImage.scaled(ui->cacheMap->size(),
                                                               Qt::KeepAspectRatio,
                                                               Qt::FastTransformation)));
}

void
Cache::refreshView()
{
    /* Keep the access marks current (paged feed + working set): re-sync the
       heat array, scan the guest page tables and decay heat, mirroring what
       the Memory Map window does. */
    mem_access_ensure_sized();
    mem_access_scan_ptes();
    uint8_t *heat = mem_access_heat_get();
    const uint32_t pages = mem_access_pages_get();
    if (heat) {
        for (uint32_t i = 0; i < pages; i++)
            heat[i] = (uint8_t) ((heat[i] * 225u) >> 8);
    }

    char tmp[96];

    const int size  = cache_sim_size_get();
    const int assoc = cache_sim_assoc_get();
    const int line  = cache_sim_line_get();

    if (!size) {
        ui->geoValue->setText(tr("CPU has no L1 cache"));
        ui->hitValue->setText(QStringLiteral("—"));
        ui->hitBar->setValue(0);
        ui->countsValue->setText(QStringLiteral("—"));
        ui->wsValue->setText(QStringLiteral("—"));
        ui->wsBar->setValue(0);
        ui->cacheMap->setText(QStringLiteral("—"));
        return;
    }

    snprintf(tmp, sizeof(tmp), "%d KB, %d-way, %d B line", size / 1024, assoc, line);
    ui->geoValue->setText(QString::fromLatin1(tmp));

    /* Window hit rate (delta since last refresh). */
    const uint64_t h = cache_sim_hits_get();
    const uint64_t m = cache_sim_misses_get();
    const uint64_t wh = h - last_hits;
    const uint64_t wm = m - last_misses;
    last_hits          = h;
    last_misses        = m;

    if (wh + wm) {
        const double rate = (double) wh / (double) (wh + wm) * 100.0;
        snprintf(tmp, sizeof(tmp), "%.2f%%", rate);
        ui->hitValue->setText(QString::fromLatin1(tmp));
        ui->hitBar->setValue((int) rate);
    } else {
        ui->hitValue->setText(tr("idle"));
        ui->hitBar->setValue(0);
    }
    snprintf(tmp, sizeof(tmp), "%llu / %llu (window),  %llu / %llu (total)",
             (unsigned long long) wh, (unsigned long long) wm,
             (unsigned long long) h, (unsigned long long) m);
    ui->countsValue->setText(QString::fromLatin1(tmp));

    renderMap();

    /* Working set: distinct 4KB pages with heat > 0. */
    uint32_t ws_pages = 0;
    if (heat) {
        for (uint32_t i = 0; i < pages; i++)
            if (heat[i])
                ws_pages++;
    }
    const uint64_t ws_kb = (uint64_t) ws_pages * 4;
    snprintf(tmp, sizeof(tmp), "%llu KB vs L1 %d KB", (unsigned long long) ws_kb, size / 1024);
    ui->wsValue->setText(QString::fromLatin1(tmp));
    ui->wsBar->setValue((int) std::min<uint64_t>(100, ws_kb * 100 / (uint64_t) std::max(1, size / 1024)));
}
