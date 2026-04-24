/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ove/types.h"
#include "ove/audio.h"
#ifdef CONFIG_OVE_TIME
#include "ove/time.h"
#endif
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* ── Helpers ────────────────────────────────────────────────────── */

static unsigned int buf_byte_size(const struct ove_audio_fmt *fmt,
                                  unsigned int frames)
{
    return frames * fmt->channels * ove_audio_sample_size(fmt->sample_fmt);
}

static int find_upstream(const struct ove_audio_graph *g, unsigned int node)
{
    for (unsigned int i = 0; i < g->edge_count; i++) {
        if (g->edges[i].to == node)
            return (int)g->edges[i].from;
    }
    return -1;
}

/* ── Init / Deinit ──────────────────────────────────────────────── */

int ove_audio_graph_init(struct ove_audio_graph *g, unsigned int frames_per_period)
{
    if (!g || frames_per_period == 0)
        return OVE_ERR_INVALID_PARAM;

    memset(g, 0, sizeof(*g));
    g->frames_per_period = frames_per_period;
    g->state = OVE_AUDIO_GRAPH_IDLE;
    return OVE_OK;
}

void ove_audio_graph_deinit(struct ove_audio_graph *g)
{
    if (!g)
        return;

    if (g->state == OVE_AUDIO_GRAPH_RUNNING)
        ove_audio_graph_stop(g);

    for (unsigned int i = 0; i < g->node_count; i++) {
        if (g->nodes[i].ops && g->nodes[i].ops->destroy)
            g->nodes[i].ops->destroy(g->nodes[i].ctx);
    }

    /* Only free when build() allocated the buffer itself
     * (caller-provided storage is signalled by a non-zero buf_storage_size). */
    if (g->buf_storage_size == 0) {
        free(g->buf_storage);
    }
    memset(g, 0, sizeof(*g));
}

int ove_audio_graph_set_buf_storage(struct ove_audio_graph *g,
                                    void *storage, size_t size)
{
    if (!g || !storage || size == 0)
        return OVE_ERR_INVALID_PARAM;
    if (g->state != OVE_AUDIO_GRAPH_IDLE)
        return OVE_ERR_NOT_SUPPORTED;

    g->buf_storage      = storage;
    g->buf_storage_size = size;
    return OVE_OK;
}

#ifdef OVE_HEAP_AUDIO
int ove_audio_graph_create_(struct ove_audio_graph *g, unsigned int frames)
{
    return ove_audio_graph_init(g, frames);
}

int ove_audio_graph_destroy(struct ove_audio_graph *g)
{
    if (!g)
        return OVE_ERR_INVALID_PARAM;
    ove_audio_graph_deinit(g);
    return OVE_OK;
}
#endif /* OVE_HEAP_AUDIO */

/* ── Add / Connect ──────────────────────────────────────────────── */

int ove_audio_graph_add_node(struct ove_audio_graph *g,
                             const struct ove_audio_node_ops *ops,
                             void *ctx, const char *name,
                             enum ove_audio_node_type type)
{
    if (!g || !ops || !name)
        return OVE_ERR_INVALID_PARAM;

    if (g->state != OVE_AUDIO_GRAPH_IDLE)
        return OVE_ERR_NOT_SUPPORTED;

    if (g->node_count >= OVE_AUDIO_GRAPH_MAX_NODES)
        return OVE_ERR_QUEUE_FULL;

    unsigned int idx = g->node_count++;
    struct ove_audio_node *n = &g->nodes[idx];
    n->name = name;
    n->type = type;
    n->ops  = ops;
    n->ctx  = ctx;
    memset(&n->out_fmt, 0, sizeof(n->out_fmt));

    return (int)idx;
}

int ove_audio_graph_connect(struct ove_audio_graph *g,
                            unsigned int from, unsigned int to)
{
    if (!g)
        return OVE_ERR_INVALID_PARAM;

    if (g->state != OVE_AUDIO_GRAPH_IDLE)
        return OVE_ERR_NOT_SUPPORTED;

    if (from >= g->node_count || to >= g->node_count || from == to)
        return OVE_ERR_INVALID_PARAM;

    if (g->edge_count >= OVE_AUDIO_GRAPH_MAX_EDGES)
        return OVE_ERR_QUEUE_FULL;

    if (g->nodes[from].type == OVE_AUDIO_NODE_SINK)
        return OVE_ERR_INVALID_PARAM;

    if (g->nodes[to].type == OVE_AUDIO_NODE_SOURCE)
        return OVE_ERR_INVALID_PARAM;

    /* Reject fan-in: each node may have at most one incoming edge.
       Mixing (multiple inputs) is not supported in this version. */
    for (unsigned int i = 0; i < g->edge_count; i++) {
        if (g->edges[i].to == to)
            return OVE_ERR_INVALID_PARAM;
    }

    g->edges[g->edge_count].from = from;
    g->edges[g->edge_count].to   = to;
    g->edge_count++;

    return OVE_OK;
}

