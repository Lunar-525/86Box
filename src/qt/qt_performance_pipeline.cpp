/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Branch-predictor (BHT) and pipeline profiler view: BHT map and
 *          accuracy, retired-uOp mix, top instructions and stall-attribution
 *          heuristics. Data comes from the branch_sim_* and pp_* modules.
 */
#include "qt_performance_pipeline.hpp"
#include "ui_qt_performance_pipeline.h"

extern "C" {
#include <86box/86box.h>
#include <86box/mem.h>
}

#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QPainter>
#include <QPixmap>
#include <QProgressBar>
#include <QSpinBox>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdio>

static const char *
pp_opcode_name(uint8_t op)
{
    switch (op) {
        case 0x00: case 0x01: case 0x02: case 0x03: return "ADD";
        case 0x08: case 0x09: case 0x0a: case 0x0b: return "OR";
        case 0x10: case 0x11: case 0x12: case 0x13: return "ADC";
        case 0x18: case 0x19: case 0x1a: case 0x1b: return "SBB";
        case 0x20: case 0x21: case 0x22: case 0x23: return "AND";
        case 0x28: case 0x29: case 0x2a: case 0x2b: return "SUB";
        case 0x30: case 0x31: case 0x32: case 0x33: return "XOR";
        case 0x38: case 0x39: case 0x3a: case 0x3b: return "CMP";
        case 0x40 ... 0x47: return "INC";
        case 0x48 ... 0x4f: return "DEC";
        case 0x50 ... 0x57: return "PUSH";
        case 0x58 ... 0x5f: return "POP";
        case 0x60: return "PUSHA";
        case 0x61: return "POPA";
        case 0x68: case 0x6a: return "PUSH";
        case 0x70: return "JO";   case 0x71: return "JNO";
        case 0x72: return "JB";   case 0x73: return "JAE";
        case 0x74: return "JE";   case 0x75: return "JNE";
        case 0x76: return "JBE";  case 0x77: return "JA";
        case 0x78: return "JS";   case 0x79: return "JNS";
        case 0x7a: return "JP";   case 0x7b: return "JNP";
        case 0x7c: return "JL";   case 0x7d: return "JGE";
        case 0x7e: return "JLE";  case 0x7f: return "JG";
        case 0x80 ... 0x83: return "GRP1";
        case 0x84: case 0x85: return "TEST";
        case 0x86: case 0x87: return "XCHG";
        case 0x88 ... 0x8b: return "MOV";
        case 0x8c: case 0x8e: return "MOV";
        case 0x8d: return "LEA";
        case 0x90: return "NOP";
        case 0x98: return "CBW";
        case 0x99: return "CWD";
        case 0x9c: return "PUSHF";
        case 0x9d: return "POPF";
        case 0xa0 ... 0xa3: return "MOV";
        case 0xa4: return "MOVSB";
        case 0xa5: return "MOVSW";
        case 0xaa: return "STOSB";
        case 0xab: return "STOSW";
        case 0xb0 ... 0xbf: return "MOV";
        case 0xc0: case 0xc1: return "SHIFTS";
        case 0xc2: case 0xc3: return "RET";
        case 0xc6: case 0xc7: return "MOV";
        case 0xc9: return "LEAVE";
        case 0xcc: return "INT3";
        case 0xcd: return "INT";
        case 0xcf: return "IRET";
        case 0xd0 ... 0xd3: return "SHIFTS";
        case 0xe0: return "LOOPNE";
        case 0xe1: return "LOOPE";
        case 0xe2: return "LOOP";
        case 0xe3: return "JCXZ";
        case 0xe8: return "CALL";
        case 0xe9: case 0xeb: return "JMP";
        case 0xf6: case 0xf7: return "GRP3";
        case 0xfe: case 0xff: return "GRP4/5";
        case 0xf8: return "CLC"; case 0xf9: return "STC";
        case 0xfa: return "CLI"; case 0xfb: return "STI";
        case 0xfc: return "CLD"; case 0xfd: return "STD";
        default: {
            static char buf[8];
            snprintf(buf, sizeof(buf), "0x%02X", op);
            return buf;
        }
    }
}

