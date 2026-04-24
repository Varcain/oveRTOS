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
#include "ove/storage.h"

#ifdef __cplusplus
extern "C" {
#endif

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
    size_t                  buf_storage_size;                    /**< @brief Size of caller-provided storage (0 = heap-allocated). */

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
 * @brief Provide caller-owned storage for inter-node audio buffers.
 *
 * Must be called after @ref ove_audio_graph_init and before
 * @ref ove_audio_graph_build.  When set, @ref ove_audio_graph_build uses
 * this buffer instead of calling @c calloc, and @ref ove_audio_graph_deinit
 * will not free it.  This is the only way to build a graph under
 * @c CONFIG_OVE_ZERO_HEAP.  Use @ref OVE_AUDIO_GRAPH_STORAGE_BYTES to size
 * the backing array.
 *
 * @param[in] g        Graph instance in the @c OVE_AUDIO_GRAPH_IDLE state.
 * @param[in] storage  Pointer to caller-owned memory (≥ @p size bytes).
 * @param[in] size     Size of @p storage in bytes; must be large enough to
 *                     cover every non-sink node's output buffer.
 * @return 0 on success, negative error code on failure.
 *
 * @note Requires @c CONFIG_OVE_AUDIO.
 * @see OVE_AUDIO_GRAPH_STORAGE_BYTES
 */
int  ove_audio_graph_set_buf_storage(struct ove_audio_graph *g,
                                     void *storage, size_t size);

/**
 * @brief Conservative upper-bound byte count for graph buffer storage.
 *
 * Computes the worst-case storage required for inter-node audio buffers:
 * every non-sink node produces one buffer of @p frames × @p channels ×
 * @p sample_bytes.  Used by @ref ove_audio_graph_create and
 * @ref OVE_AUDIO_GRAPH_DEFINE to size a zero-heap static backing array.
 */
#define OVE_AUDIO_GRAPH_STORAGE_BYTES(nodes, frames, channels, sample_bytes) \
    ((size_t)(nodes) * (size_t)(frames) * (size_t)(channels) * (size_t)(sample_bytes))

/* ── _create / _destroy — unified across heap and zero-heap modes ──── */
#ifdef OVE_HEAP_AUDIO

/**
 * @brief Internal heap-backed graph creation function.
 *
 * Prefer the @ref ove_audio_graph_create macro which works in both heap
 * and zero-heap mode.  This function is the underlying implementation used
 * in heap mode — inter-node buffers are allocated from the heap when
 * @ref ove_audio_graph_build runs.
 *
 * @param[out] g      Graph instance to initialise.
 * @param[in]  frames Per-period frame count.
 * @return 0 on success, negative error code on failure.
 *
 * @see ove_audio_graph_create
 */
int ove_audio_graph_create_(struct ove_audio_graph *g, unsigned int frames);

/**
 * @brief Stop and tear down a graph created with @ref ove_audio_graph_create.
 *
 * @param[in] g  Graph instance returned from @ref ove_audio_graph_create.
 * @return 0 on success, negative error code on failure.
 */
int ove_audio_graph_destroy(struct ove_audio_graph *g);

/**
 * @brief Create an audio graph (works in both heap and zero-heap mode).
 *
 * In heap mode, delegates to @ref ove_audio_graph_create_ — the per-node
 * intermediate buffers are calloc'd during @ref ove_audio_graph_build and
 * the @p nodes / @p channels / @p sample_bytes arguments are unused.
 *
 * In zero-heap mode, emits a per-call-site @c static backing array sized
 * by @ref OVE_AUDIO_GRAPH_STORAGE_BYTES and attaches it automatically.
 * @p nodes, @p frames, @p channels, @p sample_bytes must be compile-time
 * integer constants.
 *
 * @param pg            Pointer to the @c ove_audio_graph instance to initialise.
 * @param frames        Per-period frame count.
 * @param nodes         Maximum number of non-sink nodes in the graph.
 * @param channels      Maximum output channel count across all nodes.
 * @param sample_bytes  Widest sample size in bytes (e.g. 2 for S16, 4 for S32).
 */
#define ove_audio_graph_create(pg, frames, nodes, channels, sample_bytes) \
    ((void)(nodes), (void)(channels), (void)(sample_bytes),               \
     ove_audio_graph_create_((pg), (frames)))

#elif !defined(__ZIG_CIMPORT__) /* !OVE_HEAP_AUDIO — zero-heap mode */

/**
 * @brief Create an audio graph (zero-heap variant).
 *
 * Emits a per-call-site @c static backing array sized by
 * @ref OVE_AUDIO_GRAPH_STORAGE_BYTES and attaches it automatically.
 * @p nodes, @p frames, @p channels, @p sample_bytes must be compile-time
 * integer constants.
 *
 * @param pg            Pointer to the @c ove_audio_graph instance to initialise.
 * @param frames        Per-period frame count.
 * @param nodes         Maximum number of non-sink nodes in the graph.
 * @param channels      Maximum output channel count across all nodes.
 * @param sample_bytes  Widest sample size in bytes (e.g. 2 for S16, 4 for S32).
 */
#define ove_audio_graph_create(pg, frames, nodes, channels, sample_bytes)           \
    ({                                                                              \
        static uint8_t _ove_ag_stor_                                                \
            [OVE_AUDIO_GRAPH_STORAGE_BYTES((nodes), (frames),                       \
                                           (channels), (sample_bytes))]             \
            __attribute__((aligned(4)));                                            \
        int _r = ove_audio_graph_init((pg), (frames));                              \
        if (_r == OVE_OK) {                                                         \
            _r = ove_audio_graph_set_buf_storage((pg), _ove_ag_stor_,               \
                                                 sizeof(_ove_ag_stor_));            \
        }                                                                           \
        _r;                                                                         \
    })

#define ove_audio_graph_destroy(pg) (ove_audio_graph_deinit(pg), OVE_OK)

#endif /* OVE_HEAP_AUDIO */

/**
 * @brief Declare named static storage for an audio graph and its buffers.
 *
 * Mirrors @c OVE_THREAD_DEFINE / @c OVE_QUEUE_DEFINE: emits two file-scope
 * @c static objects: the @c ove_audio_graph instance named @p name, and a
 * byte array named @c name##_buf sized via @ref OVE_AUDIO_GRAPH_STORAGE_BYTES.
 * Pair with @ref ove_audio_graph_init and @ref ove_audio_graph_set_buf_storage
 * when the application wants explicit control:
 *
 * @code
 * OVE_AUDIO_GRAPH_DEFINE(g, 2, 512, 1, 2);
 *
 * ove_audio_graph_init(&g, 512);
 * ove_audio_graph_set_buf_storage(&g, g_buf, sizeof(g_buf));
 * @endcode
 *
 * Applications that do not need this level of control should prefer
 * @ref ove_audio_graph_create, which allocates and attaches storage in
 * one step.
 */
#define OVE_AUDIO_GRAPH_DEFINE(name, nodes, frames, channels, sample_bytes)         \
    static uint8_t name##_buf                                                       \
        [OVE_AUDIO_GRAPH_STORAGE_BYTES((nodes), (frames),                           \
                                       (channels), (sample_bytes))]                 \
        __attribute__((aligned(4)));                                                \
    static struct ove_audio_graph name

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

#ifdef __cplusplus
}
#endif

/** @} */ /* end of ove_audio group */

#endif /* OVE_AUDIO_H */
