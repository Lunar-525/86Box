/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          CPU Time indicator dialog - a more precise virtual CPU
 *          performance monitor than the built-in title-bar percentage.
 *
 *          It reports, per refresh window and since opening, how much real
 *          time was spent to emulate n ms of guest CPU time: if emulating
 *          n ms of simulated time actually took x% of n ms of real time,
 *          then x% > 100 means the emulation is slower than real time and
 *          x% < 100 means it runs ahead of real time.
 *
 *          Only the cpu_exec() CPU-execution phase is measured; display, UI
 *          and input processing time is excluded (see cpu_time_* in 86box.c).
 *          Timer callbacks fired between instruction blocks are included.
 */
#include "qt_cputime.hpp"
#include "ui_qt_cputime.h"

extern "C" {
#include <86box/86box.h>
}

#include <QTimer>

#include <cstdio>

CPUTime::CPUTime(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::CPUTime)
{
    ui->setupUi(this);

    base_guest_ns = cpu_time_guest_ns_get();
    base_real_ns  = cpu_time_real_ns_get();
    last_guest_ns = base_guest_ns;
    last_real_ns  = base_real_ns;

    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &CPUTime::updateValues);
    timer->start(250);

    updateValues();
}

CPUTime::~CPUTime()
{
    delete ui;
}

void
CPUTime::updateValues()
{
    const uint64_t guest_ns = cpu_time_guest_ns_get();
    const uint64_t real_ns  = cpu_time_real_ns_get();

    /* Defensive re-base in case the counters were reset elsewhere. */
    if ((guest_ns < last_guest_ns) || (real_ns < last_real_ns)) {
        last_guest_ns = guest_ns;
        last_real_ns  = real_ns;
        base_guest_ns = guest_ns;
        base_real_ns  = real_ns;
    }

    const uint64_t window_guest = guest_ns - last_guest_ns;
    const uint64_t window_real  = real_ns - last_real_ns;
    const uint64_t total_guest  = guest_ns - base_guest_ns;
    const uint64_t total_real   = real_ns - base_real_ns;

    last_guest_ns = guest_ns;
    last_real_ns  = real_ns;

    const double window_guest_ms = (double) window_guest / 1000000.0;
    const double window_real_ms  = (double) window_real / 1000000.0;
    const double total_guest_ms  = (double) total_guest / 1000000.0;
    const double total_real_ms   = (double) total_real / 1000000.0;

    char tmp[128];

    /* Window values. */
    snprintf(tmp, sizeof(tmp), "%.1f ms", window_guest_ms);
    ui->windowGuestValue->setText(tmp);

    if (window_guest) {
        const double pct = (double) window_real / (double) window_guest * 100.0;
        const double spd = (double) window_guest / (double) window_real * 100.0;
        snprintf(tmp, sizeof(tmp), "%.2f ms (%.1f%% of simulated time)", window_real_ms, pct);
        ui->windowRealValue->setText(tmp);
        snprintf(tmp, sizeof(tmp), "%.1f%%", spd);
        ui->windowSpeedValue->setText(tmp);
    } else {
        ui->windowRealValue->setText(tr("— (emulation idle or paused)"));
        ui->windowSpeedValue->setText(tr("—"));
    }

    /* Totals since the dialog was opened (or last reset). */
    snprintf(tmp, sizeof(tmp), "%.1f ms", total_guest_ms);
    ui->totalGuestValue->setText(tmp);

    if (total_guest) {
        const double pct = (double) total_real / (double) total_guest * 100.0;
        const double spd = (double) total_guest / (double) total_real * 100.0;
        snprintf(tmp, sizeof(tmp), "%.2f ms (%.1f%% of simulated time)", total_real_ms, pct);
        ui->totalRealValue->setText(tmp);
        snprintf(tmp, sizeof(tmp), "%.1f%%", spd);
        ui->totalSpeedValue->setText(tmp);
    } else {
        ui->totalRealValue->setText(tr("—"));
        ui->totalSpeedValue->setText(tr("—"));
    }
}

void
CPUTime::on_resetButton_clicked()
{
    cpu_time_reset();
    last_guest_ns = 0;
    last_real_ns  = 0;
    base_guest_ns = 0;
    base_real_ns  = 0;
    updateValues();
}