/* ------------------------------------------------------------------ */
/* PieChart: pie with leader-line labels instead of a separate legend. */
/* ------------------------------------------------------------------ */

PieChart::PieChart(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(260, 120);
}

void
PieChart::setSlices(const QVector<QPair<QString, QPair<int, QColor>>> &slices)
{
    this->slices = slices;
    update();
}

void
PieChart::paintEvent(QPaintEvent *event)
{
    (void) event;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    /* Label columns on both sides, pie in the middle. */
    const qreal  labelW  = 96.0;
    const QRectF pieRect(labelW + 6, 6, width() - 2.0 * (labelW + 6), height() - 12.0);
    const qreal  cx = pieRect.center().x();
    const qreal  cy = pieRect.center().y();
    const qreal  r  = std::min(pieRect.width(), pieRect.height()) / 2.0;

    int total = 0;
    for (const auto &s : slices)
        total += s.second.first;
    if (total <= 0) {
        p.setPen(Qt::NoPen);
        p.setBrush(palette().color(QPalette::Window).darker(112));
        p.drawEllipse(pieRect);
        return;
    }

    /* Draw the slices and collect the ones that get a leader-line label. */
    struct Item {
        QString name;
        int     value;
        QColor  c;
        qreal   mid;  /* Qt angle convention, degrees */
        qreal   y;    /* label row */
        int     side; /* -1 left, +1 right */
    };
    QVector<Item> items;

    double start = 90.0; /* Qt drawPie: 12 o'clock, clockwise via negative span */
    p.setPen(Qt::NoPen);
    for (const auto &s : slices) {
        const double span = (double) s.second.first / (double) total * 360.0;
        if (s.second.first) {
            p.setBrush(s.second.second);
            p.drawPie(pieRect, (int) (start * 16), (int) (-span * 16));

            if (s.second.first * 100 >= total * 2) { /* label slices >= 2% */
                Item it;
                it.name  = s.first;
                it.value = s.second.first;
                it.c     = s.second.second;
                it.mid   = start - span / 2.0;
                it.side  = (it.mid > -90.0 && it.mid < 90.0) ? 1 : -1;
                it.y     = cy - r * std::sin(it.mid * M_PI / 180.0);
                items.append(it);
            }
        }
        start -= span;
    }

    /* Sort by row and enforce a minimum gap so labels do not overlap. */
    std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.y < b.y; });
    const qreal minGap = 15.0;
    for (int i = 1; i < items.size(); i++)
        if (items[i].y < items[i - 1].y + minGap)
            items[i].y = items[i - 1].y + minGap;
    for (auto &it : items)
        it.y = std::max(8.0, std::min((qreal) height() - 8.0, it.y));

    /* Leader lines (in slice colour) from the slice edge to each label. */
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() - 1.0);
    p.setFont(f);
    for (const auto &it : items) {
        const qreal  rad   = it.mid * M_PI / 180.0;
        const qreal  px    = cx + r * std::cos(rad);
        const qreal  py    = cy - r * std::sin(rad);
        const qreal  ax    = cx + it.side * (r + 8.0);
        const QString label = QStringLiteral("%1 %2%")
                                  .arg(it.name)
                                  .arg((double) it.value / (double) total * 100.0, 0, 'f', 1);

        p.setPen(palette().color(QPalette::WindowText));
        if (it.side < 0) {
            const QRectF trect(2.0, it.y - 8.0, ax - 4.0, 16.0);
            p.drawText(trect, Qt::AlignRight | Qt::AlignVCenter, label);
        } else {
            const QRectF trect(ax + 4.0, it.y - 8.0, width() - ax - 6.0, 16.0);
            p.drawText(trect, Qt::AlignLeft | Qt::AlignVCenter, label);
        }

        p.setPen(QPen(it.c, 1.2));
        p.drawLine(QPointF(px, py), QPointF(ax, it.y));
    }
}

