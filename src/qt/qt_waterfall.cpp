/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Bus Waterfall viewer - a scrolling spectrogram-like timeline of
 *          physical bus activity: X = physical address (0..16 MB), Y = time
 *          slices (newest at the top), fed by the gated bus_act bitmap in
 *          mem.c. DMA/PIO data streams show up as vertical "rivers".
 */
#include "qt_waterfall.hpp"
#include "ui_qt_waterfall.h"

extern "C" {
#include <86box/mem.h>
}

#include <QCheckBox>
#include <QPainter>
#include <QPixmap>
#include <QSpinBox>
#include <QTimer>

#include <cstdio>
#include <cstring>

static constexpr uint64_t BUS_RANGE = 16ULL * 1024 * 1024; /* 16 MB */
static constexpr uint32_t PAGES_PER_COL = (uint32_t) (BUS_RANGE / Waterfall::COLS / 4096);

Waterfall::Waterfall(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::Waterfall)
{
    ui->setupUi(this);

    mem_access_set_enabled(1);
    bus_act_set_enabled(1);

    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &Waterfall::refreshView);
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

    if (image.isNull())
        image = QImage(COLS, TOTAL_H, QImage::Format_RGB32);

    refreshView();
}

Waterfall::~Waterfall()
{
    bus_act_set_enabled(0);
    mem_access_set_enabled(0);
    delete ui;
}

void
Waterfall::refreshView()
{
    mem_access_ensure_sized();
    mem_access_scan_ptes(); /* feeds bus_act + heat via the access marks */
    bus_act_ensure_sized();

    /* Build the newest time slice: per column, count touched pages. */
    uint8_t new_row[COLS];
    uint8_t *act = bus_act_get();
    const uint32_t act_pages = bus_act_pages_get();
    for (int c = 0; c < COLS; c++) {
        uint32_t count = 0;
        if (act) {
            const uint32_t p0 = (uint32_t) c * PAGES_PER_COL;
            const uint32_t p1 = p0 + PAGES_PER_COL;
            for (uint32_t p = p0; (p < p1) && (p < act_pages); p++)
                if (act[p])
                    count++;
        }
        new_row[c] = (uint8_t) count;
    }
    bus_act_clear();

    /* Scroll down (newest at top) and insert the new slice. */
    std::memmove(wf[1], wf[0], (DATA_ROWS - 1) * COLS);
    std::memcpy(wf[0], new_row, COLS);

    /* Render. */
    if (image.isNull())
        image = QImage(COLS, TOTAL_H, QImage::Format_RGB32);

    for (int y = RULER_H; y < TOTAL_H; y++) {
        QRgb *line = (QRgb *) image.scanLine(y);
        const uint8_t *row = wf[y - RULER_H];
        for (int x = 0; x < COLS; x++) {
            const double t = (double) row[x] / (double) PAGES_PER_COL;
            const int g = (int) (255.0 * t);
            const int b = (int) (170.0 * t);
            line[x] = qRgb(0, g, b); /* dark -> green -> cyan */
        }
    }

    /* Address ruler on the top band. */
    {
        QPainter p(&image);
        p.fillRect(0, 0, COLS, RULER_H, QColor(10, 10, 10));
        p.setPen(QColor(255, 255, 255, 140));
        p.setFont(QFont(font().family(), 7));
        for (int m = 0; m <= 16; m += 4) {
            const int x = (int) ((int64_t) m * COLS / 16);
            p.drawLine(x, 0, x, RULER_H - 1);
            char tmp[8];
            snprintf(tmp, sizeof(tmp), "%dM", m);
            p.drawText(QRect(x + 1, 0, 24, RULER_H), Qt::AlignLeft | Qt::AlignVCenter, QString::fromLatin1(tmp));
        }
        p.drawLine(0, RULER_H - 1, COLS - 1, RULER_H - 1);
    }

    ui->waterfall->setPixmap(QPixmap::fromImage(image.scaled(ui->waterfall->size(),
                                                             Qt::KeepAspectRatio,
                                                             Qt::FastTransformation)));
}
