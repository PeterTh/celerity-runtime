#pragma once

#include <mutex>
#include <utility>

#include <boost/graph/adjacency_list.hpp>
// As of Boost 1.70, this includes a header which contains a __noinline__ attribute
// for __GNUC__ == 4 (which Clang (8) apparently also identifies as).
// This breaks CUDA compilation with Clang, as the CUDA (10) headers define __noinline__
// in an incompatible manner. As a workaround we thus simply undefine it altogether.
// Potentially related to https://svn.boost.org/trac10/ticket/9392
#if defined(__clang__) && defined(__CUDA__)
#undef __noinline__
#endif
#include <boost/graph/graphviz.hpp>

#include "command.h"
#include "types.h"

#define GRAPH_PROP(Graph, PropertyName) Graph[boost::graph_bundle].PropertyName

// Since the task graph uses vecS vertex storage, the vertex descriptor is just size_t.
MAKE_PHANTOM_TYPE(tdag_vertex, size_t)

namespace celerity {
namespace detail {

	template <typename Graph>
	class locked_graph {
	  public:
		locked_graph(Graph& graph, std::mutex& mutex) : graph(graph), lock(mutex) {}
		locked_graph(locked_graph<Graph>&& other) noexcept : graph(other.graph) { lock = std::move(other.lock); }

		locked_graph(const locked_graph<Graph>& other) = delete;
		locked_graph& operator=(const locked_graph<Graph>& other) = delete;
		locked_graph& operator=(locked_graph<Graph>&& other) = delete;

		Graph& operator*() { return graph; };
		Graph& operator*() const { return graph; };

	  private:
		Graph& graph;
		std::unique_lock<std::mutex> lock;
	};

	// -------------------------------------------------------------------------------------------------------------------
	// --------------------------------------------------- TASK GRAPH ----------------------------------------------------
	// -------------------------------------------------------------------------------------------------------------------

	struct tdag_vertex_properties {
		std::string label;

		// Whether this task has been processed into the command dag
		bool processed = false;

		// The number of unsatisfied (= unprocessed) dependencies this task has
		size_t num_unsatisfied = 0;
	};

	struct tdag_edge_properties {
		// An anti-dependency indicates that the source task uses a buffer that is written to by the target task
		// (I.e. avoding write after read race conditions)
		bool anti_dependency = false;
	};

	struct tdag_graph_properties {};

	using task_dag =
	    boost::adjacency_list<boost::vecS, boost::vecS, boost::bidirectionalS, tdag_vertex_properties, tdag_edge_properties, tdag_graph_properties>;
	using tdag_edge = task_dag::edge_descriptor;

	// -------------------------------------------------------------------------------------------------------------------
	// -------------------------------------------------- COMMAND GRAPH --------------------------------------------------
	// -------------------------------------------------------------------------------------------------------------------

	// TODO: Consider also using lists here (memory / perf tradeoff)
	using cdag_OutEdgeListS = boost::vecS;
	// NOTE: It's important that we use a list or set to store vertices, so iterators are not invalidated upon deletion of vertices
	// (Since we keep various external data structures pointing back into the graph)
	using cdag_VertexListS = boost::listS;
	// We need bidirectionalS even though it's a DAG, so we can access incoming as well as outgoing edges on a vertex.
	using cdag_DirectedS = boost::bidirectionalS;
	using cdag_EdgeListS = boost::listS;

	// We can't get this type using graph_traits since we need it within the bundled properties. Fortunately adjacency_list_traits exists for exactly this
	// reason!
	using cdag_vertex = boost::adjacency_list_traits<cdag_OutEdgeListS, cdag_VertexListS, cdag_DirectedS, cdag_EdgeListS>::vertex_descriptor;
	constexpr cdag_vertex cdag_vertex_none = static_cast<cdag_vertex>(nullptr);

	using cdag_edge = boost::adjacency_list_traits<cdag_OutEdgeListS, cdag_VertexListS, cdag_DirectedS, cdag_EdgeListS>::edge_descriptor;

	struct cdag_vertex_properties {
		std::string label;
		command cmd = command::NOP;
		command_id cid;
		node_id nid = 0;
		task_id tid;
		command_data data = {};
	};

	struct cdag_edge_properties {
		// An anti-dependency indicates that the source command uses a buffer that is written to by the target command
		// (I.e. avoding write after read race conditions)
		bool anti_dependency = false;
	};

	struct cdag_graph_properties {
		command_id next_cmd_id = 0;

		// Stores the begin/end commands for each task.
		std::unordered_map<task_id, std::pair<cdag_vertex, cdag_vertex>> task_vertices;

		// Stores the corresponding vertex for each command id.
		std::unordered_map<command_id, cdag_vertex> command_vertices;
	};

	using command_dag = boost::adjacency_list<cdag_OutEdgeListS, cdag_VertexListS, cdag_DirectedS,
	    // Add vertex_index_t as a vertex property alongside the bundled properties since it's required by certain BGL algorithms (e.g. write_graphviz)
	    boost::property<boost::vertex_index_t, int, cdag_vertex_properties>, cdag_edge_properties, cdag_graph_properties, cdag_EdgeListS>;

} // namespace detail
} // namespace celerity
