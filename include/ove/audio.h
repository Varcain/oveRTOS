/* SPDX-License-Identifier: GPL-3.0-or-later */

/**
 * @defgroup ove_audio Audio graph engine
 * @brief Build and execute a DAG of audio processing nodes.
 *
 * The audio graph engine models audio processing as a directed acyclic
 * graph (DAG) of typed nodes connected by edges:
 *
 * - **Source nodes** produce audio (hardware input, file, generator).
 * - **Processor nodes** transform audio (DSP, format conversion, gain).
 * - **Sink nodes** consume audio (hardware output, file, observer).
 *
 * The graph is static: configured at init, validated at build time via
 * topological sort and format propagation, then started.  Changes require
 * stop, reconfigure, restart.
 *
 * **Execution modes:**
 * - Sink-driven: hardware DMA/callback triggers graph processing.
 * - App-driven: caller pumps one cycle via ove_audio_graph_process().
 *
 * Requires @c CONFIG_OVE_AUDIO.
 * @{
 */

#ifndef OVE_AUDIO_H
#define OVE_AUDIO_H

#include "ove/audio_node.h"

#ifdef CONFIG_OVE_AUDIO

/* ── Graph limits ───────────────────────────────────────────────── */

/** @brief Maximum number of nodes in a single audio graph. */
#define OVE_AUDIO_GRAPH_MAX_NODES   16
/** @brief Maximum number of edges in a single audio graph. */
#define OVE_AUDIO_GRAPH_MAX_EDGES   16

/* ── Edge ───────────────────────────────────────────────────────── */

/**
 * @brief Directed connection between two nodes in the audio graph.
 *
 * Both fields are zero-based indices into @c ove_audio_graph::nodes[].
 */
struct ove_audio_edge {
    unsigned int from; /**< @brief Index of the upstream (producer) node. */
    unsigned int to;   /**< @brief Index of the downstream (consumer) node. */
};

/* ── Diagnostics ────────────────────────────────────────────────── */

/**
 * @brief Runtime diagnostic counters for an audio graph.
 *
 * Retrieved with ove_audio_graph_get_stats().  All counters accumulate
 * from graph start and are reset on each ove_audio_graph_start() call.
 */
struct ove_audio_graph_stats {
    unsigned int cycles;         /**< @brief Number of completed processing cycles. */
    unsigned int underruns;      /**< @brief Sink starvation events (sink received no data). */
    unsigned int overruns;       /**< @brief Source overflow events (source dropped frames). */
    unsigned int node_errors;    /**< @brief Cumulative node process() failures. */
    unsigned int max_process_us; /**< @brief Worst-case cycle wall-clock time in microseconds. */
    unsigned int avg_process_us; /**< @brief Rolling average cycle wall-clock time in microseconds. */
};

/* ── Graph ──────────────────────────────────────────────────────── */

/**
 * @brief Lifecycle state of an audio graph.
 *
 * @see ove_audio_graph_build, ove_audio_graph_start, ove_audio_graph_stop
 */
enum ove_audio_graph_state {
    OVE_AUDIO_GRAPH_IDLE,    /**< @brief Initial state; nodes may be added and connected. */
    OVE_AUDIO_GRAPH_READY,   /**< @brief Build succeeded; graph may be started. */
    OVE_AUDIO_GRAPH_RUNNING, /**< @brief Graph is actively processing audio. */
};

/**
 * @brief Audio processing graph instance.
 *
 * Holds all nodes, edges, execution order, audio buffers, and runtime
 * statistics for one complete audio pipeline.  Must be initialised with
 * ove_audio_graph_init() before use.
 */
struct ove_audio_graph {
    struct ove_audio_node   nodes[OVE_AUDIO_GRAPH_MAX_NODES]; /**< @brief Registered node descriptors. */
    unsigned int            node_count;                        /**< @brief Number of valid entries in @c nodes[]. */

    struct ove_audio_edge   edges[OVE_AUDIO_GRAPH_MAX_EDGES]; /**< @brief Registered directed edges. */
    unsigned int            edge_count;                        /**< @brief Number of valid entries in @c edges[]. */

    unsigned int            exec_order[OVE_AUDIO_GRAPH_MAX_NODES]; /**< @brief Node indices in topological execution order. */
    unsigned int            exec_count;                             /**< @brief Number of valid entries in @c exec_order[]. */

    struct ove_audio_buf    buffers[OVE_AUDIO_GRAPH_MAX_NODES]; /**< @brief Per-node intermediate audio buffers. */
    void                   *buf_storage;                         /**< @brief Heap block backing all buffer data arrays. */

    unsigned int            frames_per_period;       /**< @brief Frame count processed per graph cycle. */
    enum ove_audio_graph_state state;                /**< @brief Current lifecycle state. */

    struct ove_audio_graph_stats stats;              /**< @brief Accumulated runtime diagnostics. */
};

/* ── Graph API ──────────────────────────────────────────────────── */

/**
 * @brief Initialise an audio graph.
 *
 * Sets up internal state and records the processing period size.  Must
 * be called before any other graph function.
 *
 * @param[in] g                  Graph instance to initialise.
 * @param[in] frames_per_period  Number of audio frames processed per cycle.
 * @return 0 on success, negative error code on failure.
 *
 * @note Requires @c CONFIG_OVE_AUDIO.
 * @see ove_audio_graph_deinit
 */
int  ove_audio_graph_init(struct ove_audio_graph *g,
                          unsigned int frames_per_period);

