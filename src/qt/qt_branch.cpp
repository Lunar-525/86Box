/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Branch predictor (BHT) viewer (approximate): prediction accuracy,
 *          taken rate and a 2-bit-saturating-counter map, fed by the gated
 *          branch marks in the interpreter Jcc macros (branch_sim_* in cpu.c).
 */
#include "qt_branch.hpp"
#include "ui_qt_branch.h"

extern "C" {
#include <86box/86box.h>
}

#include <QCheckBox>
#include <QPixmap>
#include <QProgressBar>
#include <QSpinBox>
#include <QTimer>

#include <algorithm>
#include <cstdio>

BranchPred::BranchPred(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::BranchPred)
{
    ui->setupUi(this);

    branch_sim_set_enabled(1);

    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &BranchPred::refreshView);
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

BranchPred::~BranchPred()
{
    branch_sim_set_enabled(0);
    delete ui;
}

void
BranchPred::renderMap()
{
    const int entries = branch_sim_entries_get();
    if (entries <= 0) {
        ui->bhtMap->setText(QStringLiteral("—"));
        return;
    }

    uint8_t *map = new uint8_t[(size_t) entries];
    branch_sim_map_get(map);

    const int cols = 64;
    const int rows = (entries + cols - 1) / cols;
    if ((mapImage.width() != cols) || (mapImage.height() != rows))
        mapImage = QImage(cols, rows, QImage::Format_RGB32);

    static const QRgb state_colors[4] = {
        qRgb(0x1a, 0x23, 0x7e), /* 00 strong not-taken */
        qRgb(0x42, 0xa5, 0xf5), /* 01 weakly not-taken */
        qRgb(0xff, 0xa7, 0x26), /* 10 weakly taken */
        qRgb(0xe5, 0x39, 0x35), /* 11 strong taken */
    };

    for (int y = 0; y < rows; y++) {
        QRgb *line = (QRgb *) mapImage.scanLine(y);
        for (int x = 0; x < cols; x++) {
            const int i = y * cols + x;
            line[x] = (i < entries) ? state_colors[map[i] & 3] : qRgb(0, 0, 0);
        }
    }

    delete[] map;

    ui->bhtMap->setPixmap(QPixmap::fromImage(mapImage.scaled(ui->bhtMap->size(),
                                                             Qt::KeepAspectRatio,
                                                             Qt::FastTransformation)));
}

void
BranchPred::refreshView()
{
    char tmp[96];

    const uint64_t total     = branch_sim_total_get();
    const uint64_t correct   = branch_sim_correct_get();
    const uint64_t incorrect = branch_sim_incorrect_get();
    const uint64_t taken     = branch_sim_taken_get();

    /* Window deltas (since last refresh). */
    const uint64_t wt = total - last_total;
    const uint64_t wc = correct - last_correct;
    const uint64_t wi = incorrect - last_incorrect;
    last_total         = total;
    last_correct       = correct;
    last_incorrect     = incorrect;

    if (wt) {
        const double acc = (double) wc / (double) wt * 100.0;
        snprintf(tmp, sizeof(tmp), "%.2f%%", acc);
        ui->accValue->setText(QString::fromLatin1(tmp));
        ui->accBar->setValue((int) acc);
    } else {
        ui->accValue->setText(tr("idle"));
        ui->accBar->setValue(0);
    }

    if (total) {
        const double tr = (double) taken / (double) total * 100.0;
        snprintf(tmp, sizeof(tmp), "%.2f%%  (%llu taken / %llu total)",
                 tr, (unsigned long long) taken, (unsigned long long) total);
        ui->takenValue->setText(QString::fromLatin1(tmp));
    } else
        ui->takenValue->setText(QStringLiteral("—"));

    snprintf(tmp, sizeof(tmp), "%llu / %llu (window),  %llu / %llu (total)",
             (unsigned long long) wc, (unsigned long long) wi,
             (unsigned long long) correct, (unsigned long long) incorrect);
    ui->countsValue->setText(QString::fromLatin1(tmp));

    renderMap();
}
