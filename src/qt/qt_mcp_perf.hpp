/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Performance sampling for the MCP server. Provides exactly the
 *          numbers shown by the Tools > Performance window to a process that
 *          has no such window (the VM manager asks a running VM over its IPC
 *          socket, and the emulator samples its own counters).
 *
 * Authors: 86Box contributors
 *
 *          Copyright 2025 86Box contributors
 */
#ifndef QT_MCP_PERF_HPP
#define QT_MCP_PERF_HPP

#include <QJsonObject>
#include <QObject>

#include <functional>

/* Wall clock window one sample spans. Long enough for the counters to move,
   short enough that a tool call still feels immediate. */
constexpr int MCP_PERF_SAMPLE_WINDOW_MS = 400;

namespace McpPerf {
/* The sampling counters are gated to avoid overhead while nobody looks at
   them. Tell this module when the Performance window is open so sampling
   never switches the samplers off underneath it. */
void set_viewer_open(bool open);

/* Samples the CPU speed and cache statistics over `window_ms` of wall clock
   time and reports them to `done`. Completion happens from the event loop, so
   `context` must outlive the window. A report of the form {"error": ...} means
   no sample could be taken. */
void sample(QObject *context, int window_ms, std::function<void(const QJsonObject &)> done);
}

#endif // QT_MCP_PERF_HPP