/**
 * @brief Release all resources held by an audio graph.
 *
 * Frees the heap buffer storage and resets internal state.  The graph
 * must be stopped before calling this function.
 *
 * @param[in] g  Initialised graph instance.
 *
 * @note Requires @c CONFIG_OVE_AUDIO.
 * @see ove_audio_graph_init
 */
void ove_audio_graph_deinit(struct ove_audio_graph *g);

/**
 * @brief Register a new node in the graph.
 *
 * Appends a node entry to the graph's node table.  Nodes may only be
 * added while the graph is in the @c OVE_AUDIO_GRAPH_IDLE state.
 *
 * @param[in] g     Graph instance.
 * @param[in] ops   Vtable providing the node implementation.
 * @param[in] ctx   Opaque context pointer forwarded to every vtable call.
 * @param[in] name  Human-readable node name for diagnostics.
 * @param[in] type  Role of the node: source, processor, or sink.
 * @return Non-negative node index on success, negative error code on failure.
 *
 * @note Requires @c CONFIG_OVE_AUDIO.
 * @see ove_audio_graph_connect, ove_audio_graph_build
 */
int  ove_audio_graph_add_node(struct ove_audio_graph *g,
                              const struct ove_audio_node_ops *ops,
                              void *ctx, const char *name,
                              enum ove_audio_node_type type);

/**
 * @brief Connect two nodes with a directed edge.
 *
 * Adds an edge from the node at index @p from to the node at index
 * @p to.  Both nodes must already be registered.  Edges may only be
 * added while the graph is in the @c OVE_AUDIO_GRAPH_IDLE state.
 *
 * @param[in] g     Graph instance.
 * @param[in] from  Index of the upstream (producer) node.
 * @param[in] to    Index of the downstream (consumer) node.
 * @return 0 on success, negative error code on failure.
 *
 * @note Requires @c CONFIG_OVE_AUDIO.
 * @see ove_audio_graph_add_node, ove_audio_graph_build
 */
int  ove_audio_graph_connect(struct ove_audio_graph *g,
                             unsigned int from, unsigned int to);

/**
 * @brief Validate and compile the graph.
 *
 * Performs a topological sort, propagates audio formats from sources
 * to sinks by calling each node's @c configure callback, allocates the
 * inter-node audio buffers, and transitions the graph to
 * @c OVE_AUDIO_GRAPH_READY.
 *
 * @param[in] g  Graph instance in the @c OVE_AUDIO_GRAPH_IDLE state.
 * @return 0 on success, negative error code on failure (e.g. cycle
 *         detected, format mismatch, or buffer allocation failure).
 *
 * @note Requires @c CONFIG_OVE_AUDIO.
 * @see ove_audio_graph_start
 */
int  ove_audio_graph_build(struct ove_audio_graph *g);

/**
 * @brief Start the audio graph.
 *
 * Calls each node's @c start callback in topological order and
 * transitions the graph to @c OVE_AUDIO_GRAPH_RUNNING.  The graph must
 * be in the @c OVE_AUDIO_GRAPH_READY state.
 *
 * @param[in] g  Built graph instance.
 * @return 0 on success, negative error code on failure.
 *
 * @note Requires @c CONFIG_OVE_AUDIO.
 * @see ove_audio_graph_build, ove_audio_graph_stop
 */
int  ove_audio_graph_start(struct ove_audio_graph *g);

/**
 * @brief Stop the audio graph.
 *
 * Calls each node's @c stop callback in reverse topological order and
 * transitions the graph back to @c OVE_AUDIO_GRAPH_READY.
 *
 * @param[in] g  Running graph instance.
 * @return 0 on success, negative error code on failure.
 *
 * @note Requires @c CONFIG_OVE_AUDIO.
 * @see ove_audio_graph_start
 */
int  ove_audio_graph_stop(struct ove_audio_graph *g);

/**
 * @brief Execute one processing cycle (app-driven mode).
 *
 * Calls each node's @c process callback in topological order, passing
 * inter-node buffers along the edges.  Intended for test or offline use;
 * in sink-driven mode the hardware callback drives processing instead.
 *
 * @param[in] g  Running graph instance.
 * @return 0 on success, negative error code if any node reports an error.
 *
 * @note Requires @c CONFIG_OVE_AUDIO.
 * @see ove_audio_graph_start
 */
int  ove_audio_graph_process(struct ove_audio_graph *g);

/**
 * @brief Retrieve a snapshot of graph runtime statistics.
 *
 * Copies the current diagnostic counters from the graph into the
 * caller-supplied @p stats structure.
 *
 * @param[in]  g      Graph instance (running or ready).
 * @param[out] stats  Pointer to a caller-allocated structure that will
 *                    receive the statistics snapshot.
 * @return 0 on success, negative error code on failure.
 *
 * @note Requires @c CONFIG_OVE_AUDIO.
 * @see ove_audio_graph_stats
 */
int  ove_audio_graph_get_stats(const struct ove_audio_graph *g,
                               struct ove_audio_graph_stats *stats);

#else /* !CONFIG_OVE_AUDIO */

struct ove_audio_graph { int _unused; };

static inline int ove_audio_graph_init(struct ove_audio_graph *g,
                                       unsigned int f)
{ (void)g; (void)f; return -5; /* OVE_ERR_NOT_SUPPORTED */ }
static inline void ove_audio_graph_deinit(struct ove_audio_graph *g)
{ (void)g; }

#endif /* CONFIG_OVE_AUDIO */

/** @} */ /* end of ove_audio group */

#endif /* OVE_AUDIO_H */
