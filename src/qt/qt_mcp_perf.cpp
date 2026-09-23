/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Performance sampling for the MCP server.
 *
 * Authors: 86Box contributors
 *
 *          Copyright 2025 86Box contributors
 */
#include "qt_mcp_perf.hpp"

#include <QTimer>

#include <algorithm>
#include <memory>

extern "C" {
#include <86box/86box.h>
#include <86box/mem.h>
#include <86box/plat.h>
}

namespace McpPerf {
static bool viewer_open = false;
static bool in_flight   = false;
/* Set while this module has switched the samplers on itself, so that it can
   switch them off again when it is done and no viewer needs them. */
static bool own_sampling = false;

void
set_viewer_open(bool open)
{
    viewer_open = open;
}

static void
set_sampling_enabled(bool enabled)
{
    mem_access_set_enabled(enabled ? 1 : 0);
    cache_sim_set_enabled(enabled ? 1 : 0);
}

/* Everything the Performance window reads out of one instant. */
struct Snapshot {
    uint64_t guest_ns;
    uint64_t real_ns;
    uint64_t l1_hits;
    uint64_t l1_misses;
    uint64_t l2_hits;
    uint64_t l2_misses;
};

static Snapshot
take_snapshot(void)
{
    Snapshot snapshot;

    snapshot.guest_ns  = cpu_time_guest_ns_get();
    snapshot.real_ns   = cpu_time_real_ns_get();
    snapshot.l1_hits   = cache_sim_hits_get();
    snapshot.l1_misses = cache_sim_misses_get();
    snapshot.l2_hits   = cache_sim_l2_hits_get();
    snapshot.l2_misses = cache_sim_l2_misses_get();

    return snapshot;
}

void
sample(QObject *context, int window_ms, std::function<void(const QJsonObject &)> done)
{
    if (in_flight) {
        QJsonObject busy;
        busy["error"] = QStringLiteral("A performance sample is already running.");
        done(busy);
        return;
    }

    in_flight = true;

    /* The samplers are gated; switch them on for the sampling window unless
       somebody (the Performance window) already has them on. */
    if (!viewer_open) {
        set_sampling_enabled(true);
        own_sampling = true;
    }

    const auto first = std::make_shared<Snapshot>(take_snapshot());

    QTimer::singleShot(window_ms, context, [context, first, window_ms, done]() {
        const Snapshot second = take_snapshot();

        /* Working set: which physical pages were touched during the window.
           Mirrors what the Performance window does on every refresh. */
        mem_access_ensure_sized();
        mem_access_scan_ptes();

        uint32_t used_pages = 0;
        const uint8_t *heat = mem_access_heat_get();
        const uint32_t pages = mem_access_pages_get();
        if (heat != nullptr) {
            for (uint32_t i = 0; i < pages; i++) {
                if (heat[i] != 0)
                    used_pages++;
            }
        }

        const bool was_own_sampling = own_sampling;
        own_sampling = false;
        in_flight    = false;
        if (was_own_sampling && !viewer_open)
            set_sampling_enabled(false);

        const bool     paused     = (dopause != 0);
        const uint64_t guest_diff = (second.guest_ns >= first->guest_ns) ? (second.guest_ns - first->guest_ns) : 0;
        const uint64_t real_diff  = (second.real_ns >= first->real_ns) ? (second.real_ns - first->real_ns) : 0;

        QJsonObject cpu;
        if ((guest_diff != 0) && (real_diff != 0)) {
            cpu["speed_percent"]             = static_cast<double>(guest_diff) / static_cast<double>(real_diff) * 100.0;
            cpu["guest_ms"]                  = static_cast<double>(guest_diff) / 1000000.0;
            cpu["real_ms"]                   = static_cast<double>(real_diff) / 1000000.0;
            cpu["real_percent_of_simulated"] = static_cast<double>(real_diff) / static_cast<double>(guest_diff) * 100.0;
        } else {
            cpu["speed_percent"]             = QJsonValue::Null;
            cpu["guest_ms"]                  = static_cast<double>(guest_diff) / 1000000.0;
            cpu["real_ms"]                   = static_cast<double>(real_diff) / 1000000.0;
            cpu["real_percent_of_simulated"] = QJsonValue::Null;
        }
        cpu["guest_ns_total"] = static_cast<double>(second.guest_ns);
        cpu["real_ns_total"]  = static_cast<double>(second.real_ns);
        cpu["emulation_time_ratio_percent"] =
            (second.guest_ns != 0)
                ? static_cast<double>(second.real_ns) / static_cast<double>(second.guest_ns) * 100.0
                : 0.0;

        const int l1_size  = cache_sim_size_get();
        const int l1_assoc = cache_sim_assoc_get();

        QJsonObject l1;
        l1["available"] = (l1_size != 0);
        if (l1_size != 0) {
            const uint64_t hits   = second.l1_hits - first->l1_hits;
            const uint64_t misses = second.l1_misses - first->l1_misses;

            l1["size_kb"]     = l1_size / 1024;
            l1["ways"]        = l1_assoc;
            l1["line_bytes"]  = cache_sim_line_get();
            l1["hits_total"]  = static_cast<double>(second.l1_hits);
            l1["misses_total"] = static_cast<double>(second.l1_misses);
            /* Deltas are unsigned; a counter reset would wrap, so clamp. */
            l1["hits_window"]   = static_cast<double>((hits <= second.l1_hits) ? hits : 0);
            l1["misses_window"] = static_cast<double>((misses <= second.l1_misses) ? misses : 0);
            if ((hits + misses) != 0)
                l1["hit_rate_percent"] = static_cast<double>(hits) / static_cast<double>(hits + misses) * 100.0;
            else
                l1["hit_rate_percent"] = QJsonValue::Null;
        }

        QJsonObject l2;
        const bool l2_active = (cache_sim_l2_active() != 0);
        l2["available"] = l2_active;
        if (l2_active) {
            const uint64_t hits   = second.l2_hits - first->l2_hits;
            const uint64_t misses = second.l2_misses - first->l2_misses;

            l2["size_kb"]       = cache_sim_l2_size_get() / 1024;
            l2["ways"]          = cache_sim_l2_assoc_get();
            l2["line_bytes"]    = 32;
            l2["hits_total"]    = static_cast<double>(second.l2_hits);
            l2["misses_total"]  = static_cast<double>(second.l2_misses);
            l2["hits_window"]   = static_cast<double>((hits <= second.l2_hits) ? hits : 0);
            l2["misses_window"] = static_cast<double>((misses <= second.l2_misses) ? misses : 0);
            if ((hits + misses) != 0)
                l2["hit_rate_percent"] = static_cast<double>(hits) / static_cast<double>(hits + misses) * 100.0;
            else
                l2["hit_rate_percent"] = QJsonValue::Null;
        }

        const double working_set_kb = static_cast<double>(used_pages) * 4.0;

        QJsonObject working_set;
        working_set["pages"] = static_cast<double>(used_pages);
        working_set["kb"]    = working_set_kb;
        if (l1_size != 0) {
            const double l1_kb = std::max(1, l1_size / 1024);
            working_set["l1_size_kb"]             = l1_kb;
            working_set["percent_of_l1"]          = working_set_kb / l1_kb * 100.0;
            working_set["fits_into_l1"]           = (working_set_kb <= l1_kb);
        }
        working_set["total_pages"]    = static_cast<double>(pages);
        working_set["total_ram_kb"]   = static_cast<double>(mem_ram_size_get()) / 1024.0;

        QJsonObject result;
        result["paused"]           = paused;
        result["sample_window_ms"] = window_ms;
        result["cpu"]              = cpu;
        result["l1"]               = l1;
        result["l2"]               = l2;
        result["working_set"]      = working_set;

        Q_UNUSED(context);
        done(result);
    });
}
}
