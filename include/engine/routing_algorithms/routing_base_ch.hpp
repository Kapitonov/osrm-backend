#ifndef OSRM_ENGINE_ROUTING_BASE_CH_HPP
#define OSRM_ENGINE_ROUTING_BASE_CH_HPP

#include "engine/algorithm.hpp"
#include "engine/datafacade/contiguous_internalmem_datafacade.hpp"
#include "engine/routing_algorithms/routing_base.hpp"
#include "engine/search_engine_data.hpp"

#include "util/typedefs.hpp"

#include <boost/assert.hpp>

namespace osrm
{
namespace engine
{

namespace routing_algorithms
{

namespace ch
{

// Stalling
template <bool DIRECTION, typename HeapT>
bool stallAtNode(const datafacade::ContiguousInternalMemoryDataFacade<algorithm::CH> &facade,
                 const NodeID node,
                 const EdgeWeight weight,
                 const HeapT &query_heap)
{
    for (auto edge : facade.GetAdjacentEdgeRange(node))
    {
        const auto &data = facade.GetEdgeData(edge);
        if (DIRECTION == REVERSE_DIRECTION ? data.forward : data.backward)
        {
            const NodeID to = facade.GetTarget(edge);
            const EdgeWeight edge_weight = data.weight;
            BOOST_ASSERT_MSG(edge_weight > 0, "edge_weight invalid");
            if (query_heap.WasInserted(to))
            {
                if (query_heap.GetKey(to) + edge_weight < weight)
                {
                    return true;
                }
            }
        }
    }
    return false;
}

template <bool DIRECTION>
void relaxOutgoingEdges(const datafacade::ContiguousInternalMemoryDataFacade<algorithm::CH> &facade,
                        const NodeID node,
                        const EdgeWeight weight,
                        SearchEngineData::QueryHeap &heap)
{
    for (const auto edge : facade.GetAdjacentEdgeRange(node))
    {
        const auto &data = facade.GetEdgeData(edge);
        if (DIRECTION == FORWARD_DIRECTION ? data.forward : data.backward)
        {
            const NodeID to = facade.GetTarget(edge);
            const EdgeWeight edge_weight = data.weight;

            BOOST_ASSERT_MSG(edge_weight > 0, "edge_weight invalid");
            const EdgeWeight to_weight = weight + edge_weight;

            // New Node discovered -> Add to Heap + Node Info Storage
            if (!heap.WasInserted(to))
            {
                heap.Insert(to, to_weight, node);
            }
            // Found a shorter Path -> Update weight
            else if (to_weight < heap.GetKey(to))
            {
                // new parent
                heap.GetData(to).parent = node;
                heap.DecreaseKey(to, to_weight);
            }
        }
    }
}

/*
min_edge_offset is needed in case we use multiple
nodes as start/target nodes with different (even negative) offsets.
In that case the termination criterion is not correct
anymore.

Example:
forward heap: a(-100), b(0),
reverse heap: c(0), d(100)

a --- d
  \ /
  / \
b --- c

This is equivalent to running a bi-directional Dijkstra on the following graph:

    a --- d
   /  \ /  \
  y    x    z
   \  / \  /
    b --- c

The graph is constructed by inserting nodes y and z that are connected to the initial nodes
using edges (y, a) with weight -100, (y, b) with weight 0 and,
(d, z) with weight 100, (c, z) with weight 0 corresponding.
Since we are dealing with a graph that contains _negative_ edges,
we need to add an offset to the termination criterion.
*/
static constexpr bool ENABLE_STALLING = true;
static constexpr bool DISABLE_STALLING = false;
template <bool DIRECTION, bool STALLING = ENABLE_STALLING>
void routingStep(const datafacade::ContiguousInternalMemoryDataFacade<algorithm::CH> &facade,
                 SearchEngineData::QueryHeap &forward_heap,
                 SearchEngineData::QueryHeap &reverse_heap,
                 NodeID &middle_node_id,
                 EdgeWeight &upper_bound,
                 EdgeWeight min_edge_offset,
                 const bool force_loop_forward,
                 const bool force_loop_reverse)
{
    const NodeID node = forward_heap.DeleteMin();
    const EdgeWeight weight = forward_heap.GetKey(node);

    if (reverse_heap.WasInserted(node))
    {
        const EdgeWeight new_weight = reverse_heap.GetKey(node) + weight;
        if (new_weight < upper_bound)
        {
            // if loops are forced, they are so at the source
            if ((force_loop_forward && forward_heap.GetData(node).parent == node) ||
                (force_loop_reverse && reverse_heap.GetData(node).parent == node) ||
                // in this case we are looking at a bi-directional way where the source
                // and target phantom are on the same edge based node
                new_weight < 0)
            {
                // check whether there is a loop present at the node
                for (const auto edge : facade.GetAdjacentEdgeRange(node))
                {
                    const auto &data = facade.GetEdgeData(edge);
                    if (DIRECTION == FORWARD_DIRECTION ? data.forward : data.backward)
                    {
                        const NodeID to = facade.GetTarget(edge);
                        if (to == node)
                        {
                            const EdgeWeight edge_weight = data.weight;
                            const EdgeWeight loop_weight = new_weight + edge_weight;
                            if (loop_weight >= 0 && loop_weight < upper_bound)
                            {
                                middle_node_id = node;
                                upper_bound = loop_weight;
                            }
                        }
                    }
                }
            }
            else
            {
                BOOST_ASSERT(new_weight >= 0);

                middle_node_id = node;
                upper_bound = new_weight;
            }
        }
    }

    // make sure we don't terminate too early if we initialize the weight
    // for the nodes in the forward heap with the forward/reverse offset
    BOOST_ASSERT(min_edge_offset <= 0);
    if (weight + min_edge_offset > upper_bound)
    {
        forward_heap.DeleteAll();
        return;
    }

    // Stalling
    if (STALLING && stallAtNode<DIRECTION>(facade, node, weight, forward_heap))
    {
        return;
    }

    relaxOutgoingEdges<DIRECTION>(facade, node, weight, forward_heap);
}

template <bool UseDuration>
EdgeWeight
getLoopWeight(const datafacade::ContiguousInternalMemoryDataFacade<algorithm::CH> &facade,
              NodeID node)
{
    EdgeWeight loop_weight = UseDuration ? MAXIMAL_EDGE_DURATION : INVALID_EDGE_WEIGHT;
    for (auto edge : facade.GetAdjacentEdgeRange(node))
    {
        const auto &data = facade.GetEdgeData(edge);
        if (data.forward)
        {
            const NodeID to = facade.GetTarget(edge);
            if (to == node)
            {
                const auto value = UseDuration ? data.duration : data.weight;
                loop_weight = std::min(loop_weight, value);
            }
        }
    }
    return loop_weight;
}

/**
 * Given a sequence of connected `NodeID`s in the CH graph, performs a depth-first unpacking of
 * the shortcut
 * edges.  For every "original" edge found, it calls the `callback` with the two NodeIDs for the
 * edge, and the EdgeData
 * for that edge.
 *
 * The primary purpose of this unpacking is to expand a path through the CH into the original
 * route through the
 * pre-contracted graph.
 *
 * Because of the depth-first-search, the `callback` will effectively be called in sequence for
 * the original route
 * from beginning to end.
 *
 * @param packed_path_begin iterator pointing to the start of the NodeID list
 * @param packed_path_end iterator pointing to the end of the NodeID list
 * @param callback void(const std::pair<NodeID, NodeID>, const EdgeID &) called for each
 * original edge found.
 */
template <typename BidirectionalIterator, typename Callback>
void unpackPath(const datafacade::ContiguousInternalMemoryDataFacade<algorithm::CH> &facade,
                BidirectionalIterator packed_path_begin,
                BidirectionalIterator packed_path_end,
                Callback &&callback)
{
    // make sure we have at least something to unpack
    if (packed_path_begin == packed_path_end)
        return;

    std::stack<std::pair<NodeID, NodeID>> recursion_stack;

    // We have to push the path in reverse order onto the stack because it's LIFO.
    for (auto current = std::prev(packed_path_end); current != packed_path_begin;
         current = std::prev(current))
    {
        recursion_stack.emplace(*std::prev(current), *current);
    }

    std::pair<NodeID, NodeID> edge;
    while (!recursion_stack.empty())
    {
        edge = recursion_stack.top();
        recursion_stack.pop();

        // Look for an edge on the forward CH graph (.forward)
        EdgeID smaller_edge_id = facade.FindSmallestEdge(
            edge.first, edge.second, [](const auto &data) { return data.forward; });

        // If we didn't find one there, the we might be looking at a part of the path that
        // was found using the backward search.  Here, we flip the node order (.second, .first)
        // and only consider edges with the `.backward` flag.
        if (SPECIAL_EDGEID == smaller_edge_id)
        {
            smaller_edge_id = facade.FindSmallestEdge(
                edge.second, edge.first, [](const auto &data) { return data.backward; });
        }

        // If we didn't find anything *still*, then something is broken and someone has
        // called this function with bad values.
        BOOST_ASSERT_MSG(smaller_edge_id != SPECIAL_EDGEID, "Invalid smaller edge ID");

        const auto &data = facade.GetEdgeData(smaller_edge_id);
        BOOST_ASSERT_MSG(data.weight != std::numeric_limits<EdgeWeight>::max(),
                         "edge weight invalid");

        // If the edge is a shortcut, we need to add the two halfs to the stack.
        if (data.shortcut)
        { // unpack
            const NodeID middle_node_id = data.turn_id;
            // Note the order here - we're adding these to a stack, so we
            // want the first->middle to get visited before middle->second
            recursion_stack.emplace(middle_node_id, edge.second);
            recursion_stack.emplace(edge.first, middle_node_id);
        }
        else
        {
            // We found an original edge, call our callback.
            std::forward<Callback>(callback)(edge, smaller_edge_id);
        }
    }
}

template <typename RandomIter, typename FacadeT>
void unpackPath(const FacadeT &facade,
                RandomIter packed_path_begin,
                RandomIter packed_path_end,
                const PhantomNodes &phantom_nodes,
                std::vector<PathData> &unpacked_path)
{
    const auto nodes_number = std::distance(packed_path_begin, packed_path_end);
    BOOST_ASSERT(nodes_number > 0);

    std::vector<EdgeID> unpacked_edges;

    auto source_node = *packed_path_begin, target_node = *packed_path_begin;
    if (nodes_number > 1)
    {
        target_node = *std::prev(packed_path_end);
        unpacked_edges.reserve(std::distance(packed_path_begin, packed_path_end));
        unpackPath(
            facade,
            packed_path_begin,
            packed_path_end,
            [&facade, &unpacked_edges](std::pair<NodeID, NodeID> & /* edge */,
                                       const auto &edge_id) { unpacked_edges.push_back(edge_id); });
    }

    annotatePath(facade, source_node, target_node, unpacked_edges, phantom_nodes, unpacked_path);
}

/**
 * Unpacks a single edge (NodeID->NodeID) from the CH graph down to it's original non-shortcut
 * route.
 * @param from the node the CH edge starts at
 * @param to the node the CH edge finishes at
 * @param unpacked_path the sequence of original NodeIDs that make up the expanded CH edge
 */
void unpackEdge(const datafacade::ContiguousInternalMemoryDataFacade<algorithm::CH> &facade,
                const NodeID from,
                const NodeID to,
                std::vector<NodeID> &unpacked_path);

void retrievePackedPathFromHeap(const SearchEngineData::QueryHeap &forward_heap,
                                const SearchEngineData::QueryHeap &reverse_heap,
                                const NodeID middle_node_id,
                                std::vector<NodeID> &packed_path);

void retrievePackedPathFromSingleHeap(const SearchEngineData::QueryHeap &search_heap,
                                      const NodeID middle_node_id,
                                      std::vector<NodeID> &packed_path);

// assumes that heaps are already setup correctly.
// ATTENTION: This only works if no additional offset is supplied next to the Phantom Node
// Offsets.
// In case additional offsets are supplied, you might have to force a loop first.
// A forced loop might be necessary, if source and target are on the same segment.
// If this is the case and the offsets of the respective direction are larger for the source
// than the target
// then a force loop is required (e.g. source_phantom.forward_segment_id ==
// target_phantom.forward_segment_id
// && source_phantom.GetForwardWeightPlusOffset() > target_phantom.GetForwardWeightPlusOffset())
// requires
// a force loop, if the heaps have been initialized with positive offsets.
void search(const datafacade::ContiguousInternalMemoryDataFacade<algorithm::CH> &facade,
            SearchEngineData::QueryHeap &forward_heap,
            SearchEngineData::QueryHeap &reverse_heap,
            std::int32_t &weight,
            std::vector<NodeID> &packed_leg,
            const bool force_loop_forward,
            const bool force_loop_reverse,
            const int duration_upper_bound = INVALID_EDGE_WEIGHT);

// Alias to be compatible with the overload for CoreCH that needs 4 heaps
inline void search(const datafacade::ContiguousInternalMemoryDataFacade<algorithm::CH> &facade,
                   SearchEngineData::QueryHeap &forward_heap,
                   SearchEngineData::QueryHeap &reverse_heap,
                   SearchEngineData::QueryHeap &,
                   SearchEngineData::QueryHeap &,
                   std::int32_t &weight,
                   std::vector<NodeID> &packed_leg,
                   const bool force_loop_forward,
                   const bool force_loop_reverse,
                   const int duration_upper_bound = INVALID_EDGE_WEIGHT)
{
    search(facade,
           forward_heap,
           reverse_heap,
           weight,
           packed_leg,
           force_loop_forward,
           force_loop_reverse,
           duration_upper_bound);
}

// assumes that heaps are already setup correctly.
// A forced loop might be necessary, if source and target are on the same segment.
// If this is the case and the offsets of the respective direction are larger for the source
// than the target
// then a force loop is required (e.g. source_phantom.forward_segment_id ==
// target_phantom.forward_segment_id
// && source_phantom.GetForwardWeightPlusOffset() > target_phantom.GetForwardWeightPlusOffset())
// requires
// a force loop, if the heaps have been initialized with positive offsets.
void search(const datafacade::ContiguousInternalMemoryDataFacade<algorithm::CoreCH> &facade,
            SearchEngineData::QueryHeap &forward_heap,
            SearchEngineData::QueryHeap &reverse_heap,
            SearchEngineData::QueryHeap &forward_core_heap,
            SearchEngineData::QueryHeap &reverse_core_heap,
            int &weight,
            std::vector<NodeID> &packed_leg,
            const bool force_loop_forward,
            const bool force_loop_reverse,
            int duration_upper_bound = INVALID_EDGE_WEIGHT);

bool needsLoopForward(const PhantomNode &source_phantom, const PhantomNode &target_phantom);

bool needsLoopBackwards(const PhantomNode &source_phantom, const PhantomNode &target_phantom);

double getPathDistance(const datafacade::ContiguousInternalMemoryDataFacade<algorithm::CH> &facade,
                       const std::vector<NodeID> &packed_path,
                       const PhantomNode &source_phantom,
                       const PhantomNode &target_phantom);

// Requires the heaps for be empty
// If heaps should be adjusted to be initialized outside of this function,
// the addition of force_loop parameters might be required
double
getNetworkDistance(const datafacade::ContiguousInternalMemoryDataFacade<algorithm::CoreCH> &facade,
                   SearchEngineData::QueryHeap &forward_heap,
                   SearchEngineData::QueryHeap &reverse_heap,
                   SearchEngineData::QueryHeap &forward_core_heap,
                   SearchEngineData::QueryHeap &reverse_core_heap,
                   const PhantomNode &source_phantom,
                   const PhantomNode &target_phantom,
                   int duration_upper_bound = INVALID_EDGE_WEIGHT);

// Requires the heaps for be empty
// If heaps should be adjusted to be initialized outside of this function,
// the addition of force_loop parameters might be required
double
getNetworkDistance(const datafacade::ContiguousInternalMemoryDataFacade<algorithm::CH> &facade,
                   SearchEngineData::QueryHeap &forward_heap,
                   SearchEngineData::QueryHeap &reverse_heap,
                   const PhantomNode &source_phantom,
                   const PhantomNode &target_phantom,
                   int duration_upper_bound = INVALID_EDGE_WEIGHT);

// Alias to be compatible with the overload for CoreCH that needs 4 heaps
inline double
getNetworkDistance(const datafacade::ContiguousInternalMemoryDataFacade<algorithm::CH> &facade,
                   SearchEngineData::QueryHeap &forward_heap,
                   SearchEngineData::QueryHeap &reverse_heap,
                   SearchEngineData::QueryHeap &,
                   SearchEngineData::QueryHeap &,
                   const PhantomNode &source_phantom,
                   const PhantomNode &target_phantom,
                   int duration_upper_bound = INVALID_EDGE_WEIGHT)
{
    return getNetworkDistance(
        facade, forward_heap, reverse_heap, source_phantom, target_phantom, duration_upper_bound);
}

} // namespace ch
} // namespace routing_algorithms
} // namespace engine
} // namespace osrm

#endif // OSRM_ENGINE_ROUTING_BASE_CH_HPP
