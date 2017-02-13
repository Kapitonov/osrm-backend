#include "partition/inertial_flow.hpp"
#include "partition/bisection_graph.hpp"
#include "partition/reorder_first_last.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <mutex>
#include <set>
#include <tuple>
#include <utility>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

namespace osrm
{
namespace partition
{

InertialFlow::InertialFlow(const GraphView &view_) : view(view_) {}

DinicMaxFlow::MinCut InertialFlow::ComputePartition(const std::size_t num_slopes,
                                                    const double balance,
                                                    const double source_sink_rate,
                                                    visual::InertialFlowProgress &progress)
{
    return BestMinCut(num_slopes, source_sink_rate, balance, progress);
}

InertialFlow::SpatialOrder InertialFlow::MakeSpatialOrder(const double ratio,
                                                          const double slope) const
{
    struct NodeWithCoordinate
    {
        NodeWithCoordinate(NodeID nid_, util::Coordinate coordinate_)
            : nid{nid_}, coordinate{std::move(coordinate_)}
        {
        }

        NodeID nid;
        util::Coordinate coordinate;
    };

    using Embedding = std::vector<NodeWithCoordinate>;

    Embedding embedding;
    embedding.reserve(view.NumberOfNodes());

    // adress of the very first node
    const auto node_zero = &(*view.Begin());

    std::transform(view.Begin(), view.End(), std::back_inserter(embedding), [&](const auto &node) {
        const auto node_id = static_cast<NodeID>(&node - node_zero);
        return NodeWithCoordinate{node_id, node.coordinate};
    });

    const auto project = [slope](const auto &each) {
        auto lon = static_cast<std::int32_t>(each.coordinate.lon);
        auto lat = static_cast<std::int32_t>(each.coordinate.lat);

        return slope * lon + (1. - std::fabs(slope)) * lat;
    };

    const auto spatially = [&](const auto &lhs, const auto &rhs) {
        return project(lhs) < project(rhs);
    };

    const std::size_t n = ratio * embedding.size();

    reorderFirstLast(embedding, n, spatially);

    InertialFlow::SpatialOrder order;

    order.sources.reserve(n);
    order.sinks.reserve(n);

    for (auto it = begin(embedding), last = begin(embedding) + n; it != last; ++it)
        order.sources.insert(it->nid);

    for (auto it = end(embedding) - n, last = end(embedding); it != last; ++it)
        order.sinks.insert(it->nid);

    return order;
}

DinicMaxFlow::MinCut InertialFlow::BestMinCut(const std::size_t n,
                                              const double ratio,
                                              const double balance,
                                              visual::InertialFlowProgress &progress) const
{
    DinicMaxFlow::MinCut best;
    best.num_edges = -1;

    const auto get_balance = [this, balance](const auto num_nodes_source) {
        const auto perfect_balance = view.NumberOfNodes() / 2;
        const auto allowed_balance = balance * perfect_balance;
        const auto bigger_side =
            std::max(num_nodes_source, view.NumberOfNodes() - num_nodes_source);

        if (bigger_side > allowed_balance)
            return bigger_side / static_cast<double>(allowed_balance);
        else
            return 1.0;
    };

    auto best_balance = 1;

    std::mutex lock;

    tbb::blocked_range<std::size_t> range{0, n, 1};

    const auto balance_delta = [this](const auto num_nodes_source) {
        const std::int64_t difference =
            static_cast<std::int64_t>(view.NumberOfNodes()) / 2 - num_nodes_source;
        return std::abs(difference);
    };

    tbb::parallel_for(range, [&, this](const auto &chunk) {
        for (auto round = chunk.begin(), end = chunk.end(); round != end; ++round)
        {
            const auto slope = -1. + round * (2. / n);

            auto order = this->MakeSpatialOrder(ratio, slope);
            auto cut = DinicMaxFlow()(view, order.sources, order.sinks);
            auto cut_balance = get_balance(cut.num_nodes_source);

            {
                std::lock_guard<std::mutex> guard{lock};

                // Swap to keep the destruction of the old object outside of critical section.
                if (cut.num_edges * cut_balance < best.num_edges * best_balance ||
                    (cut.num_edges == best.num_edges &&
                     balance_delta(cut.num_nodes_source) < balance_delta(best.num_nodes_source)))
                {
                    best_balance = cut_balance;
                    progress.best_cut = cut.cut_vis.cut;
                    std::swap(best, cut);
                }

                progress.cuts_by_slope.push_back(cut.cut_vis);
            }
            // cut gets destroyed here
        }
    });

    return best;
}

} // namespace partition
} // namespace osrm