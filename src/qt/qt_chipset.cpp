/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Chipset (bridge) activity viewer - per-IRQ-line interrupt
 *          activity (8259 PIC), per-channel DMA activity (8237) and
 *          per-slot PCI config-space access counts. Counters come from
 *          the chipset_mon_* functions in 86box.c.
 */
#include "qt_chipset.hpp"
#include "ui_qt_chipset.h"

extern "C" {
#include <86box/86box.h>
}

#include <QCheckBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdio>

static const char *
irq_name(int i)
{
    switch (i) {
        case 0: return "Timer";   /* PIT */
        case 1: return "Keyboard";/* KBC */
        case 2: return "Cascade"; /* PIC2 */
        case 3: return "COM2";
        case 4: return "COM1";
        case 5: return "LPT2";
        case 6: return "Floppy";
        case 7: return "LPT1";
        case 8: return "RTC";
        case 9: return "Virq";
        case 10: return "Free";
        case 11: return "Free";
        case 12: return "Mouse";
        case 13: return "FPU";
        case 14: return "HDD1";
        case 15: return "HDD2";
        default: return "";
    }
}

void
Chipset::addRow(QVBoxLayout *layout, const QString &name, int idx,
                std::array<QProgressBar *, 32> &bars, std::array<QLabel *, 32> &vals,
                const char *color)
{
    QHBoxLayout *h = new QHBoxLayout;
    h->setSpacing(3);
    h->setContentsMargins(0, 0, 0, 0);
    QLabel *lbl = new QLabel(name);
    lbl->setFixedWidth(96);
    lbl->setStyleSheet(QStringLiteral("font-size: 9px;"));
    QProgressBar *bar = new QProgressBar;
    bar->setRange(0, 100);
    bar->setTextVisible(false);
    bar->setFixedHeight(12);
    bar->setStyleSheet(QString::fromLatin1(
        "QProgressBar { border: 1px solid #555; border-radius: 2px; background: #333; }"
        "QProgressBar::chunk { background-color: %1; border-radius: 1px; }").arg(QString::fromLatin1(color)));
    QLabel *val = new QLabel("0");
    val->setFixedWidth(52);
    val->setStyleSheet(QStringLiteral("font-size: 9px;"));
    val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    h->addWidget(lbl);
    h->addWidget(bar, 1);
    h->addWidget(val);
    layout->addLayout(h);
    bars[idx] = bar;
    vals[idx] = val;
}

Chipset::Chipset(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::Chipset)
{
    ui->setupUi(this);

    chipset_mon_set_enabled(1);

    for (int i = 0; i < 16; i++)
        addRow(ui->irqLayout, QString::asprintf("IRQ %d (%s)", i, irq_name(i)), i,
               irqBars, irqVals, "#4CAF50");
    for (int i = 0; i < 8; i++)
        addRow(ui->dmaLayout, QString::asprintf("DMA %d", i), i,
               dmaBars, dmaVals, "#FF9800");
    for (int i = 0; i < 32; i++)
        addRow(ui->pciLayout, QString::asprintf("PCI slot %d", i), i,
               pciBars, pciVals, "#2196F3");

    /* Compact the rows and group boxes (title must sit above the content,
       so use the documented subcontrol-origin pattern to avoid overlap). */
    for (QVBoxLayout *lay : { ui->irqLayout, ui->dmaLayout, ui->pciLayout }) {
        lay->setSpacing(1);
        lay->setContentsMargins(2, 2, 2, 2);
    }
    const QString gbStyle = QStringLiteral(
        "QGroupBox { margin-top: 9px; padding-top: 3px; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 3px; font-size: 10px; }");
    ui->irqGroup->setStyleSheet(gbStyle);
    ui->dmaGroup->setStyleSheet(gbStyle);
    ui->pciGroup->setStyleSheet(gbStyle);
    ui->contentLayout->setSpacing(4);
    ui->contentLayout->setContentsMargins(2, 2, 2, 2);

    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &Chipset::refreshView);
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

Chipset::~Chipset()
{
    chipset_mon_set_enabled(0);
    delete ui;
}

void
Chipset::refreshView()
{
    uint32_t irqD[16] = {0}, dmaD[8] = {0}, pciD[32] = {0};
    int      irqMax = 1, dmaMax = 1, pciMax = 1;

    for (int i = 0; i < 16; i++) {
        const uint32_t cur = chipset_mon_irq_get(i);
        irqD[i]            = first ? 0 : (cur - lastIrq[i]);
        lastIrq[i]         = cur;
        if ((int) irqD[i] > irqMax)
            irqMax = (int) irqD[i];
    }
    for (int i = 0; i < 8; i++) {
        const uint32_t cur = chipset_mon_dma_get(i);
        dmaD[i]            = first ? 0 : (cur - lastDma[i]);
        lastDma[i]         = cur;
        if ((int) dmaD[i] > dmaMax)
            dmaMax = (int) dmaD[i];
    }
    for (int i = 0; i < 32; i++) {
        const uint32_t cur = chipset_mon_pci_get(i);
        pciD[i]            = first ? 0 : (cur - lastPci[i]);
        lastPci[i]         = cur;
        if ((int) pciD[i] > pciMax)
            pciMax = (int) pciD[i];
    }
    first = false;

    auto setBars = [](int n, int m, const uint32_t *d, std::array<QProgressBar *, 32> &bars,
                      std::array<QLabel *, 32> &vals) {
        for (int i = 0; i < n; i++) {
            if (!bars[i])
                continue;
            bars[i]->setMaximum(m);
            bars[i]->setValue((int) d[i]);
            char tmp[16];
            snprintf(tmp, sizeof(tmp), "%u", d[i]);
            vals[i]->setText(QString::fromLatin1(tmp));
        }
    };

    setBars(16, irqMax, irqD, irqBars, irqVals);
    setBars(8, dmaMax, dmaD, dmaBars, dmaVals);
    setBars(32, pciMax, pciD, pciBars, pciVals);

    char tmp[64];
    snprintf(tmp, sizeof(tmp), "PCI config accesses total: %u (since open)", chipset_mon_pci_total());
    ui->statusLabel->setText(QString::fromLatin1(tmp));
}
