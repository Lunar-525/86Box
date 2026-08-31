/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Memory Usage indicator dialog - shows the live host process
 *          memory footprint of the emulator (RSS / virtual memory) together
 *          with the emulated machine's RAM and BIOS ROM sizing.
 *
 *          See mem_usage_* in 86box.c.
 */
#include "qt_memusage.hpp"
#include "ui_qt_memusage.h"

extern "C" {
#include <86box/86box.h>
}

#include <QTimer>

#include <cstdio>

static void
format_bytes(char *buf, size_t len, uint64_t bytes)
{
    if (bytes >= (1024ULL * 1024ULL * 1024ULL))
        snprintf(buf, len, "%.2f GB", (double) bytes / (1024.0 * 1024.0 * 1024.0));
    else
        snprintf(buf, len, "%.1f MB", (double) bytes / (1024.0 * 1024.0));
}

MemUsage::MemUsage(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::MemUsage)
{
    ui->setupUi(this);

    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &MemUsage::updateValues);
    timer->start(250);

    updateValues();
}

MemUsage::~MemUsage()
{
    delete ui;
}

void
MemUsage::updateValues()
{
    char tmp[64];

    format_bytes(tmp, sizeof(tmp), mem_usage_rss_get());
    ui->rssValue->setText(tmp);

    format_bytes(tmp, sizeof(tmp), mem_usage_vms_get());
    ui->vmsValue->setText(tmp);

    format_bytes(tmp, sizeof(tmp), mem_guest_ram_get());
    ui->ramValue->setText(tmp);

    format_bytes(tmp, sizeof(tmp), mem_guest_rom_get());
    ui->romValue->setText(tmp);
}