/* ── Topological sort (Kahn's algorithm) ────────────────────────── */

static int topo_sort(struct ove_audio_graph *g)
{
    unsigned int in_degree[OVE_AUDIO_GRAPH_MAX_NODES] = {0};
    unsigned int queue[OVE_AUDIO_GRAPH_MAX_NODES];
    unsigned int head = 0, tail = 0;

    for (unsigned int i = 0; i < g->edge_count; i++)
        in_degree[g->edges[i].to]++;

    for (unsigned int i = 0; i < g->node_count; i++) {
        if (in_degree[i] == 0)
            queue[tail++] = i;
    }

    g->exec_count = 0;
    while (head < tail) {
        unsigned int node = queue[head++];
        g->exec_order[g->exec_count++] = node;

        for (unsigned int i = 0; i < g->edge_count; i++) {
            if (g->edges[i].from == node) {
                unsigned int dest = g->edges[i].to;
                in_degree[dest]--;
                if (in_degree[dest] == 0)
                    queue[tail++] = dest;
            }
        }
    }

    return (g->exec_count == g->node_count) ? OVE_OK : OVE_ERR_INVALID_PARAM;
}

/* ── Build ──────────────────────────────────────────────────────── */

int ove_audio_graph_build(struct ove_audio_graph *g)
{
    if (!g)
        return OVE_ERR_INVALID_PARAM;

    if (g->state != OVE_AUDIO_GRAPH_IDLE)
        return OVE_ERR_NOT_SUPPORTED;

    int ret = topo_sort(g);
    if (ret != OVE_OK)
        return ret;

    for (unsigned int i = 0; i < g->exec_count; i++) {
        unsigned int idx = g->exec_order[i];
        struct ove_audio_node *node = &g->nodes[idx];

        const struct ove_audio_fmt *in_fmt = NULL;

        if (node->type != OVE_AUDIO_NODE_SOURCE) {
            int upstream = find_upstream(g, idx);
            if (upstream >= 0)
                in_fmt = &g->nodes[upstream].out_fmt;
        }

        struct ove_audio_fmt *out_ptr = NULL;
        if (node->type != OVE_AUDIO_NODE_SINK)
            out_ptr = &node->out_fmt;

        ret = node->ops->configure(node->ctx, in_fmt, out_ptr);
        if (ret != OVE_OK)
            return ret;
    }

    /* Validate non-sink nodes produced valid output formats */
    for (unsigned int i = 0; i < g->node_count; i++) {
        if (g->nodes[i].type != OVE_AUDIO_NODE_SINK) {
            if (g->nodes[i].out_fmt.channels == 0 ||
                g->nodes[i].out_fmt.sample_rate == 0)
                return OVE_ERR_INVALID_PARAM;
        }
    }

    /* Validate sample rate agreement at each edge */
    for (unsigned int i = 0; i < g->edge_count; i++) {
        unsigned int from_rate = g->nodes[g->edges[i].from].out_fmt.sample_rate;
        unsigned int to_idx = g->edges[i].to;
        if (g->nodes[to_idx].type != OVE_AUDIO_NODE_SINK) {
            unsigned int to_rate = g->nodes[to_idx].out_fmt.sample_rate;
            if (from_rate != to_rate)
                return OVE_ERR_INVALID_PARAM;
        }
    }

    size_t total_size = 0;
    size_t offsets[OVE_AUDIO_GRAPH_MAX_NODES] = {0};

    for (unsigned int i = 0; i < g->node_count; i++) {
        offsets[i] = total_size;
        if (g->nodes[i].type != OVE_AUDIO_NODE_SINK) {
            size_t bs = buf_byte_size(&g->nodes[i].out_fmt,
                                       g->frames_per_period);
            /* Overflow check: reject pathological configs before calloc */
            if (bs > SIZE_MAX - total_size)
                return OVE_ERR_NO_MEMORY;
            total_size += bs;
        }
    }

    if (total_size > 0) {
        if (g->buf_storage_size > 0) {
            /* Caller-provided storage (required under CONFIG_OVE_ZERO_HEAP). */
            if (g->buf_storage_size < total_size)
                return OVE_ERR_NO_MEMORY;
            memset(g->buf_storage, 0, total_size);
        } else {
#ifdef CONFIG_OVE_ZERO_HEAP
            /* No heap available — caller must pre-provide storage via
             * ove_audio_graph_set_buf_storage(). */
            return OVE_ERR_NO_MEMORY;
#else
            g->buf_storage = calloc(1, total_size);
            if (!g->buf_storage)
                return OVE_ERR_NO_MEMORY;
#endif
        }
    }

    for (unsigned int i = 0; i < g->node_count; i++) {
        g->buffers[i].frames = g->frames_per_period;
        if (g->nodes[i].type != OVE_AUDIO_NODE_SINK) {
            g->buffers[i].data = (char *)g->buf_storage + offsets[i];
            g->buffers[i].fmt  = &g->nodes[i].out_fmt;
        } else {
            g->buffers[i].data = NULL;
            g->buffers[i].fmt  = NULL;
        }
    }

    g->state = OVE_AUDIO_GRAPH_READY;
    return OVE_OK;
}

