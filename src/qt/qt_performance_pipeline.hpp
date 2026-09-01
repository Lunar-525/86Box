#ifndef QT_PERFORMANCE_PIPELINE_HPP
#define QT_PERFORMANCE_PIPELINE_HPP

#include <QColor>
#include <QDialog>
#include <QElapsedTimer>
#include <QImage>
#include <QPair>
#include <QString>
#include <QVector>
#include <QWidget>

#include <cstdint>

class QPaintEvent;
class QTimer;

/* Pie chart with leader-line labels: each slice is connected by a line to
   its "name pct%" label on the left or right of the pie, so the data stays
   visually tied to the chart instead of a separate legend. */
class PieChart : public QWidget {
public:
    explicit PieChart(QWidget *parent = nullptr);
    /* name, value, colour; zero values are skipped. */
    void setSlices(const QVector<QPair<QString, QPair<int, QColor>>> &slices);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QVector<QPair<QString, QPair<int, QColor>>> slices;
};

namespace Ui {
class PerformancePipeline;
}

/* Branch-predictor (BHT) and pipeline profiler view: BHT map + accuracy,
   retired-uOp mix, top instructions and stall-attribution heuristics. */
class PerformancePipeline : public QDialog {
    Q_OBJECT

public:
    explicit PerformancePipeline(QWidget *parent = nullptr);
    ~PerformancePipeline();

private slots:
    void updateValues();

private:
    void renderBhtMap();

    Ui::PerformancePipeline *ui;
    QTimer                  *timer;

    /* Branch prediction. */
    QImage   bhtMapImage;
    uint64_t last_total     = 0;
    uint64_t last_correct   = 0;
    uint64_t last_incorrect = 0;

    /* Pipeline profiler. */
    uint64_t last_tsc = 0;
    uint64_t last_pp_uops = 0, last_pp_ins = 0;
    uint64_t last_pp_alu = 0, last_pp_load = 0, last_pp_store = 0, last_pp_branch = 0;
    uint64_t last_pp_fpu = 0, last_pp_shift = 0, last_pp_mmx = 0, last_pp_misc = 0;
    uint64_t last_pp_hist[256] = { 0 };

    /* Top-instructions rolling window (per-refresh opcode deltas). */
    QVector<QVector<uint64_t>> pp_ring;
    QVector<uint64_t>          pp_ring_ts;
    int                        pp_top_window_ms = 5000;
    QElapsedTimer              pp_elapsed;
};

#endif // QT_PERFORMANCE_PIPELINE_HPP
