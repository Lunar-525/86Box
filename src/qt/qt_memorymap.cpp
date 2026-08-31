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
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>

#include <cmath>
#include <cstdio>

/* ------------------------------------------------------------------ */
/* MemDetailStrip: detailed diagram of the first 1MB of guest RAM.     */
/* ------------------------------------------------------------------ */

MemDetailStrip::MemDetailStrip(QWidget *parent)
    : QWidget(parent)
{
    setMinimumHeight(72);
}

void
MemDetailStrip::setHeatEnabled(bool enabled)
{
    heat_on = enabled;
    update();
}

void
MemDetailStrip::paintEvent(QPaintEvent *event)
{
    (void) event;
    QPainter p(this);
    p.fillRect(rect(), QColor(20, 20, 20));

    if (!ram)
        return;

    const int w      = width();
    const int band_h = 14;
    const int tick_h = 14;
    const int top    = band_h;
    const int bot    = height() - tick_h;

    uint8_t *heat = mem_access_heat_get();

    /* Content + heat rows (1 pixel per (1MB/w) bytes). */
    for (int x = 0; x < w; x++) {
        const uint32_t start = (uint32_t) (((uint64_t) x * 0x100000) / w);
        const uint32_t end   = (uint32_t) (((uint64_t) (x + 1) * 0x100000) / w);
        const uint32_t span  = (end > start) ? (end - start) : 1;
        uint32_t sum = 0;
        for (int s = 0; s < 8; s++) {
            const uint32_t off = start + (span * (uint32_t) (s + 1)) / 9;
            if (off < 0x100000)
                sum += ram[off];
        }
        uint8_t grey = (uint8_t) (sum >> 3);

        uint8_t hh = 0;
        if (heat_on && heat) {
            uint32_t pg0 = start >> 12;
            uint32_t pg1 = (end - 1) >> 12;
            if (pg1 > 255)
                pg1 = 255;
            for (uint32_t pg = pg0; pg <= pg1; pg++)
                if (heat[pg] > hh)
                    hh = heat[pg];
        }

        if (hh) {
            int r, g, b;
            if (hh <= 128) {
                r = qMin(255, (int) hh * 2);
                g = 255;
                b = 0;
            } else {
                r = 255;
                g = 255 - ((int) (hh - 128) * 2);
                b = 0;
            }
            const int a = (int) hh;
            p.setPen(QColor((grey * (255 - a) + r * a) >> 8,
                            (grey * (255 - a) + g * a) >> 8,
                            (grey * (255 - a) + b * a) >> 8));
        } else
            p.setPen(QColor(grey, grey, grey));
        p.drawLine(x, top, x, bot);
    }

    /* Region band. */
    auto regionColor = [](uint32_t addr, QColor &col, const char **name) {
        if (addr < 0xA0000) {
            col  = QColor(0x2e, 0x7d, 0x32); /* green  : conventional */
            *name = "Conventional";
        } else if (addr < 0xC0000) {
            col  = QColor(0x7b, 0x3f, 0xb0); /* purple : video RAM */
            *name = "Video";
        } else if (addr < 0xF0000) {
            col  = QColor(0x15, 0x63, 0xa8); /* blue   : adapter ROM */
            *name = "Adapter ROM";
        } else {
            col  = QColor(0xb0, 0x2f, 0x2f); /* red    : system BIOS */
            *name = "BIOS";
        }
    };

    for (int x = 0; x < w; x++) {
        const uint32_t addr = (uint32_t) (((uint64_t) x * 0x100000) / w);
        QColor        col;
        const char   *name;
        regionColor(addr, col, &name);
        p.setPen(col);
        p.drawLine(x, 0, x, band_h - 1);
    }

    /* Region labels (one per band). */
    p.setFont(QFont(font().family(), 8));
    auto labelBand = [&](uint32_t start, uint32_t end, const char *text) {
        const int x0 = (int) (((uint64_t) start * w) / 0x100000);
        const int x1 = (int) (((uint64_t) end * w) / 0x100000);
        p.setPen(Qt::white);
        p.drawText(QRect(x0 + 2, 0, x1 - x0 - 4, band_h), Qt::AlignVCenter | Qt::AlignLeft, QString::fromLatin1(text));
    };
    labelBand(0x00000, 0x9FFFF, "Conventional (0-640K)");
    labelBand(0xA0000, 0xBFFFF, "Video");
    labelBand(0xC0000, 0xEFFFF, "Adapter ROM");
    labelBand(0xF0000, 0xFFFFF, "BIOS");

    /* Boundary dividers. */
    p.setPen(QColor(255, 255, 255, 120));
    for (uint32_t a : { 0x9FFFF, 0xA0000, 0xBFFFF, 0xC0000, 0xEFFFF, 0xF0000 })
        p.drawLine((int) (((uint64_t) a * w) / 0x100000), 0, (int) (((uint64_t) a * w) / 0x100000), height() - 1);

    /* Address ticks. */
    struct Tick {
        uint32_t     addr;
        const char  *label;
    };
    static const Tick ticks[] = {
        { 0x00000, "0" }, { 0x9FFFF, "640K" }, { 0xA0000, "" },
        { 0xC0000, "768K" }, { 0xF0000, "960K" }, { 0xFFFFF, "1M" },
    };
    p.setPen(Qt::white);
    for (const Tick &t : ticks) {
        const int x = (int) (((uint64_t) t.addr * w) / 0x100000);
        p.drawLine(x, bot, x, bot + 6);
        p.drawText(QRect(x - 20, bot + 5, 40, tick_h - 5), Qt::AlignHCenter | Qt::AlignTop, QString::fromLatin1(t.label));
    }
}

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
    connect(ui->heatCheck, &QCheckBox::toggled, this, [this](bool on) {
        ui->memDetail->setHeatEnabled(on);
        if (on) {
            mem_access_clear_heat();
            refreshView();
        }
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

    if (ui->heatCheck->isChecked()) {
        /* Paged-mode heat source: feed heat from the guest page tables (this
           covers accesses from both the interpreter and the JIT). */
        mem_access_scan_ptes();

        /* Decay existing heat so activity visibly fades to fully transparent
           after a couple of seconds without access (~0.88 per tick). */
        uint8_t *heat = mem_access_heat_get();
        const uint32_t pages = mem_access_pages_get();
        if (heat) {
            for (uint32_t i = 0; i < pages; i++)
                heat[i] = (uint8_t) ((heat[i] * 225u) >> 8);
        }
    }

    renderImage();
    ui->memDetail->update();
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
            if (h && ui->heatCheck->isChecked()) {
                /* Heat overlay: low heat = green, medium = yellow, high = red;
                   blended with the content by alpha = h/255 so it fades to
                   fully transparent as the heat decays to zero. */
                int r, g, b;
                if (h <= 128) {
                    r = qMin(255, (int) h * 2);
                    g = 255;
                    b = 0;
                } else {
                    r = 255;
                    g = 255 - ((int) (h - 128) * 2);
                    b = 0;
                }
                const int a = (int) h;
                line[x] = qRgb((grey * (255 - a) + r * a) >> 8,
                               (grey * (255 - a) + g * a) >> 8,
                               (grey * (255 - a) + b * a) >> 8);
            } else
                line[x] = qRgb(grey, grey, grey);
        }
    }

    /* Boundary markers on the overview: 1MB (cyan) and 16MB (yellow,
       the classic AT / ISA bus limit) page rows. */
    if (cols >= 2) {
        QPainter p(&image);
        p.setFont(QFont(font().family(), 7));
        p.setPen(QPen(QColor(0, 255, 255), 1));
        if (pages > 256) {
            const int row = (int) (256 / (uint32_t) cols);
            if ((row >= 0) && (row < rows)) {
                p.drawLine(0, row, cols - 1, row);
                p.drawText(QRect(2, row + 1, 64, 9), Qt::AlignLeft, QStringLiteral("1MB"));
            }
        }
        p.setPen(QPen(QColor(255, 255, 0), 1));
        if (pages > 4096) {
            const int row = (int) (4096 / (uint32_t) cols);
            if ((row >= 0) && (row < rows)) {
                p.drawLine(0, row, cols - 1, row);
                p.drawText(QRect(2, row + 1, 64, 9), Qt::AlignLeft, QStringLiteral("16MB"));
            }
        }
    }

    ui->memImage->setPixmap(QPixmap::fromImage(image.scaled(ui->memImage->size(),
                                                            Qt::KeepAspectRatio,
                                                            Qt::SmoothTransformation)));
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