/* ── Execution ──────────────────────────────────────────────────── */

int ove_audio_graph_process(struct ove_audio_graph *g)
{
    if (!g || g->state < OVE_AUDIO_GRAPH_READY)
        return OVE_ERR_NOT_SUPPORTED;

#ifdef CONFIG_OVE_TIME
    uint64_t t_start = 0;
    ove_time_get_us(&t_start);
#endif

    for (unsigned int i = 0; i < g->exec_count; i++) {
        unsigned int idx = g->exec_order[i];
        struct ove_audio_node *node = &g->nodes[idx];

        const struct ove_audio_buf *in_buf = NULL;
        struct ove_audio_buf *out_buf = NULL;

        if (node->type != OVE_AUDIO_NODE_SOURCE) {
            int upstream = find_upstream(g, idx);
            if (upstream >= 0)
                in_buf = &g->buffers[upstream];
        }

        if (node->type != OVE_AUDIO_NODE_SINK)
            out_buf = &g->buffers[idx];

        int ret = node->ops->process(node->ctx, in_buf, out_buf);
        if (ret != OVE_OK) {
            g->stats.node_errors++;
            if (out_buf && out_buf->data) {
                memset(out_buf->data,
                       0,
                       buf_byte_size(out_buf->fmt, out_buf->frames));
            }
        }
    }

    g->stats.cycles++;

#ifdef CONFIG_OVE_TIME
    uint64_t t_end = 0;
    ove_time_get_us(&t_end);
    uint32_t elapsed = (uint32_t)(t_end - t_start);
    if (elapsed > g->stats.max_process_us)
        g->stats.max_process_us = elapsed;
    /* Rolling average: avg = avg + (elapsed - avg) / cycles */
    if (g->stats.cycles == 1)
        g->stats.avg_process_us = elapsed;
    else
        g->stats.avg_process_us += (elapsed - g->stats.avg_process_us) /
                                   g->stats.cycles;
#endif

    return OVE_OK;
}

/* ── Start / Stop ───────────────────────────────────────────────── */

int ove_audio_graph_start(struct ove_audio_graph *g)
{
    if (!g || g->state != OVE_AUDIO_GRAPH_READY)
        return OVE_ERR_NOT_SUPPORTED;

    for (unsigned int i = 0; i < g->exec_count; i++) {
        struct ove_audio_node *node = &g->nodes[g->exec_order[i]];
        if (node->ops->start) {
            int ret = node->ops->start(node->ctx);
            if (ret != OVE_OK) {
                /* Roll back: stop already-started nodes in reverse */
                for (int j = (int)i - 1; j >= 0; j--) {
                    struct ove_audio_node *prev = &g->nodes[g->exec_order[j]];
                    if (prev->ops->stop)
                        prev->ops->stop(prev->ctx);
                }
                return ret;
            }
        }
    }

    g->state = OVE_AUDIO_GRAPH_RUNNING;
    return OVE_OK;
}

int ove_audio_graph_stop(struct ove_audio_graph *g)
{
    if (!g || g->state != OVE_AUDIO_GRAPH_RUNNING)
        return OVE_ERR_NOT_SUPPORTED;

    for (int i = (int)g->exec_count - 1; i >= 0; i--) {
        struct ove_audio_node *node = &g->nodes[g->exec_order[i]];
        if (node->ops->stop)
            node->ops->stop(node->ctx);
    }

    g->state = OVE_AUDIO_GRAPH_READY;
    return OVE_OK;
}

/* ── Diagnostics ────────────────────────────────────────────────── */

int ove_audio_graph_get_stats(const struct ove_audio_graph *g,
                              struct ove_audio_graph_stats *stats)
{
    if (!g || !stats)
        return OVE_ERR_INVALID_PARAM;

    *stats = g->stats;
    return OVE_OK;
}
