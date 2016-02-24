#include "extractor/edge_based_edge.hpp"
#include "extractor/edge_based_graph_factory.hpp"
#include "util/coordinate.hpp"
#include "util/coordinate_calculation.hpp"
#include "util/percent.hpp"
#include "util/integer_range.hpp"
#include "util/lua_util.hpp"
#include "util/simple_logger.hpp"
#include "util/timing_util.hpp"
#include "util/exception.hpp"

#include "engine/guidance/turn_classification.hpp"
#include "engine/guidance/guidance_toolkit.hpp"

#include <boost/assert.hpp>
#include <boost/numeric/conversion/cast.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>

namespace osrm
{
namespace extractor
{

// configuration of turn classification
const bool constexpr INVERT = true;
const bool constexpr RESOLVE_TO_RIGHT = true;
const bool constexpr RESOLVE_TO_LEFT = false;

// what angle is interpreted as going straight
const double constexpr STRAIGHT_ANGLE = 180.;
// if a turn deviates this much from going straight, it will be kept straight
const double constexpr MAXIMAL_ALLOWED_NO_TURN_DEVIATION = 2.;
// angle that lies between two nearly indistinguishable roads
const double constexpr NARROW_TURN_ANGLE = 35.;
// angle difference that can be classified as straight, if its the only narrow turn
const double constexpr FUZZY_STRAIGHT_ANGLE = 15.;
const double constexpr DISTINCTION_RATIO = 2;

using engine::guidance::TurnPossibility;
using engine::guidance::TurnInstruction;
using engine::guidance::DirectionModifier;
using engine::guidance::TurnType;
using engine::guidance::FunctionalRoadClass;

using engine::guidance::classifyIntersection;
using engine::guidance::isLowPriorityRoadClass;
using engine::guidance::angularDeviation;
using engine::guidance::getTurnDirection;
using engine::guidance::getRepresentativeCoordinate;
using engine::guidance::isBasic;
using engine::guidance::isRampClass;
using engine::guidance::isUturn;
using engine::guidance::isConflict;
using engine::guidance::isSlightTurn;
using engine::guidance::isSlightModifier;
using engine::guidance::mirrorDirectionModifier;

// Configuration to find representative candidate for turn angle calculations

EdgeBasedGraphFactory::EdgeBasedGraphFactory(
    std::shared_ptr<util::NodeBasedDynamicGraph> node_based_graph,
    const CompressedEdgeContainer &compressed_edge_container,
    const std::unordered_set<NodeID> &barrier_nodes,
    const std::unordered_set<NodeID> &traffic_lights,
    std::shared_ptr<const RestrictionMap> restriction_map,
    const std::vector<QueryNode> &node_info_list,
    SpeedProfileProperties speed_profile)
    : m_max_edge_id(0), m_node_info_list(node_info_list),
      m_node_based_graph(std::move(node_based_graph)),
      m_restriction_map(std::move(restriction_map)), m_barrier_nodes(barrier_nodes),
      m_traffic_lights(traffic_lights), m_compressed_edge_container(compressed_edge_container),
      speed_profile(std::move(speed_profile))
{
}

void EdgeBasedGraphFactory::GetEdgeBasedEdges(
    util::DeallocatingVector<EdgeBasedEdge> &output_edge_list)
{
    BOOST_ASSERT_MSG(0 == output_edge_list.size(), "Vector is not empty");
    using std::swap; // Koenig swap
    swap(m_edge_based_edge_list, output_edge_list);
}

void EdgeBasedGraphFactory::GetEdgeBasedNodes(std::vector<EdgeBasedNode> &nodes)
{
#ifndef NDEBUG
    for (const EdgeBasedNode &node : m_edge_based_node_list)
    {
        BOOST_ASSERT(
            util::Coordinate(m_node_info_list[node.u].lon, m_node_info_list[node.u].lat).IsValid());
        BOOST_ASSERT(
            util::Coordinate(m_node_info_list[node.v].lon, m_node_info_list[node.v].lat).IsValid());
    }
#endif
    using std::swap; // Koenig swap
    swap(nodes, m_edge_based_node_list);
}

void EdgeBasedGraphFactory::GetStartPointMarkers(std::vector<bool> &node_is_startpoint)
{
    using std::swap; // Koenig swap
    swap(m_edge_based_node_is_startpoint, node_is_startpoint);
}

void EdgeBasedGraphFactory::GetEdgeBasedNodeWeights(std::vector<EdgeWeight> &output_node_weights)
{
    using std::swap; // Koenig swap
    swap(m_edge_based_node_weights, output_node_weights);
}

unsigned EdgeBasedGraphFactory::GetHighestEdgeID() { return m_max_edge_id; }

void EdgeBasedGraphFactory::InsertEdgeBasedNode(const NodeID node_u, const NodeID node_v)
{
    // merge edges together into one EdgeBasedNode
    BOOST_ASSERT(node_u != SPECIAL_NODEID);
    BOOST_ASSERT(node_v != SPECIAL_NODEID);

    // find forward edge id and
    const EdgeID edge_id_1 = m_node_based_graph->FindEdge(node_u, node_v);
    BOOST_ASSERT(edge_id_1 != SPECIAL_EDGEID);

    const EdgeData &forward_data = m_node_based_graph->GetEdgeData(edge_id_1);

    // find reverse edge id and
    const EdgeID edge_id_2 = m_node_based_graph->FindEdge(node_v, node_u);
    BOOST_ASSERT(edge_id_2 != SPECIAL_EDGEID);

    const EdgeData &reverse_data = m_node_based_graph->GetEdgeData(edge_id_2);

    if (forward_data.edge_id == SPECIAL_NODEID && reverse_data.edge_id == SPECIAL_NODEID)
    {
        return;
    }

    if (forward_data.edge_id != SPECIAL_NODEID && reverse_data.edge_id == SPECIAL_NODEID)
        m_edge_based_node_weights[forward_data.edge_id] = INVALID_EDGE_WEIGHT;

    BOOST_ASSERT(m_compressed_edge_container.HasEntryForID(edge_id_1) ==
                 m_compressed_edge_container.HasEntryForID(edge_id_2));
    BOOST_ASSERT(m_compressed_edge_container.HasEntryForID(edge_id_1));
    BOOST_ASSERT(m_compressed_edge_container.HasEntryForID(edge_id_2));
    const auto &forward_geometry = m_compressed_edge_container.GetBucketReference(edge_id_1);
    BOOST_ASSERT(forward_geometry.size() ==
                 m_compressed_edge_container.GetBucketReference(edge_id_2).size());
    const auto geometry_size = forward_geometry.size();

    // There should always be some geometry
    BOOST_ASSERT(0 != geometry_size);

    NodeID current_edge_source_coordinate_id = node_u;

    // traverse arrays from start and end respectively
    for (const auto i : util::irange(std::size_t{ 0 }, geometry_size))
    {
        BOOST_ASSERT(
            current_edge_source_coordinate_id ==
            m_compressed_edge_container.GetBucketReference(edge_id_2)[geometry_size - 1 - i]
                .node_id);
        const NodeID current_edge_target_coordinate_id = forward_geometry[i].node_id;
        BOOST_ASSERT(current_edge_target_coordinate_id != current_edge_source_coordinate_id);

        // build edges
        m_edge_based_node_list.emplace_back(
            forward_data.edge_id, reverse_data.edge_id, current_edge_source_coordinate_id,
            current_edge_target_coordinate_id, forward_data.name_id,
            m_compressed_edge_container.GetPositionForID(edge_id_1),
            m_compressed_edge_container.GetPositionForID(edge_id_2), false, INVALID_COMPONENTID, i,
            forward_data.travel_mode, reverse_data.travel_mode);

        m_edge_based_node_is_startpoint.push_back(forward_data.startpoint ||
                                                  reverse_data.startpoint);
        current_edge_source_coordinate_id = current_edge_target_coordinate_id;
    }

    BOOST_ASSERT(current_edge_source_coordinate_id == node_v);
}

void EdgeBasedGraphFactory::FlushVectorToStream(
    std::ofstream &edge_data_file, std::vector<OriginalEdgeData> &original_edge_data_vector) const
{
    if (original_edge_data_vector.empty())
    {
        return;
    }
    edge_data_file.write((char *)&(original_edge_data_vector[0]),
                         original_edge_data_vector.size() * sizeof(OriginalEdgeData));
    original_edge_data_vector.clear();
}

void EdgeBasedGraphFactory::Run(const std::string &original_edge_data_filename,
                                lua_State *lua_state,
                                const std::string &edge_segment_lookup_filename,
                                const std::string &edge_penalty_filename,
                                const bool generate_edge_lookup)
{
    TIMER_START(renumber);
    m_max_edge_id = RenumberEdges() - 1;
    TIMER_STOP(renumber);

    TIMER_START(generate_nodes);
    m_edge_based_node_weights.reserve(m_max_edge_id + 1);
    GenerateEdgeExpandedNodes();
    TIMER_STOP(generate_nodes);

    TIMER_START(generate_edges);
    GenerateEdgeExpandedEdges(original_edge_data_filename, lua_state, edge_segment_lookup_filename,
                              edge_penalty_filename, generate_edge_lookup);

    TIMER_STOP(generate_edges);

    util::SimpleLogger().Write() << "Timing statistics for edge-expanded graph:";
    util::SimpleLogger().Write() << "Renumbering edges: " << TIMER_SEC(renumber) << "s";
    util::SimpleLogger().Write() << "Generating nodes: " << TIMER_SEC(generate_nodes) << "s";
    util::SimpleLogger().Write() << "Generating edges: " << TIMER_SEC(generate_edges) << "s";
}

/// Renumbers all _forward_ edges and sets the edge_id.
/// A specific numbering is not important. Any unique ID will do.
/// Returns the number of edge based nodes.
unsigned EdgeBasedGraphFactory::RenumberEdges()
{
    // renumber edge based node of outgoing edges
    unsigned numbered_edges_count = 0;
    for (const auto current_node : util::irange(0u, m_node_based_graph->GetNumberOfNodes()))
    {
        for (const auto current_edge : m_node_based_graph->GetAdjacentEdgeRange(current_node))
        {
            EdgeData &edge_data = m_node_based_graph->GetEdgeData(current_edge);

            // only number incoming edges
            if (edge_data.reversed)
            {
                continue;
            }

            // oneway streets always require this self-loop. Other streets only if a u-turn plus
            // traversal
            // of the street takes longer than the loop
            m_edge_based_node_weights.push_back(edge_data.distance + speed_profile.u_turn_penalty);

            BOOST_ASSERT(numbered_edges_count < m_node_based_graph->GetNumberOfEdges());
            edge_data.edge_id = numbered_edges_count;
            ++numbered_edges_count;

            BOOST_ASSERT(SPECIAL_NODEID != edge_data.edge_id);
        }
    }

    return numbered_edges_count;
}

/// Creates the nodes in the edge expanded graph from edges in the node-based graph.
void EdgeBasedGraphFactory::GenerateEdgeExpandedNodes()
{
    util::Percent progress(m_node_based_graph->GetNumberOfNodes());

    // loop over all edges and generate new set of nodes
    for (const auto node_u : util::irange(0u, m_node_based_graph->GetNumberOfNodes()))
    {
        BOOST_ASSERT(node_u != SPECIAL_NODEID);
        BOOST_ASSERT(node_u < m_node_based_graph->GetNumberOfNodes());
        progress.printStatus(node_u);
        for (EdgeID e1 : m_node_based_graph->GetAdjacentEdgeRange(node_u))
        {
            const EdgeData &edge_data = m_node_based_graph->GetEdgeData(e1);
            BOOST_ASSERT(e1 != SPECIAL_EDGEID);
            const NodeID node_v = m_node_based_graph->GetTarget(e1);

            BOOST_ASSERT(SPECIAL_NODEID != node_v);
            // pick only every other edge, since we have every edge as an outgoing
            // and incoming egde
            if (node_u > node_v)
            {
                continue;
            }

            BOOST_ASSERT(node_u < node_v);

            // if we found a non-forward edge reverse and try again
            if (edge_data.edge_id == SPECIAL_NODEID)
            {
                InsertEdgeBasedNode(node_v, node_u);
            }
            else
            {
                InsertEdgeBasedNode(node_u, node_v);
            }
        }
    }

    BOOST_ASSERT(m_edge_based_node_list.size() == m_edge_based_node_is_startpoint.size());
    BOOST_ASSERT(m_max_edge_id + 1 == m_edge_based_node_weights.size());

    util::SimpleLogger().Write() << "Generated " << m_edge_based_node_list.size()
                                 << " nodes in edge-expanded graph";
}

/// Actually it also generates OriginalEdgeData and serializes them...
void EdgeBasedGraphFactory::GenerateEdgeExpandedEdges(
    const std::string &original_edge_data_filename,
    lua_State *lua_state,
    const std::string &edge_segment_lookup_filename,
    const std::string &edge_fixed_penalties_filename,
    const bool generate_edge_lookup)
{
    util::SimpleLogger().Write() << "generating edge-expanded edges";

    std::size_t node_based_edge_counter = 0;
    std::size_t original_edges_counter = 0;
    restricted_turns_counter = 0;
    skipped_uturns_counter = 0;
    skipped_barrier_turns_counter = 0;

    std::ofstream edge_data_file(original_edge_data_filename.c_str(), std::ios::binary);
    std::ofstream edge_segment_file;
    std::ofstream edge_penalty_file;

    if (generate_edge_lookup)
    {
        edge_segment_file.open(edge_segment_lookup_filename.c_str(), std::ios::binary);
        edge_penalty_file.open(edge_fixed_penalties_filename.c_str(), std::ios::binary);
    }

    // Writes a dummy value at the front that is updated later with the total length
    const unsigned length_prefix_empty_space{0};
    edge_data_file.write(reinterpret_cast<const char *>(&length_prefix_empty_space),
                         sizeof(length_prefix_empty_space));

    std::vector<OriginalEdgeData> original_edge_data_vector;
    original_edge_data_vector.reserve(1024 * 1024);

    // Loop over all turns and generate new set of edges.
    // Three nested loop look super-linear, but we are dealing with a (kind of)
    // linear number of turns only.
    util::Percent progress(m_node_based_graph->GetNumberOfNodes());

    struct CompareTurnPossibilities
    {
        bool operator()(const std::vector<TurnPossibility> &left,
                        const std::vector<TurnPossibility> &right) const
        {
            if (left.size() < right.size())
                return true;
            if (left.size() > right.size())
                return false;
            for (std::size_t i = 0; i < left.size(); ++i)
            {
                if ((((int)left[i].angle + 16) % 256) / 32 <
                    (((int)right[i].angle + 16) % 256) / 32)
                    return true;
                if ((((int)left[i].angle + 16) % 256) / 32 >
                    (((int)right[i].angle + 16) % 256) / 32)
                    return false;
            }
            return false;
        }
    };

// temporary switch to allow display of turn types
#define SHOW_TURN_TYPES 0
#if SHOW_TURN_TYPES
    std::map<std::vector<TurnPossibility>, std::vector<util::FixedPointCoordinate>,
             CompareTurnPossibilities> turn_types;
#endif

    for (const auto node_u : util::irange(0u, m_node_based_graph->GetNumberOfNodes()))
    {
#if SHOW_TURN_TYPES
        auto turn_possibilities = classifyIntersection(
            node_u, *m_node_based_graph, m_compressed_edge_container, m_node_info_list);
        if (turn_possibilities.empty())
            continue;
        auto set = turn_types.find(turn_possibilities);
        if (set != turn_types.end())
        {
            if (set->second.size() < 5)
                set->second.emplace_back(m_node_info_list[node_u].lat,
                                         m_node_info_list[node_u].lon);
        }
        else
        {
            turn_types[turn_possibilities] = std::vector<util::FixedPointCoordinate>(
                1, {m_node_info_list[node_u].lat, m_node_info_list[node_u].lon});
        }
#endif
        // progress.printStatus(node_u);
        for (const EdgeID edge_form_u : m_node_based_graph->GetAdjacentEdgeRange(node_u))
        {
            if (m_node_based_graph->GetEdgeData(edge_form_u).reversed)
            {
                continue;
            }

            ++node_based_edge_counter;
            auto turn_candidates = getTurnCandidates(node_u, edge_form_u);
#define PRINT_DEBUG_CANDIDATES 0
#if PRINT_DEBUG_CANDIDATES
            std::cout << "Initial Candidates:\n";
            for (auto tc : turn_candidates)
                std::cout << "\t" << tc.toString() << " "
                          << (int)m_node_based_graph->GetEdgeData(tc.eid)
                                 .road_classification.road_class
                          << std::endl;
#endif
            turn_candidates = optimizeCandidates(edge_form_u, turn_candidates);
#if PRINT_DEBUG_CANDIDATES
            std::cout << "Optimized Candidates:\n";
            for (auto tc : turn_candidates)
                std::cout << "\t" << tc.toString() << " "
                          << (int)m_node_based_graph->GetEdgeData(tc.eid)
                                 .road_classification.road_class
                          << std::endl;
#endif
            turn_candidates = suppressTurns(edge_form_u, turn_candidates);
#if PRINT_DEBUG_CANDIDATES
            std::cout << "Suppressed Candidates:\n";
            for (auto tc : turn_candidates)
                std::cout << "\t" << tc.toString() << " "
                          << (int)m_node_based_graph->GetEdgeData(tc.eid)
                                 .road_classification.road_class
                          << std::endl;
#endif

            const NodeID node_v = m_node_based_graph->GetTarget(edge_form_u);

            for (const auto turn : turn_candidates)
            {
                if (!turn.valid)
                    continue;

                const double turn_angle = turn.angle;

                // only add an edge if turn is not prohibited
                const EdgeData &edge_data1 = m_node_based_graph->GetEdgeData(edge_form_u);
                const EdgeData &edge_data2 = m_node_based_graph->GetEdgeData(turn.eid);

                BOOST_ASSERT(edge_data1.edge_id != edge_data2.edge_id);
                BOOST_ASSERT(!edge_data1.reversed);
                BOOST_ASSERT(!edge_data2.reversed);

                // the following is the core of the loop.
                unsigned distance = edge_data1.distance;
                if (m_traffic_lights.find(node_v) != m_traffic_lights.end())
                {
                    distance += speed_profile.traffic_signal_penalty;
                }

                const int turn_penalty = GetTurnPenalty(turn_angle, lua_state);
                const auto turn_instruction = turn.instruction;

                if (isUturn(turn_instruction))
                {
                    distance += speed_profile.u_turn_penalty;
                }

                distance += turn_penalty;

                BOOST_ASSERT(m_compressed_edge_container.HasEntryForID(edge_form_u));
                original_edge_data_vector.emplace_back(
                    m_compressed_edge_container.GetPositionForID(edge_form_u), edge_data1.name_id,
                    turn_instruction, edge_data1.travel_mode);

                ++original_edges_counter;

                if (original_edge_data_vector.size() > 1024 * 1024 * 10)
                {
                    FlushVectorToStream(edge_data_file, original_edge_data_vector);
                }

                BOOST_ASSERT(SPECIAL_NODEID != edge_data1.edge_id);
                BOOST_ASSERT(SPECIAL_NODEID != edge_data2.edge_id);

                // NOTE: potential overflow here if we hit 2^32 routable edges
                BOOST_ASSERT(m_edge_based_edge_list.size() <= std::numeric_limits<NodeID>::max());
                m_edge_based_edge_list.emplace_back(edge_data1.edge_id, edge_data2.edge_id,
                                                    m_edge_based_edge_list.size(), distance, true,
                                                    false);

                // Here is where we write out the mapping between the edge-expanded edges, and
                // the node-based edges that are originally used to calculate the `distance`
                // for the edge-expanded edges.  About 40 lines back, there is:
                //
                //                 unsigned distance = edge_data1.distance;
                //
                // This tells us that the weight for an edge-expanded-edge is based on the weight
                // of the *source* node-based edge.  Therefore, we will look up the individual
                // segments of the source node-based edge, and write out a mapping between
                // those and the edge-based-edge ID.
                // External programs can then use this mapping to quickly perform
                // updates to the edge-expanded-edge based directly on its ID.
                if (generate_edge_lookup)
                {
                    unsigned fixed_penalty = distance - edge_data1.distance;
                    edge_penalty_file.write(reinterpret_cast<const char *>(&fixed_penalty),
                                            sizeof(fixed_penalty));
                    const auto node_based_edges =
                        m_compressed_edge_container.GetBucketReference(edge_form_u);
                    NodeID previous = node_u;

                    const unsigned node_count = node_based_edges.size() + 1;
                    edge_segment_file.write(reinterpret_cast<const char *>(&node_count),
                                            sizeof(node_count));
                    const QueryNode &first_node = m_node_info_list[previous];
                    edge_segment_file.write(reinterpret_cast<const char *>(&first_node.node_id),
                                            sizeof(first_node.node_id));

                    for (auto target_node : node_based_edges)
                    {
                        const QueryNode &from = m_node_info_list[previous];
                        const QueryNode &to = m_node_info_list[target_node.node_id];
                        const double segment_length =
                            util::coordinate_calculation::greatCircleDistance(from, to);

                        edge_segment_file.write(reinterpret_cast<const char *>(&to.node_id),
                                                sizeof(to.node_id));
                        edge_segment_file.write(reinterpret_cast<const char *>(&segment_length),
                                                sizeof(segment_length));
                        edge_segment_file.write(reinterpret_cast<const char *>(&target_node.weight),
                                                sizeof(target_node.weight));
                        previous = target_node.node_id;
                    }
                }
            }
        }
    }
#if SHOW_TURN_TYPES
    std::cout << "[info] found " << turn_types.size() << " turn types." << std::endl;
    for (const auto &tt : turn_types)
    {
        std::cout << tt.second.size();
        for (auto coord : tt.second)
            std::cout << " " << coord.lat << " " << coord.lon;

        std::cout << " " << tt.first.size();
        for (auto tte : tt.first)
            std::cout << " " << (int)tte.angle;

        std::cout << std::endl;
    }
#endif

    FlushVectorToStream(edge_data_file, original_edge_data_vector);

    // Finally jump back to the empty space at the beginning and write length prefix
    edge_data_file.seekp(std::ios::beg);

    const auto length_prefix = boost::numeric_cast<unsigned>(original_edges_counter);
    static_assert(sizeof(length_prefix_empty_space) == sizeof(length_prefix), "type mismatch");

    edge_data_file.write(reinterpret_cast<const char *>(&length_prefix), sizeof(length_prefix));

    util::SimpleLogger().Write() << "Generated " << m_edge_based_node_list.size()
                                 << " edge based nodes";
    util::SimpleLogger().Write() << "Node-based graph contains " << node_based_edge_counter
                                 << " edges";
    util::SimpleLogger().Write() << "Edge-expanded graph ...";
    util::SimpleLogger().Write() << "  contains " << m_edge_based_edge_list.size() << " edges";
    util::SimpleLogger().Write() << "  skips " << restricted_turns_counter << " turns, "
                                                                              "defined by "
                                 << m_restriction_map->size() << " restrictions";
    util::SimpleLogger().Write() << "  skips " << skipped_uturns_counter << " U turns";
    util::SimpleLogger().Write() << "  skips " << skipped_barrier_turns_counter
                                 << " turns over barriers";
}

std::vector<EdgeBasedGraphFactory::TurnCandidate>
EdgeBasedGraphFactory::optimizeRamps(const EdgeID via_edge,
                                     std::vector<TurnCandidate> turn_candidates) const
{
    EdgeID continue_eid = SPECIAL_EDGEID;
    double continue_angle = 0;
    const auto &in_edge_data = m_node_based_graph->GetEdgeData(via_edge);
    for (auto &candidate : turn_candidates)
    {
        if (candidate.instruction.direction_modifier == DirectionModifier::UTurn)
            continue;

        const auto &out_edge_data = m_node_based_graph->GetEdgeData(candidate.eid);
        if (out_edge_data.name_id == in_edge_data.name_id)
        {
            continue_eid = candidate.eid;
            continue_angle = candidate.angle;
            if (angularDeviation(candidate.angle, STRAIGHT_ANGLE) < NARROW_TURN_ANGLE &&
                isRampClass(in_edge_data.road_classification.road_class))
                candidate.instruction = TurnType::Suppressed;
            break;
        }
    }

    if (continue_eid != SPECIAL_EDGEID)
    {
        bool to_the_right = true;
        for (auto &candidate : turn_candidates)
        {
            if (candidate.eid == continue_eid)
            {
                to_the_right = false;
                continue;
            }

            if (candidate.instruction.type != TurnType::Ramp)
                continue;

            if (isSlightModifier(candidate.instruction.direction_modifier))
                candidate.instruction.direction_modifier =
                    (to_the_right) ? DirectionModifier::SlightRight : DirectionModifier::SlightLeft;
        }
    }
    return turn_candidates;
}

TurnType
EdgeBasedGraphFactory::checkForkAndEnd(const EdgeID via_eid,
                                       const std::vector<TurnCandidate> &turn_candidates) const
{
    if (turn_candidates.size() != 3 ||
        turn_candidates.front().instruction.direction_modifier != DirectionModifier::UTurn)
        return TurnType::Invalid;

    if (isOnRoundabout(turn_candidates[1].instruction))
    {
        BOOST_ASSERT(isOnRoundabout(turn_candidates[2].instruction));
        return TurnType::Invalid;
    }
    BOOST_ASSERT(!isOnRoundabout(turn_candidates[2].instruction));

    FunctionalRoadClass road_classes[3] = {
        m_node_based_graph->GetEdgeData(via_eid).road_classification.road_class,
        m_node_based_graph->GetEdgeData(turn_candidates[1].eid).road_classification.road_class,
        m_node_based_graph->GetEdgeData(turn_candidates[2].eid).road_classification.road_class};

    if (angularDeviation(turn_candidates[1].angle, STRAIGHT_ANGLE) < NARROW_TURN_ANGLE &&
        angularDeviation(turn_candidates[2].angle, STRAIGHT_ANGLE) < NARROW_TURN_ANGLE)
    {
        if (road_classes[0] != road_classes[1] || road_classes[1] != road_classes[2])
            return TurnType::Invalid;

        if (turn_candidates[1].valid && turn_candidates[2].valid)
            return TurnType::Fork;
    }

    else if (angularDeviation(turn_candidates[1].angle, 90) < NARROW_TURN_ANGLE &&
             angularDeviation(turn_candidates[2].angle, 270) < NARROW_TURN_ANGLE)
    {
        return TurnType::EndOfRoad;
    }

    return TurnType::Invalid;
}

std::vector<EdgeBasedGraphFactory::TurnCandidate>
EdgeBasedGraphFactory::handleForkAndEnd(const TurnType type,
                                        std::vector<TurnCandidate> turn_candidates) const
{
    turn_candidates[1].instruction.type = type;
    turn_candidates[1].instruction.direction_modifier =
        (type == TurnType::Fork) ? DirectionModifier::SlightRight : DirectionModifier::Right;
    turn_candidates[2].instruction.type = type;
    turn_candidates[2].instruction.direction_modifier =
        (type == TurnType::Fork) ? DirectionModifier::SlightLeft : DirectionModifier::Left;
    return turn_candidates;
}

// requires sorted candidates
std::vector<EdgeBasedGraphFactory::TurnCandidate>
EdgeBasedGraphFactory::optimizeCandidates(const EdgeID via_eid,
                                          std::vector<TurnCandidate> turn_candidates) const
{
    BOOST_ASSERT_MSG(std::is_sorted(turn_candidates.begin(), turn_candidates.end(),
                                    [](const TurnCandidate &left, const TurnCandidate &right)
                                    {
                                        return left.angle < right.angle;
                                    }),
                     "Turn Candidates not sorted by angle.");
    if (turn_candidates.size() <= 1)
        return turn_candidates;

    TurnType type = checkForkAndEnd(via_eid, turn_candidates);
    if (type != TurnType::Invalid)
        return handleForkAndEnd(type, std::move(turn_candidates));

    turn_candidates = optimizeRamps(via_eid, std::move(turn_candidates));

    const auto getLeft = [&turn_candidates](std::size_t index)
    {
        return (index + 1) % turn_candidates.size();
    };
    const auto getRight = [&turn_candidates](std::size_t index)
    {
        return (index + turn_candidates.size() - 1) % turn_candidates.size();
    };

    // handle availability of multiple u-turns (e.g. street with separated small parking roads)
    if (isUturn(turn_candidates[0].instruction) && turn_candidates[0].angle == 0)
    {
        if (isUturn(turn_candidates[getLeft(0)].instruction))
            turn_candidates[getLeft(0)].instruction.direction_modifier =
                DirectionModifier::SharpLeft;
        if (isUturn(turn_candidates[getRight(0)].instruction))
            turn_candidates[getRight(0)].instruction.direction_modifier =
                DirectionModifier::SharpRight;
    }

    const auto keepStraight = [](double angle)
    {
        return std::abs(angle - 180) < 5;
    };

    for (std::size_t turn_index = 0; turn_index < turn_candidates.size(); ++turn_index)
    {
        auto &turn = turn_candidates[turn_index];
        if (!isBasic(turn.instruction.type) || isUturn(turn.instruction) ||
            isOnRoundabout(turn.instruction))
            continue;
        auto &left = turn_candidates[getLeft(turn_index)];
        if (turn.angle == left.angle)
        {
            util::SimpleLogger().Write(logDEBUG)
                << "[warning] conflicting turn angles, identical road duplicated? "
                << m_node_info_list[m_node_based_graph->GetTarget(via_eid)].lat << " "
                << m_node_info_list[m_node_based_graph->GetTarget(via_eid)].lon << std::endl;
        }
        if (isConflict(turn.instruction, left.instruction))
        {
            // begin of a conflicting region
            std::size_t conflict_begin = turn_index;
            std::size_t conflict_end = getLeft(turn_index);
            std::size_t conflict_size = 2;
            while (
                isConflict(turn_candidates[getLeft(conflict_end)].instruction, turn.instruction) &&
                conflict_size < turn_candidates.size())
            {
                conflict_end = getLeft(conflict_end);
                ++conflict_size;
            }

            turn_index = (conflict_end < conflict_begin) ? turn_candidates.size() : conflict_end;

            if (conflict_size > 3)
            {
                // check if some turns are invalid to find out about good handling
            }

            auto &instruction_left_of_end = turn_candidates[getLeft(conflict_end)].instruction;
            auto &instruction_right_of_begin =
                turn_candidates[getRight(conflict_begin)].instruction;
            auto &candidate_at_end = turn_candidates[conflict_end];
            auto &candidate_at_begin = turn_candidates[conflict_begin];
            if (conflict_size == 2)
            {
                if (turn.instruction.direction_modifier == DirectionModifier::Straight)
                {
                    if (instruction_left_of_end.direction_modifier !=
                            DirectionModifier::SlightLeft &&
                        instruction_right_of_begin.direction_modifier !=
                            DirectionModifier::SlightRight)
                    {
                        std::int32_t resolved_count = 0;
                        // uses side-effects in resolve
                        if (!keepStraight(candidate_at_end.angle) &&
                            !resolve(candidate_at_end.instruction, instruction_left_of_end,
                                     RESOLVE_TO_LEFT))
                            util::SimpleLogger().Write(logDEBUG)
                                << "[warning] failed to resolve conflict";
                        else
                            ++resolved_count;
                        // uses side-effects in resolve
                        if (!keepStraight(candidate_at_begin.angle) &&
                            !resolve(candidate_at_begin.instruction, instruction_right_of_begin,
                                     RESOLVE_TO_RIGHT))
                            util::SimpleLogger().Write(logDEBUG)
                                << "[warning] failed to resolve conflict";
                        else
                            ++resolved_count;
                        if (resolved_count >= 1 &&
                            (!keepStraight(candidate_at_begin.angle) ||
                             !keepStraight(candidate_at_end.angle))) // should always be the
                                                                     // case, theoretically
                            continue;
                    }
                }
                if (candidate_at_begin.confidence < candidate_at_end.confidence)
                { // if right shift is cheaper, or only option
                    if (resolve(candidate_at_begin.instruction, instruction_right_of_begin,
                                RESOLVE_TO_RIGHT))
                        continue;
                    else if (resolve(candidate_at_end.instruction, instruction_left_of_end,
                                     RESOLVE_TO_LEFT))
                        continue;
                }
                else
                {
                    if (resolve(candidate_at_end.instruction, instruction_left_of_end,
                                RESOLVE_TO_LEFT))
                        continue;
                    else if (resolve(candidate_at_begin.instruction, instruction_right_of_begin,
                                     RESOLVE_TO_RIGHT))
                        continue;
                }
                if (isSlightTurn(turn.instruction) || isSharpTurn(turn.instruction))
                {
                    auto resolve_direction =
                        (turn.instruction.direction_modifier == DirectionModifier::SlightRight ||
                         turn.instruction.direction_modifier == DirectionModifier::SharpLeft)
                            ? RESOLVE_TO_RIGHT
                            : RESOLVE_TO_LEFT;
                    if (resolve_direction == RESOLVE_TO_RIGHT &&
                        resolveTransitive(
                            candidate_at_begin.instruction, instruction_right_of_begin,
                            turn_candidates[getRight(getRight(conflict_begin))].instruction,
                            RESOLVE_TO_RIGHT))
                        continue;
                    else if (resolve_direction == RESOLVE_TO_LEFT &&
                             resolveTransitive(
                                 candidate_at_end.instruction, instruction_left_of_end,
                                 turn_candidates[getLeft(getLeft(conflict_end))].instruction,
                                 RESOLVE_TO_LEFT))
                        continue;
                }
            }
            else if (conflict_size >= 3)
            {
                // a conflict of size larger than three cannot be handled with the current
                // model.
                // Handle it as best as possible and keep the rest of the conflicting turns
                if (conflict_size > 3)
                {
                    NodeID conflict_location = m_node_based_graph->GetTarget(via_eid);
                    util::SimpleLogger().Write(logDEBUG)
                        << "[warning] found conflict larget than size three at "
                        << m_node_info_list[conflict_location].lat << ", "
                        << m_node_info_list[conflict_location].lon;
                }

                if (!resolve(candidate_at_begin.instruction, instruction_right_of_begin,
                             RESOLVE_TO_RIGHT))
                {
                    if (isSlightTurn(turn.instruction))
                        resolveTransitive(
                            candidate_at_begin.instruction, instruction_right_of_begin,
                            turn_candidates[getRight(getRight(conflict_begin))].instruction,
                            RESOLVE_TO_RIGHT);
                    else if (isSharpTurn(turn.instruction))
                        resolveTransitive(
                            candidate_at_end.instruction, instruction_left_of_end,
                            turn_candidates[getLeft(getLeft(conflict_end))].instruction,
                            RESOLVE_TO_LEFT);
                }
                if (!resolve(candidate_at_end.instruction, instruction_left_of_end,
                             RESOLVE_TO_LEFT))
                {
                    if (isSlightTurn(turn.instruction))
                        resolveTransitive(
                            candidate_at_end.instruction, instruction_left_of_end,
                            turn_candidates[getLeft(getLeft(conflict_end))].instruction,
                            RESOLVE_TO_LEFT);
                    else if (isSharpTurn(turn.instruction))
                        resolveTransitive(
                            candidate_at_begin.instruction, instruction_right_of_begin,
                            turn_candidates[getRight(getRight(conflict_begin))].instruction,
                            RESOLVE_TO_RIGHT);
                }
            }
        }
    }
    return turn_candidates;
}

bool EdgeBasedGraphFactory::isObviousChoice(const EdgeID via_eid,
                                            const std::size_t turn_index,
                                            const std::vector<TurnCandidate> &turn_candidates) const
{
    const auto getLeft = [&turn_candidates](std::size_t index)
    {
        return (index + 1) % turn_candidates.size();
    };
    const auto getRight = [&turn_candidates](std::size_t index)
    {
        return (index + turn_candidates.size() - 1) % turn_candidates.size();
    };
    const auto &candidate = turn_candidates[turn_index];
    const EdgeData &in_data = m_node_based_graph->GetEdgeData(via_eid);
    const EdgeData &out_data = m_node_based_graph->GetEdgeData(candidate.eid);
    const auto &candidate_to_the_left = turn_candidates[getLeft(turn_index)];

    const auto &candidate_to_the_right = turn_candidates[getRight(turn_index)];

    const auto hasValidRatio = [this](const TurnCandidate &left, const TurnCandidate &center,
                                      const TurnCandidate &right)
    {
        auto angle_left = (left.angle > 180) ? angularDeviation(left.angle, STRAIGHT_ANGLE) : 180;
        auto angle_right =
            (right.angle < 180) ? angularDeviation(right.angle, STRAIGHT_ANGLE) : 180;
        auto self_angle = angularDeviation(center.angle, STRAIGHT_ANGLE);
        return angularDeviation(center.angle, STRAIGHT_ANGLE) < NARROW_TURN_ANGLE &&
               ((center.angle < STRAIGHT_ANGLE)
                    ? (angle_right > self_angle && angle_left / self_angle > DISTINCTION_RATIO)
                    : (angle_left > self_angle && angle_right / self_angle > DISTINCTION_RATIO));
    };
    // only valid turn
    if (!isLowPriorityRoadClass(
            m_node_based_graph->GetEdgeData(candidate.eid).road_classification.road_class))
    {
        bool is_only_normal_road = true;
        BOOST_ASSERT(turn_candidates[0].instruction.type == TurnType::Turn &&
                     turn_candidates[0].instruction.direction_modifier == DirectionModifier::UTurn);
        for (size_t i = 0; i < turn_candidates.size(); ++i)
        {
            if (i == turn_index || turn_candidates[i].angle == 0) // skip self and u-turn
                continue;
            if (!isLowPriorityRoadClass(m_node_based_graph->GetEdgeData(turn_candidates[i].eid)
                                            .road_classification.road_class))
            {
                is_only_normal_road = false;
                break;
            }
        }
        if (is_only_normal_road == true)
            return true;
    }

    return turn_candidates.size() == 1 ||
           // only non u-turn
           (turn_candidates.size() == 2 &&
            isUturn(candidate_to_the_left.instruction)) || // nearly straight turn
           angularDeviation(candidate.angle, STRAIGHT_ANGLE) < MAXIMAL_ALLOWED_NO_TURN_DEVIATION ||
           hasValidRatio(candidate_to_the_left, candidate, candidate_to_the_right) ||
           (in_data.name_id != 0 && in_data.name_id == out_data.name_id &&
            angularDeviation(candidate.angle, STRAIGHT_ANGLE) < NARROW_TURN_ANGLE / 2);
}

std::vector<EdgeBasedGraphFactory::TurnCandidate>
EdgeBasedGraphFactory::suppressTurns(const EdgeID via_eid,
                                     std::vector<TurnCandidate> turn_candidates) const
{
    if (turn_candidates.size() == 3)
    {
        BOOST_ASSERT(turn_candidates[0].instruction.direction_modifier == DirectionModifier::UTurn);
        if (isLowPriorityRoadClass(m_node_based_graph->GetEdgeData(turn_candidates[1].eid)
                                       .road_classification.road_class) &&
            !isLowPriorityRoadClass(m_node_based_graph->GetEdgeData(turn_candidates[2].eid)
                                        .road_classification.road_class))
        {
            if (angularDeviation(turn_candidates[2].angle, STRAIGHT_ANGLE) < NARROW_TURN_ANGLE)
            {
                if (m_node_based_graph->GetEdgeData(turn_candidates[2].eid).name_id ==
                    m_node_based_graph->GetEdgeData(via_eid).name_id)
                {
                    turn_candidates[2].instruction = TurnInstruction::NO_TURN();
                }
                else
                {
                    turn_candidates[2].instruction.type = TurnType::NewName;
                }
                return turn_candidates;
            }
        }
        else if (isLowPriorityRoadClass(m_node_based_graph->GetEdgeData(turn_candidates[2].eid)
                                            .road_classification.road_class) &&
                 !isLowPriorityRoadClass(m_node_based_graph->GetEdgeData(turn_candidates[1].eid)
                                             .road_classification.road_class))
        {
            if (angularDeviation(turn_candidates[1].angle, STRAIGHT_ANGLE) < NARROW_TURN_ANGLE)
            {
                if (m_node_based_graph->GetEdgeData(turn_candidates[1].eid).name_id ==
                    m_node_based_graph->GetEdgeData(via_eid).name_id)
                {
                    turn_candidates[1].instruction = TurnInstruction::NO_TURN();
                }
                else
                {
                    turn_candidates[1].instruction.type = TurnType::NewName;
                }
                return turn_candidates;
            }
        }
    }

    BOOST_ASSERT_MSG(std::is_sorted(turn_candidates.begin(), turn_candidates.end(),
                                    [](const TurnCandidate &left, const TurnCandidate &right)
                                    {
                                        return left.angle < right.angle;
                                    }),
                     "Turn Candidates not sorted by angle.");

    const auto getLeft = [&turn_candidates](std::size_t index)
    {
        return (index + 1) % turn_candidates.size();
    };
    const auto getRight = [&turn_candidates](std::size_t index)
    {
        return (index + turn_candidates.size() - 1) % turn_candidates.size();
    };

    const EdgeData &in_data = m_node_based_graph->GetEdgeData(via_eid);

    bool has_obvious_with_same_name = false;
    double obvious_with_same_name_angle = 0;
    for (std::size_t turn_index = 0; turn_index < turn_candidates.size(); ++turn_index)
    {
        if (m_node_based_graph->GetEdgeData(turn_candidates[turn_index].eid).name_id ==
                in_data.name_id &&
            isObviousChoice(via_eid, turn_index, turn_candidates))
        {
            has_obvious_with_same_name = true;
            obvious_with_same_name_angle = turn_candidates[turn_index].angle;
            break;
        }
    }

    for (std::size_t turn_index = 0; turn_index < turn_candidates.size(); ++turn_index)
    {
        auto &candidate = turn_candidates[turn_index];
        if (!isBasic(candidate.instruction.type))
            continue;

        const EdgeData &out_data = m_node_based_graph->GetEdgeData(candidate.eid);
        if (out_data.name_id == in_data.name_id && in_data.name_id != 0 &&
            candidate.instruction.direction_modifier != DirectionModifier::UTurn &&
            !has_obvious_with_same_name)
        {
            candidate.instruction.type = TurnType::Continue;
        }
        if (candidate.valid && !isUturn(candidate.instruction))
        {
            // TODO road category would be useful to indicate obviousness of turn
            // check if turn can be omitted or at least changed
            const auto &left = turn_candidates[getLeft(turn_index)];
            const auto &right = turn_candidates[getRight(turn_index)];

            // make very slight instructions straight, if they are the only valid choice going
            // with
            // at most a slight turn
            if ((!isSlightModifier(getTurnDirection(left.angle)) || !left.valid) &&
                (!isSlightModifier(getTurnDirection(right.angle)) || !right.valid) &&
                angularDeviation(candidate.angle, STRAIGHT_ANGLE) < FUZZY_STRAIGHT_ANGLE)
                candidate.instruction.direction_modifier = DirectionModifier::Straight;

            // TODO this smaller comparison for turns is DANGEROUS, has to be revised if turn
            // instructions change
            if (in_data.travel_mode ==
                out_data.travel_mode) // make sure to always announce mode changes
            {
                if (isObviousChoice(via_eid, turn_index, turn_candidates))
                {

                    if (in_data.name_id == out_data.name_id) // same road
                    {
                        candidate.instruction.type = TurnType::Suppressed;
                    }

                    else if (!has_obvious_with_same_name)
                    {
                        // TODO discuss, we might want to keep the current name of the turn. But
                        // this would mean emitting a turn when you just keep on a road
                        if (isRampClass(in_data.road_classification.road_class) &&
                            !isRampClass(out_data.road_classification.road_class))
                        {
                            candidate.instruction.type = TurnType::Merge;
                            candidate.instruction.direction_modifier =
                                mirrorDirectionModifier(candidate.instruction.direction_modifier);
                        }
                        else
                        {
                            if (engine::guidance::canBeSuppressed(candidate.instruction.type))
                                candidate.instruction.type = TurnType::NewName;
                        }
                    }
                    else if (candidate.angle < obvious_with_same_name_angle)
                        candidate.instruction.direction_modifier = DirectionModifier::SlightRight;
                    else
                        candidate.instruction.direction_modifier = DirectionModifier::SlightLeft;
                }
                else if (candidate.instruction.direction_modifier == DirectionModifier::Straight &&
                         has_obvious_with_same_name)
                {
                    if (candidate.angle < obvious_with_same_name_angle)
                        candidate.instruction.direction_modifier = DirectionModifier::SlightRight;
                    else
                        candidate.instruction.direction_modifier = DirectionModifier::SlightLeft;
                }
            }
        }
    }
    return turn_candidates;
}

std::vector<EdgeBasedGraphFactory::TurnCandidate>
EdgeBasedGraphFactory::getTurnCandidates(const NodeID from_node, const EdgeID via_eid)
{
    std::vector<TurnCandidate> turn_candidates;
    const NodeID turn_node = m_node_based_graph->GetTarget(via_eid);
    const NodeID only_restriction_to_node =
        m_restriction_map->CheckForEmanatingIsOnlyTurn(from_node, turn_node);
    const bool is_barrier_node = m_barrier_nodes.find(turn_node) != m_barrier_nodes.end();

    bool has_non_roundabout = false, has_roundabout_entry;
    for (const EdgeID onto_edge : m_node_based_graph->GetAdjacentEdgeRange(turn_node))
    {
        bool turn_is_valid = true;
        if (m_node_based_graph->GetEdgeData(onto_edge).reversed)
        {
            turn_is_valid = false;
        }
        const NodeID to_node = m_node_based_graph->GetTarget(onto_edge);

        if (turn_is_valid && (only_restriction_to_node != SPECIAL_NODEID) &&
            (to_node != only_restriction_to_node))
        {
            // We are at an only_-restriction but not at the right turn.
            ++restricted_turns_counter;
            turn_is_valid = false;
        }

        if (turn_is_valid)
        {
            if (is_barrier_node)
            {
                if (from_node != to_node)
                {
                    ++skipped_barrier_turns_counter;
                    turn_is_valid = false;
                }
            }
            else
            {
                if (from_node == to_node && m_node_based_graph->GetOutDegree(turn_node) > 1)
                {
                    auto number_of_emmiting_bidirectional_edges = 0;
                    for (auto edge : m_node_based_graph->GetAdjacentEdgeRange(turn_node))
                    {
                        auto target = m_node_based_graph->GetTarget(edge);
                        auto reverse_edge = m_node_based_graph->FindEdge(target, turn_node);
                        if (!m_node_based_graph->GetEdgeData(reverse_edge).reversed)
                        {
                            ++number_of_emmiting_bidirectional_edges;
                        }
                    }
                    if (number_of_emmiting_bidirectional_edges > 1)
                    {
                        ++skipped_uturns_counter;
                        turn_is_valid = false;
                    }
                }
            }
        }

        // only add an edge if turn is not a U-turn except when it is
        // at the end of a dead-end street
        if (m_restriction_map->CheckIfTurnIsRestricted(from_node, turn_node, to_node) &&
            (only_restriction_to_node == SPECIAL_NODEID) && (to_node != only_restriction_to_node))
        {
            // We are at an only_-restriction but not at the right turn.
            ++restricted_turns_counter;
            turn_is_valid = false;
        }

        // unpack first node of second segment if packed

        const auto first_coordinate = getRepresentativeCoordinate(
            from_node, turn_node, via_eid, INVERT, m_compressed_edge_container, m_node_info_list);
        const auto third_coordinate = getRepresentativeCoordinate(
            turn_node, to_node, onto_edge, !INVERT, m_compressed_edge_container, m_node_info_list);

        const auto angle = util::coordinate_calculation::computeAngle(
            first_coordinate, m_node_info_list[turn_node], third_coordinate);

        const auto turn = AnalyzeTurn(from_node, via_eid, turn_node, onto_edge, to_node, angle);

        if (turn_is_valid && !entersRoundabout(turn))
            has_non_roundabout = true;
        else if (turn_is_valid)
            has_roundabout_entry = true;

        auto confidence = getTurnConfidence(angle, turn);
        if (!turn_is_valid)
            confidence *= 0.8; // makes invalid turns more likely to be resolved in conflicts

        turn_candidates.push_back({onto_edge, turn_is_valid, angle, turn, confidence});
    }

    if (has_non_roundabout && has_roundabout_entry)
    {
        for (auto &candidate : turn_candidates)
        {
            if (entersRoundabout(candidate.instruction))
            {
                if (candidate.instruction.type == TurnType::EnterRotary)
                    candidate.instruction.type = TurnType::EnterRotaryAtExit;
                if (candidate.instruction.type == TurnType::EnterRoundabout)
                    candidate.instruction.type = TurnType::EnterRoundaboutAtExit;
            }
        }
    }

    const auto ByAngle = [](const TurnCandidate &first, const TurnCandidate second)
    {
        return first.angle < second.angle;
    };
    std::sort(std::begin(turn_candidates), std::end(turn_candidates), ByAngle);

    const auto getLeft = [&](std::size_t index)
    {
        return (index + 1) % turn_candidates.size();
    };

    const auto getRight = [&](std::size_t index)
    {
        return (index + turn_candidates.size() - 1) % turn_candidates.size();
    };

    const auto isInvalidEquivalent = [&](std::size_t this_turn, std::size_t valid_turn)
    {
        if (!turn_candidates[valid_turn].valid || turn_candidates[this_turn].valid)
            return false;

        return angularDeviation(turn_candidates[this_turn].angle,
                                turn_candidates[valid_turn].angle) < NARROW_TURN_ANGLE;
    };

    for (std::size_t index = 0; index < turn_candidates.size(); ++index)
    {
        if (isInvalidEquivalent(index, getRight(index)) ||
            isInvalidEquivalent(index, getLeft(index)))
        {
            turn_candidates.erase(turn_candidates.begin() + index);
            --index;
        }
    }
    return turn_candidates;
}

int EdgeBasedGraphFactory::GetTurnPenalty(double angle, lua_State *lua_state) const
{

    if (speed_profile.has_turn_penalty_function)
    {
        try
        {
            // call lua profile to compute turn penalty
            double penalty =
                luabind::call_function<double>(lua_state, "turn_function", 180. - angle);
            return static_cast<int>(penalty);
        }
        catch (const luabind::error &er)
        {
            util::SimpleLogger().Write(logWARNING) << er.what();
        }
    }
    return 0;
}

// node_u -- (edge_1) --> node_v -- (edge_2) --> node_w
TurnInstruction EdgeBasedGraphFactory::AnalyzeTurn(const NodeID node_u,
                                                   const EdgeID edge1,
                                                   const NodeID node_v,
                                                   const EdgeID edge2,
                                                   const NodeID node_w,
                                                   const double angle) const
{

    const EdgeData &data1 = m_node_based_graph->GetEdgeData(edge1);
    const EdgeData &data2 = m_node_based_graph->GetEdgeData(edge2);
    bool from_ramp = isRampClass(data1.road_classification.road_class);
    bool to_ramp = isRampClass(data2.road_classification.road_class);
    if (node_u == node_w)
    {
        return {TurnType::Turn, DirectionModifier::UTurn};
    }

    // roundabouts need to be handled explicitely
    if (data1.roundabout && data2.roundabout)
    {
        // Is a turn possible? If yes, we stay on the roundabout!
        if (1 == m_node_based_graph->GetDirectedOutDegree(node_v))
        {
            // No turn possible.
            return TurnInstruction::NO_TURN();
        }
        return TurnInstruction::REMAIN_ROUNDABOUT(getTurnDirection(angle));
    }
    // Does turn start or end on roundabout?
    if (data1.roundabout || data2.roundabout)
    {
        // We are entering the roundabout
        if ((!data1.roundabout) && data2.roundabout)
        {
            return TurnInstruction::ENTER_ROUNDABOUT(getTurnDirection(angle));
        }
        // We are leaving the roundabout
        if (data1.roundabout && (!data2.roundabout))
        {
            return TurnInstruction::EXIT_ROUNDABOUT(getTurnDirection(angle));
        }
    }

    if (!from_ramp && to_ramp)
    {
        return {TurnType::Ramp, getTurnDirection(angle)};
    }

    // assign a designated turn angle instruction purely based on the angle
    return {TurnType::Turn, getTurnDirection(angle)};
}

} // namespace extractor
} // namespace osrm
