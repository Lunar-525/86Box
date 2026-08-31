/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Memory Map viewer - renders the guest physical RAM (ram[]) as a
 *          grayscale image (one cell per 4KB page) with a decaying access
 *          heat overlay. The heat is fed by the guest page-table
 *          Accessed/Dirty bits while paging is enabled and by interpreter
 *          write marks while in real mode (see mem_access_* in mem.c).
 *          Access tracking is gated on this window being open.
 */
#include "qt_memorymap.hpp"
#include "ui_qt_memorymap.h"

extern "C" {
#include <86box/86box.h>
#include <86box/mem.h>
}

#include <QCheckBox>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>

#include <cmath>
#include <cstdio>

MemoryMap::MemoryMap(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::MemoryMap)
{
    ui->setupUi(this);

    mem_access_set_enabled(1);

    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &MemoryMap::refreshView);
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

MemoryMap::~MemoryMap()
{
    mem_access_set_enabled(0);
    delete ui;
}

void
MemoryMap::refreshView()
{
    /* Re-sync the heat array if the machine's RAM configuration changed. */
    mem_access_ensure_sized();

    /* Paged-mode heat source: feed heat from the guest page tables (this
       covers accesses from both the interpreter and the JIT). */
    mem_access_scan_ptes();

    /* Decay existing heat so activity visibly fades over a few seconds. */
    uint8_t *heat = mem_access_heat_get();
    const uint32_t pages = mem_access_pages_get();
    if (heat) {
        for (uint32_t i = 0; i < pages; i++)
            heat[i] = (uint8_t) ((heat[i] * 230u) >> 8); /* ~0.9 per tick */
    }

    renderImage();
    updateStatus();
}

void
MemoryMap::renderImage()
{
    const uint32_t pages = mem_access_pages_get();
    if (!pages || !ram)
        return;

    uint8_t *heat = mem_access_heat_get();
    const int cols = (int) std::ceil(std::sqrt((double) pages));
    const int rows = (int) ((pages + (uint32_t) cols - 1) / (uint32_t) cols);
    if (cols < 1)
        return;

    if ((image.width() != cols) || (image.height() != rows))
        image = QImage(cols, rows, QImage::Format_RGB32);

    const uint8_t *r = ram;
    for (int y = 0; y < rows; y++) {
        QRgb *line = (QRgb *) image.scanLine(y);
        for (int x = 0; x < cols; x++) {
            const uint32_t page = (uint32_t) (y * cols + x);
            if (page >= pages) {
                line[x] = qRgb(0, 0, 0);
                continue;
            }

            /* Content: mean of 16 samples spread across the page. */
            const uint8_t *base = r + ((size_t) page << 12);
            uint32_t sum = 0;
            for (int s = 0; s < 16; s++)
                sum += base[(s * 256) & 0xfff];
            const uint8_t grey = (uint8_t) (sum >> 4);

            const uint8_t h = heat ? heat[page] : 0;
            if (h) {
                /* Blend towards red/orange by heat. */
                const int rv = grey + (((255 - grey) * (int) h) >> 8);
                const int gv = (grey * (255 - (int) h)) >> 8;
                const int bv = (grey * (255 - (int) h)) >> 8;
                line[x] = qRgb(rv, gv, bv);
            } else
                line[x] = qRgb(grey, grey, grey);
        }
    }

    ui->memImage->setPixmap(QPixmap::fromImage(image.scaled(ui->memImage->size(),
                                                            Qt::KeepAspectRatio,
                                                            Qt::FastTransformation)));
}

void
MemoryMap::updateStatus()
{
    char tmp[160];
    const uint32_t pages = mem_access_pages_get();
    const uint64_t ram_bytes = mem_guest_ram_get();
    snprintf(tmp, sizeof(tmp), "Guest RAM: %.1f MB (%u pages, %dx%d grid) - %s",
             (double) ram_bytes / (1024.0 * 1024.0), pages,
             image.width(), image.height(),
             mem_access_paging_enabled() ? "paging on (page-table scan)" : "real mode (write tracking)");
    ui->statusLabel->setText(QString::fromLatin1(tmp));
}

void
MemoryMap::on_resetButton_clicked()
{
    mem_access_clear_heat();
    renderImage();
}