/* ------------------------------------------------------------------ */
/* PerformancePipeline dialog.                                         */
/* ------------------------------------------------------------------ */

PerformancePipeline::PerformancePipeline(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::PerformancePipeline)
{
    ui->setupUi(this);

    /* Enable the gated monitors while this window is open. */
    branch_sim_set_enabled(1);
    pp_set_enabled(1);

    pp_elapsed.start();

    connect(ui->ppWinCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int idx) {
                static const int w[] = { 1000, 2000, 5000, 15000, 0 }; /* 0 = all */
                pp_top_window_ms = w[idx];
                updateValues();
            });

    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &PerformancePipeline::updateValues);
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

PerformancePipeline::~PerformancePipeline()
{
    pp_set_enabled(0);
    branch_sim_set_enabled(0);
    delete ui;
}

void
PerformancePipeline::renderBhtMap()
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
    if ((bhtMapImage.width() != cols) || (bhtMapImage.height() != rows))
        bhtMapImage = QImage(cols, rows, QImage::Format_RGB32);

    /* Light enough to stay readable on the dark qdarkstyle theme. */
    static const QRgb state_colors[4] = {
        qRgb(0x79, 0x86, 0xcb), /* 00 strong not-taken */
        qRgb(0x42, 0xa5, 0xf5), /* 01 weakly not-taken */
        qRgb(0xff, 0xa7, 0x26), /* 10 weakly taken */
        qRgb(0xe5, 0x39, 0x35), /* 11 strong taken */
    };

    for (int y = 0; y < rows; y++) {
        QRgb *line = (QRgb *) bhtMapImage.scanLine(y);
        for (int x = 0; x < cols; x++) {
            const int i = y * cols + x;
            line[x] = (i < entries) ? state_colors[map[i] & 3] : qRgb(0, 0, 0);
        }
    }

    delete[] map;

    ui->bhtMap->setPixmap(QPixmap::fromImage(bhtMapImage.scaled(ui->bhtMap->size(),
                                                                Qt::KeepAspectRatio,
                                                                Qt::FastTransformation)));
}

