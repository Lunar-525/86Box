#ifndef QT_CACHE_HPP
#define QT_CACHE_HPP

#include <QDialog>
#include <QImage>

class QTimer;

namespace Ui {
class Cache;
}

/* CPU L1 cache viewer (approximate): hit rate, sets x ways cache map and
   working-set vs L1-size comparison, driven by the gated cache simulator in
   mem.c. Gated on this window being open. */
class Cache : public QDialog {
    Q_OBJECT

public:
    explicit Cache(QWidget *parent = nullptr);
    ~Cache();

private slots:
    void refreshView();

private:
    void renderMap();

    Ui::Cache *ui;
    QTimer    *timer;
    QImage     mapImage;

    uint64_t last_hits   = 0;
    uint64_t last_misses = 0;
};

#endif // QT_CACHE_HPP