void
PerformancePipeline::updateValues()
{
    char tmp[128];

    /* --- Branch prediction --- */
    const uint64_t total     = branch_sim_total_get();
    const uint64_t correct   = branch_sim_correct_get();
    const uint64_t incorrect = branch_sim_incorrect_get();
    const uint64_t taken     = branch_sim_taken_get();

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

    snprintf(tmp, sizeof(tmp), "%llu / %llu", (unsigned long long) wc, (unsigned long long) wi);
    ui->wbValue->setText(QString::fromLatin1(tmp));
    snprintf(tmp, sizeof(tmp), "%llu / %llu",
             (unsigned long long) correct, (unsigned long long) incorrect);
    ui->tbValue->setText(QString::fromLatin1(tmp));

    if (total) {
        const double tr = (double) taken / (double) total * 100.0;
        snprintf(tmp, sizeof(tmp), "%.1f%% (%llu taken / %llu total)",
                 tr, (unsigned long long) taken, (unsigned long long) total);
        ui->takenValue->setText(QString::fromLatin1(tmp));
    } else
        ui->takenValue->setText(QStringLiteral("—"));

    renderBhtMap();

    /* --- Pipeline (approximate) --- */
    const uint64_t tsc_now = cpu_tsc_get();
    const uint64_t wcyc    = tsc_now - last_tsc;
    last_tsc               = tsc_now;

    const uint64_t wu     = pp_uops_get()   - last_pp_uops;
    const uint64_t wins   = pp_ins_get()    - last_pp_ins;
    const uint64_t walu   = pp_alu_get()    - last_pp_alu;
    const uint64_t wload  = pp_load_get()   - last_pp_load;
    const uint64_t wstore = pp_store_get()  - last_pp_store;
    const uint64_t wbr    = pp_branch_get() - last_pp_branch;
    const uint64_t wfpu   = pp_fpu_get()    - last_pp_fpu;
    const uint64_t wshift = pp_shift_get()  - last_pp_shift;
    const uint64_t wmmx   = pp_mmx_get()    - last_pp_mmx;
    const uint64_t wmisc  = pp_misc_get()   - last_pp_misc;

    last_pp_uops   = pp_uops_get();
    last_pp_ins    = pp_ins_get();
    last_pp_alu    = pp_alu_get();
    last_pp_load   = pp_load_get();
    last_pp_store  = pp_store_get();
    last_pp_branch = pp_branch_get();
    last_pp_fpu    = pp_fpu_get();
    last_pp_shift  = pp_shift_get();
    last_pp_mmx    = pp_mmx_get();
    last_pp_misc   = pp_misc_get();

    if (wcyc && wu) {
        const int    iw      = pp_issue_width_get();
        const double rupc    = (double) wu / (double) wcyc;
        const uint64_t ideal = wu / (uint64_t) iw;
        const int64_t stall  = (int64_t) wcyc - (int64_t) ideal;
        const double util    = std::min(100.0, rupc / (double) iw * 100.0);

        snprintf(tmp, sizeof(tmp), "%.2f  (%d-wide)", rupc, iw);
        ui->ppIPCValue->setText(QString::fromLatin1(tmp));
        ui->ppUtilBar->setValue((int) util);
        snprintf(tmp, sizeof(tmp), "%llu / %llu / %lld",
                 (unsigned long long) ideal, (unsigned long long) wcyc, (long long) stall);
        ui->ppStallValue->setText(QString::fromLatin1(tmp));

        /* uOp mix as a compact pie chart + percentage legend. */
        struct MixEntry {
            const char *name;
            uint64_t    n;
            QColor      c;
        };
        const MixEntry mix[8] = {
            { "ALU",    walu,   QColor(0x4f, 0xc3, 0xf7) }, /* ALU    */
            { "Load",   wload,  QColor(0x26, 0xa6, 0x9a) }, /* Load   */
            { "Store",  wstore, QColor(0xff, 0xa7, 0x26) }, /* Store  */
            { "Branch", wbr,    QColor(0xef, 0x53, 0x50) }, /* Branch */
            { "FPU",    wfpu,   QColor(0xab, 0x47, 0xbc) }, /* FPU    */
            { "Shift",  wshift, QColor(0x66, 0xbb, 0x6a) }, /* Shift  */
            { "MMX",    wmmx,   QColor(0xff, 0xee, 0x58) }, /* MMX    */
            { "Misc",   wmisc,  QColor(0x9e, 0x9e, 0x9e) }, /* Misc   */
        };

        QVector<QPair<QString, QPair<int, QColor>>> slices;
        for (const auto &m : mix)
            if (m.n)
                slices.append({ QString::fromLatin1(m.name), { (int) m.n, m.c } });
        ui->ppMix->setSlices(slices);

        /* Stall-attribution heuristics (honest: probabilities, not counts). */
        const double totalu = (double) wu;
        auto level = [](int pct) { return (pct >= 66) ? "High" : (pct >= 33) ? "Medium" : "Low"; };

        /* Memory proxy: high load/store ratio AND abnormally high CPI. */
        const double ls_ratio  = (double) (wload + wstore) / totalu;
        const double cpi       = wins ? (double) wcyc / (double) wins : 0.0;
        const double ideal_cpi = 1.0 / (double) iw;
        const double cpi_boost = std::min(2.0, std::max(0.0, (cpi - ideal_cpi) / ideal_cpi));
        const int mem_pct = (int) std::min(100.0, std::min(1.0, ls_ratio / 0.5) * 100.0 * (1.0 + 0.5 * cpi_boost));
        snprintf(tmp, sizeof(tmp), "%d%% (%s)", mem_pct, level(mem_pct));
        ui->ppMemValue->setText(QString::fromLatin1(tmp));
        ui->ppMemBar->setValue(mem_pct);

        /* Branch proxy: branch uOp density. */
        const int br_pct = (int) std::min(100.0, std::min(1.0, (double) wbr / totalu / 0.25) * 100.0);
        snprintf(tmp, sizeof(tmp), "%d%% (%s)", br_pct, level(br_pct));
        ui->ppBrValue->setText(QString::fromLatin1(tmp));
        ui->ppBrBar->setValue(br_pct);

        /* FPU proxy: FPU uOp density. */
        const int fpu_pct = (int) std::min(100.0, std::min(1.0, (double) wfpu / totalu / 0.4) * 100.0);
        snprintf(tmp, sizeof(tmp), "%d%% (%s)", fpu_pct, level(fpu_pct));
        ui->ppFpuValue->setText(QString::fromLatin1(tmp));
        ui->ppFpuBar->setValue(fpu_pct);
    } else {
        ui->ppIPCValue->setText(tr("idle"));
        ui->ppUtilBar->setValue(0);
        ui->ppStallValue->setText(QStringLiteral("—"));
        ui->ppMix->setSlices({});
        ui->ppMemValue->setText(QStringLiteral("—"));
        ui->ppMemBar->setValue(0);
        ui->ppBrValue->setText(QStringLiteral("—"));
        ui->ppBrBar->setValue(0);
        ui->ppFpuValue->setText(QStringLiteral("—"));
        ui->ppFpuBar->setValue(0);
    }

    /* Top instructions: rolling window of opcode deltas -> labelled pie. */
    uint64_t hist[256];
    pp_hist_get(hist);
    uint64_t delta[256];
    for (int i = 0; i < 256; i++) {
        delta[i] = hist[i] - last_pp_hist[i];
        last_pp_hist[i] = hist[i];
    }
    {
        const uint64_t now = (uint64_t) pp_elapsed.elapsed();
        pp_ring.append(QVector<uint64_t>(delta, delta + 256));
        pp_ring_ts.append(now);
        while ((pp_ring.size() > 1) && pp_top_window_ms &&
               ((now - pp_ring_ts.first()) > (uint64_t) pp_top_window_ms)) {
            pp_ring.removeFirst();
            pp_ring_ts.removeFirst();
        }
    }

    uint64_t sum[256] = { 0 };
    for (const auto &s : pp_ring)
        for (int i = 0; i < 256; i++)
            sum[i] += s[i];

    struct OpCount {
        uint8_t  op;
        uint64_t n;
    };
    OpCount counts[256];
    int      ncnt = 0;
    for (int i = 0; i < 256; i++)
        if (sum[i]) {
            counts[ncnt].op = (uint8_t) i;
            counts[ncnt].n  = sum[i];
            ncnt++;
        }
    std::sort(counts, counts + ncnt, [](const OpCount &a, const OpCount &b) { return a.n > b.n; });

    static const QRgb top_palette[10] = {
        qRgb(0xe5, 0x73, 0x73), qRgb(0x64, 0xb5, 0xf6), qRgb(0x81, 0xc7, 0x84), qRgb(0xff, 0xb7, 0x4d),
        qRgb(0xba, 0x68, 0xc8), qRgb(0x4d, 0xb6, 0xac), qRgb(0xf0, 0x62, 0x92), qRgb(0xff, 0xd5, 0x4f),
        qRgb(0x90, 0xa4, 0xae), qRgb(0xa1, 0x88, 0x7f),
    };

    QVector<QPair<QString, QPair<int, QColor>>> psegs;
    const int shown = std::min(ncnt, 10);
    for (int i = 0; i < shown; i++)
        psegs.append({ QString::fromLatin1(pp_opcode_name(counts[i].op)),
                       { (int) counts[i].n, QColor(top_palette[i]) } });
    ui->ppTopPie->setSlices(psegs);
}
