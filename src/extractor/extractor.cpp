#include "extractor/extractor.hpp"

#include "extractor/edge_based_edge.hpp"
#include "extractor/extraction_containers.hpp"
#include "extractor/extraction_node.hpp"
#include "extractor/extraction_relation.hpp"
#include "extractor/extraction_way.hpp"
#include "extractor/extractor_callbacks.hpp"
#include "extractor/files.hpp"
#include "extractor/node_based_graph_factory.hpp"
#include "extractor/raster_source.hpp"
#include "extractor/restriction_filter.hpp"
#include "extractor/restriction_parser.hpp"
#include "extractor/scripting_environment.hpp"

#include "storage/io.hpp"

#include "util/exception.hpp"
#include "util/exception_utils.hpp"
#include "util/graph_loader.hpp"
#include "util/integer_range.hpp"
#include "util/log.hpp"
#include "util/name_table.hpp"
#include "util/range_table.hpp"
#include "util/timing_util.hpp"

#include "extractor/compressed_edge_container.hpp"
#include "extractor/restriction_index.hpp"
#include "extractor/way_restriction_map.hpp"
#include "util/static_graph.hpp"
#include "util/static_rtree.hpp"

// Keep debug include to make sure the debug header is in sync with types.
#include "util/debug.hpp"

#include "extractor/tarjan_scc.hpp"

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/iterator/function_input_iterator.hpp>
#include <boost/optional/optional.hpp>
#include <boost/scope_exit.hpp>

#include <osmium/handler/node_locations_for_ways.hpp>
#include <osmium/index/map/flex_mem.hpp>
#include <osmium/io/any_input.hpp>
#include <osmium/thread/pool.hpp>
#include <osmium/visitor.hpp>

#include <tbb/pipeline.h>
#include <tbb/task_scheduler_init.h>

#include <cstdlib>

#include <algorithm>
#include <atomic>
#include <bitset>
#include <chrono>
#include <future>
#include <iostream>
#include <iterator>
#include <memory>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace osrm
{
namespace extractor
{

namespace
{
// Converts the class name map into a fixed mapping of index to name
void SetClassNames(const std::vector<std::string> &class_names,
                   ExtractorCallbacks::ClassesMap &classes_map,
                   ProfileProperties &profile_properties)
{
    // if we get a list of class names we can validate if we set invalid classes
    // and add classes that were never reference
    if (!class_names.empty())
    {
        // add class names that were never used explicitly on a way
        // this makes sure we can correctly validate unkown class names later
        for (const auto &name : class_names)
        {
            if (!isValidClassName(name))
            {
                throw util::exception("Invalid class name " + name + " only [a-Z0-9] allowed.");
            }

            auto iter = classes_map.find(name);
            if (iter == classes_map.end())
            {
                auto index = classes_map.size();
                if (index > MAX_CLASS_INDEX)
                {
                    throw util::exception("Maximum number of classes if " +
                                          std::to_string(MAX_CLASS_INDEX + 1));
                }

                classes_map[name] = getClassData(index);
            }
        }

        // check if class names are only from the list supplied by the user
        for (const auto &pair : classes_map)
        {
            auto iter = std::find(class_names.begin(), class_names.end(), pair.first);
            if (iter == class_names.end())
            {
                throw util::exception("Profile used unknown class name: " + pair.first);
            }
        }
    }

    for (const auto &pair : classes_map)
    {
        auto range = getClassIndexes(pair.second);
        BOOST_ASSERT(range.size() == 1);
        profile_properties.SetClassName(range.front(), pair.first);
    }
}

// Converts the class name list to a mask list
void SetExcludableClasses(const ExtractorCallbacks::ClassesMap &classes_map,
                          const std::vector<std::vector<std::string>> &excludable_classes,
                          ProfileProperties &profile_properties)
{
    if (excludable_classes.size() > MAX_EXCLUDABLE_CLASSES)
    {
        throw util::exception("Only " + std::to_string(MAX_EXCLUDABLE_CLASSES) +
                              " excludable combinations allowed.");
    }

    // The exclude index 0 is reserve for not excludeing anything
    profile_properties.SetExcludableClasses(0, 0);

    std::size_t combination_index = 1;
    for (const auto &combination : excludable_classes)
    {
        ClassData mask = 0;
        for (const auto &name : combination)
        {
            auto iter = classes_map.find(name);
            if (iter == classes_map.end())
            {
                util::Log(logWARNING)
                    << "Unknown class name " + name + " in excludable combination. Ignoring.";
            }
            else
            {
                mask |= iter->second;
            }
        }

        if (mask > 0)
        {
            profile_properties.SetExcludableClasses(combination_index++, mask);
        }
    }
}
}

/**
 * TODO: Refactor this function into smaller functions for better readability.
 *
 * This function is the entry point for the whole extraction process. The goal of the extraction
 * step is to filter and convert the OSM geometry to something more fitting for routing.
 * That includes:
 *  - extracting turn restrictions
 *  - splitting ways into (directional!) edge segments
 *  - checking if nodes are barriers or traffic signal
 *  - discarding all tag information: All relevant type information for nodes/ways
 *    is extracted at this point.
 *
 * The result of this process are the following files:
 *  .names : Names of all streets, stored as long consecutive string with prefix sum based index
 *  .osrm  : Nodes and edges in a intermediate format that easy to digest for osrm-contract
 *  .restrictions : Turn restrictions that are used by osrm-contract to construct the edge-expanded
 * graph
 *
 */
int Extractor::run(ScriptingEnvironment &scripting_environment)
{
    util::LogPolicy::GetInstance().Unmute();

    const unsigned recommended_num_threads = tbb::task_scheduler_init::default_num_threads();
    const auto number_of_threads = std::min(recommended_num_threads, config.requested_num_threads);
    tbb::task_scheduler_init init(number_of_threads ? number_of_threads
                                                    : tbb::task_scheduler_init::automatic);

    guidance::LaneDescriptionMap turn_lane_map;
    std::vector<TurnRestriction> turn_restrictions;
    std::vector<ConditionalTurnRestriction> conditional_turn_restrictions;
    std::tie(turn_lane_map, turn_restrictions, conditional_turn_restrictions) =
        ParseOSMData(scripting_environment, number_of_threads);

    // Transform the node-based graph that OSM is based on into an edge-based graph
    // that is better for routing.  Every edge becomes a node, and every valid
    // movement (e.g. turn from A->B, and B->A) becomes an edge
    util::Log() << "Generating edge-expanded graph representation";

    TIMER_START(expansion);

    EdgeBasedNodeDataContainer edge_based_nodes_container;
    std::vector<EdgeBasedNodeSegment> edge_based_node_segments;
    util::DeallocatingVector<EdgeBasedEdge> edge_based_edge_list;
    std::vector<bool> node_is_startpoint;
    std::vector<EdgeWeight> edge_based_node_weights;

    // Create a node-based graph from the OSRM file
    NodeBasedGraphFactory node_based_graph_factory(config.GetPath(".osrm"),
                                                   scripting_environment,
                                                   turn_restrictions,
                                                   conditional_turn_restrictions);

    util::Log() << "Find segregated edges in node-based graph ..." << std::flush;
    TIMER_START(segregated);

    auto segregated_edges = FindSegregatedNodes(node_based_graph_factory);

    TIMER_STOP(segregated);
    util::Log() << "ok, after " << TIMER_SEC(segregated) << "s";
    util::Log() << "Segregated edges count = " << segregated_edges.size();

    util::Log() << "Writing nodes for nodes-based and edges-based graphs ...";
    auto const &coordinates = node_based_graph_factory.GetCoordinates();
    files::writeNodes(
        config.GetPath(".osrm.nbg_nodes"), coordinates, node_based_graph_factory.GetOsmNodes());
    node_based_graph_factory.ReleaseOsmNodes();

    auto const &node_based_graph = node_based_graph_factory.GetGraph();

    // The osrm-partition tool requires the compressed node based graph with an embedding.
    //
    // The `Run` function above re-numbers non-reverse compressed node based graph edges
    // to a continuous range so that the nodes in the edge based graph are continuous.
    //
    // Luckily node based node ids still coincide with the coordinate array.
    // That's the reason we can only here write out the final compressed node based graph.

    // Dumps to file asynchronously and makes sure we wait for its completion.
    std::future<void> compressed_node_based_graph_writing;

    BOOST_SCOPE_EXIT_ALL(&)
    {
        if (compressed_node_based_graph_writing.valid())
            compressed_node_based_graph_writing.wait();
    };

    compressed_node_based_graph_writing = std::async(std::launch::async, [&] {
        WriteCompressedNodeBasedGraph(
            config.GetPath(".osrm.cnbg").string(), node_based_graph, coordinates);
    });

    node_based_graph_factory.GetCompressedEdges().PrintStatistics();

    const auto &barrier_nodes = node_based_graph_factory.GetBarriers();
    const auto &traffic_signals = node_based_graph_factory.GetTrafficSignals();
    // stealing the annotation data from the node-based graph
    edge_based_nodes_container =
        EdgeBasedNodeDataContainer({}, std::move(node_based_graph_factory.GetAnnotationData()));

    conditional_turn_restrictions =
        removeInvalidRestrictions(std::move(conditional_turn_restrictions), node_based_graph);

    const auto number_of_node_based_nodes = node_based_graph.GetNumberOfNodes();

    const auto number_of_edge_based_nodes =
        BuildEdgeExpandedGraph(node_based_graph,
                               coordinates,
                               node_based_graph_factory.GetCompressedEdges(),
                               barrier_nodes,
                               traffic_signals,
                               turn_restrictions,
                               conditional_turn_restrictions,
                               segregated_edges,
                               turn_lane_map,
                               scripting_environment,
                               edge_based_nodes_container,
                               edge_based_node_segments,
                               node_is_startpoint,
                               edge_based_node_weights,
                               edge_based_edge_list,
                               config.GetPath(".osrm.icd").string());

    TIMER_STOP(expansion);

    // output the geometry of the node-based graph, needs to be done after the last usage, since it
    // destroys internal containers
    files::writeSegmentData(config.GetPath(".osrm.geometry"),
                            *node_based_graph_factory.GetCompressedEdges().ToSegmentData());

    util::Log() << "Saving edge-based node weights to file.";
    TIMER_START(timer_write_node_weights);
    {
        storage::io::FileWriter writer(config.GetPath(".osrm.enw"),
                                       storage::io::FileWriter::GenerateFingerprint);
        storage::serialization::write(writer, edge_based_node_weights);
    }
    TIMER_STOP(timer_write_node_weights);
    util::Log() << "Done writing. (" << TIMER_SEC(timer_write_node_weights) << ")";

    util::Log() << "Computing strictly connected components ...";
    FindComponents(number_of_edge_based_nodes,
                   edge_based_edge_list,
                   edge_based_node_segments,
                   edge_based_nodes_container);

    util::Log() << "Building r-tree ...";
    TIMER_START(rtree);
    BuildRTree(std::move(edge_based_node_segments), std::move(node_is_startpoint), coordinates);

    TIMER_STOP(rtree);

    files::writeNodeData(config.GetPath(".osrm.ebg_nodes"), edge_based_nodes_container);

    util::Log() << "Writing edge-based-graph edges       ... " << std::flush;
    TIMER_START(write_edges);
    files::writeEdgeBasedGraph(
        config.GetPath(".osrm.ebg"), number_of_edge_based_nodes, edge_based_edge_list);
    TIMER_STOP(write_edges);
    util::Log() << "ok, after " << TIMER_SEC(write_edges) << "s";

    util::Log() << "Processed " << edge_based_edge_list.size() << " edges";

    const auto nodes_per_second =
        static_cast<std::uint64_t>(number_of_node_based_nodes / TIMER_SEC(expansion));
    const auto edges_per_second =
        static_cast<std::uint64_t>((number_of_edge_based_nodes) / TIMER_SEC(expansion));

    util::Log() << "Expansion: " << nodes_per_second << " nodes/sec and " << edges_per_second
                << " edges/sec";
    util::Log() << "To prepare the data for routing, run: "
                << "./osrm-contract " << config.GetPath(".osrm");

    return 0;
}

std::tuple<guidance::LaneDescriptionMap,
           std::vector<TurnRestriction>,
           std::vector<ConditionalTurnRestriction>>
Extractor::ParseOSMData(ScriptingEnvironment &scripting_environment,
                        const unsigned number_of_threads)
{
    TIMER_START(extracting);

    util::Log() << "Input file: " << config.input_path.filename().string();
    if (!config.profile_path.empty())
    {
        util::Log() << "Profile: " << config.profile_path.filename().string();
    }
    util::Log() << "Threads: " << number_of_threads;

    const osmium::io::File input_file(config.input_path.string());
    osmium::thread::Pool pool(number_of_threads);

    util::Log() << "Parsing in progress..";
    TIMER_START(parsing);

    { // Parse OSM header
        osmium::io::Reader reader(input_file, osmium::osm_entity_bits::nothing);
        osmium::io::Header header = reader.header();

        std::string generator = header.get("generator");
        if (generator.empty())
        {
            generator = "unknown tool";
        }
        util::Log() << "input file generated by " << generator;

        // write .timestamp data file
        std::string timestamp = header.get("osmosis_replication_timestamp");
        if (timestamp.empty())
        {
            timestamp = "n/a";
        }
        util::Log() << "timestamp: " << timestamp;

        storage::io::FileWriter timestamp_file(config.GetPath(".osrm.timestamp"),
                                               storage::io::FileWriter::GenerateFingerprint);

        timestamp_file.WriteFrom(timestamp.c_str(), timestamp.length());
    }

    // Extraction containers and restriction parser
    ExtractionContainers extraction_containers;
    ExtractorCallbacks::ClassesMap classes_map;
    guidance::LaneDescriptionMap turn_lane_map;
    auto extractor_callbacks =
        std::make_unique<ExtractorCallbacks>(extraction_containers,
                                             classes_map,
                                             turn_lane_map,
                                             scripting_environment.GetProfileProperties());

    // get list of supported relation types
    auto relation_types = scripting_environment.GetRelations();
    std::sort(relation_types.begin(), relation_types.end());

    std::vector<std::string> restrictions = scripting_environment.GetRestrictions();
    // setup restriction parser
    const RestrictionParser restriction_parser(
        scripting_environment.GetProfileProperties().use_turn_restrictions,
        config.parse_conditionals,
        restrictions);

    // OSM data reader
    using SharedBuffer = std::shared_ptr<osmium::memory::Buffer>;
    struct ParsedBuffer
    {
        SharedBuffer buffer;
        std::vector<std::pair<const osmium::Node &, ExtractionNode>> resulting_nodes;
        std::vector<std::pair<const osmium::Way &, ExtractionWay>> resulting_ways;
        std::vector<std::pair<const osmium::Relation &, ExtractionRelation>> resulting_relations;
        std::vector<InputConditionalTurnRestriction> resulting_restrictions;
    };

    ExtractionRelationContainer relations;

    const auto buffer_reader = [](osmium::io::Reader &reader) {
        return tbb::filter_t<void, SharedBuffer>(
            tbb::filter::serial_in_order, [&reader](tbb::flow_control &fc) {
                if (auto buffer = reader.read())
                {
                    return std::make_shared<osmium::memory::Buffer>(std::move(buffer));
                }
                else
                {
                    fc.stop();
                    return SharedBuffer{};
                }
            });
    };

    // Node locations cache (assumes nodes are placed before ways)
    using osmium_index_type =
        osmium::index::map::FlexMem<osmium::unsigned_object_id_type, osmium::Location>;
    using osmium_location_handler_type = osmium::handler::NodeLocationsForWays<osmium_index_type>;

    osmium_index_type location_cache;
    osmium_location_handler_type location_handler(location_cache);

    tbb::filter_t<SharedBuffer, SharedBuffer> location_cacher(
        tbb::filter::serial_in_order, [&location_handler](SharedBuffer buffer) {
            osmium::apply(buffer->begin(), buffer->end(), location_handler);
            return buffer;
        });

    // OSM elements Lua parser
    tbb::filter_t<SharedBuffer, ParsedBuffer> buffer_transformer(
        tbb::filter::parallel, [&](const SharedBuffer buffer) {

            ParsedBuffer parsed_buffer;
            parsed_buffer.buffer = buffer;
            scripting_environment.ProcessElements(*buffer,
                                                  restriction_parser,
                                                  relations,
                                                  parsed_buffer.resulting_nodes,
                                                  parsed_buffer.resulting_ways,
                                                  parsed_buffer.resulting_restrictions);
            return parsed_buffer;
        });

    // Parsed nodes and ways handler
    unsigned number_of_nodes = 0;
    unsigned number_of_ways = 0;
    unsigned number_of_restrictions = 0;
    tbb::filter_t<ParsedBuffer, void> buffer_storage(
        tbb::filter::serial_in_order, [&](const ParsedBuffer &parsed_buffer) {

            number_of_nodes += parsed_buffer.resulting_nodes.size();
            // put parsed objects thru extractor callbacks
            for (const auto &result : parsed_buffer.resulting_nodes)
            {
                extractor_callbacks->ProcessNode(result.first, result.second);
            }
            number_of_ways += parsed_buffer.resulting_ways.size();
            for (const auto &result : parsed_buffer.resulting_ways)
            {
                extractor_callbacks->ProcessWay(result.first, result.second);
            }

            number_of_restrictions += parsed_buffer.resulting_restrictions.size();
            for (const auto &result : parsed_buffer.resulting_restrictions)
            {
                extractor_callbacks->ProcessRestriction(result);
            }
        });

    tbb::filter_t<SharedBuffer, std::shared_ptr<ExtractionRelationContainer>> buffer_relation_cache(
        tbb::filter::parallel, [&](const SharedBuffer buffer) {
            if (!buffer)
                return std::shared_ptr<ExtractionRelationContainer>{};

            auto relations = std::make_shared<ExtractionRelationContainer>();
            for (auto entity = buffer->cbegin(), end = buffer->cend(); entity != end; ++entity)
            {
                if (entity->type() != osmium::item_type::relation)
                    continue;

                const auto &rel = static_cast<const osmium::Relation &>(*entity);

                const char *rel_type = rel.get_value_by_key("type");
                if (!rel_type ||
                    !std::binary_search(
                        relation_types.begin(), relation_types.end(), std::string(rel_type)))
                    continue;

                ExtractionRelation extracted_rel({rel.id(), osmium::item_type::relation});
                for (auto const &t : rel.tags())
                    extracted_rel.attributes.emplace_back(std::make_pair(t.key(), t.value()));

                for (auto const &m : rel.members())
                {
                    ExtractionRelation::OsmIDTyped const mid(m.ref(), m.type());
                    extracted_rel.AddMember(mid, m.role());
                    relations->AddRelationMember(extracted_rel.id, mid);
                }

                relations->AddRelation(std::move(extracted_rel));
            };
            return relations;
        });

    unsigned number_of_relations = 0;
    tbb::filter_t<std::shared_ptr<ExtractionRelationContainer>, void> buffer_storage_relation(
        tbb::filter::serial_in_order,
        [&](const std::shared_ptr<ExtractionRelationContainer> parsed_relations) {

            number_of_relations += parsed_relations->GetRelationsNum();
            relations.Merge(std::move(*parsed_relations));
        });

    // Parse OSM elements with parallel transformer
    // Number of pipeline tokens that yielded the best speedup was about 1.5 * num_cores
    const auto num_threads = tbb::task_scheduler_init::default_num_threads() * 1.5;
    const auto read_meta =
        config.use_metadata ? osmium::io::read_meta::yes : osmium::io::read_meta::no;

    { // Relations reading pipeline
        util::Log() << "Parse relations ...";
        osmium::io::Reader reader(input_file, osmium::osm_entity_bits::relation, read_meta);
        tbb::parallel_pipeline(
            num_threads, buffer_reader(reader) & buffer_relation_cache & buffer_storage_relation);
    }

    { // Nodes and ways reading pipeline
        util::Log() << "Parse ways and nodes ...";
        osmium::io::Reader reader(input_file,
                                  osmium::osm_entity_bits::node | osmium::osm_entity_bits::way |
                                      osmium::osm_entity_bits::relation,
                                  read_meta);

        const auto pipeline =
            scripting_environment.HasLocationDependentData() && config.use_locations_cache
                ? buffer_reader(reader) & location_cacher & buffer_transformer & buffer_storage
                : buffer_reader(reader) & buffer_transformer & buffer_storage;
        tbb::parallel_pipeline(num_threads, pipeline);
    }

    TIMER_STOP(parsing);
    util::Log() << "Parsing finished after " << TIMER_SEC(parsing) << " seconds";

    util::Log() << "Raw input contains " << number_of_nodes << " nodes, " << number_of_ways
                << " ways, and " << number_of_relations << " relations, " << number_of_restrictions
                << " restrictions";

    extractor_callbacks.reset();

    if (extraction_containers.all_edges_list.empty())
    {
        throw util::exception(std::string("There are no edges remaining after parsing.") +
                              SOURCE_REF);
    }

    extraction_containers.PrepareData(scripting_environment,
                                      config.GetPath(".osrm").string(),
                                      config.GetPath(".osrm.names").string());

    auto profile_properties = scripting_environment.GetProfileProperties();
    SetClassNames(scripting_environment.GetClassNames(), classes_map, profile_properties);
    auto excludable_classes = scripting_environment.GetExcludableClasses();
    SetExcludableClasses(classes_map, excludable_classes, profile_properties);
    files::writeProfileProperties(config.GetPath(".osrm.properties").string(), profile_properties);

    TIMER_STOP(extracting);
    util::Log() << "extraction finished after " << TIMER_SEC(extracting) << "s";

    return std::make_tuple(std::move(turn_lane_map),
                           std::move(extraction_containers.unconditional_turn_restrictions),
                           std::move(extraction_containers.conditional_turn_restrictions));
}

void Extractor::FindComponents(unsigned number_of_edge_based_nodes,
                               const util::DeallocatingVector<EdgeBasedEdge> &input_edge_list,
                               const std::vector<EdgeBasedNodeSegment> &input_node_segments,
                               EdgeBasedNodeDataContainer &nodes_container) const
{
    using InputEdge = util::static_graph_details::SortableEdgeWithData<void>;
    using UncontractedGraph = util::StaticGraph<void>;
    std::vector<InputEdge> edges;
    edges.reserve(input_edge_list.size() * 2);

    for (const auto &edge : input_edge_list)
    {
        BOOST_ASSERT_MSG(static_cast<unsigned int>(std::max(edge.data.weight, 1)) > 0,
                         "edge distance < 1");
        BOOST_ASSERT(edge.source < number_of_edge_based_nodes);
        BOOST_ASSERT(edge.target < number_of_edge_based_nodes);
        if (edge.data.forward)
        {
            edges.push_back({edge.source, edge.target});
        }

        if (edge.data.backward)
        {
            edges.push_back({edge.target, edge.source});
        }
    }

    // Connect forward and backward nodes of each edge to enforce
    // forward and backward edge-based nodes be in one strongly-connected component
    for (const auto &segment : input_node_segments)
    {
        if (segment.reverse_segment_id.enabled)
        {
            BOOST_ASSERT(segment.forward_segment_id.id < number_of_edge_based_nodes);
            BOOST_ASSERT(segment.reverse_segment_id.id < number_of_edge_based_nodes);
            edges.push_back({segment.forward_segment_id.id, segment.reverse_segment_id.id});
            edges.push_back({segment.reverse_segment_id.id, segment.forward_segment_id.id});
        }
    }

    tbb::parallel_sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());

    auto uncontracted_graph = UncontractedGraph(number_of_edge_based_nodes, edges);

    TarjanSCC<UncontractedGraph> component_search(uncontracted_graph);
    component_search.Run();

    for (NodeID node_id = 0; node_id < number_of_edge_based_nodes; ++node_id)
    {
        const auto forward_component = component_search.GetComponentID(node_id);
        const auto component_size = component_search.GetComponentSize(forward_component);
        const auto is_tiny = component_size < config.small_component_size;
        BOOST_ASSERT(node_id < nodes_container.NumberOfNodes());
        nodes_container.nodes[node_id].component_id = {1 + forward_component, is_tiny};
    }
}

/**
 \brief Building an edge-expanded graph from node-based input and turn restrictions
*/

EdgeID Extractor::BuildEdgeExpandedGraph(
    // input data
    const util::NodeBasedDynamicGraph &node_based_graph,
    const std::vector<util::Coordinate> &coordinates,
    const CompressedEdgeContainer &compressed_edge_container,
    const std::unordered_set<NodeID> &barrier_nodes,
    const std::unordered_set<NodeID> &traffic_signals,
    const std::vector<TurnRestriction> &turn_restrictions,
    const std::vector<ConditionalTurnRestriction> &conditional_turn_restrictions,
    const std::unordered_set<EdgeID> &segregated_edges,
    // might have to be updated to add new lane combinations
    guidance::LaneDescriptionMap &turn_lane_map,
    // for calculating turn penalties
    ScriptingEnvironment &scripting_environment,
    // output data
    EdgeBasedNodeDataContainer &edge_based_nodes_container,
    std::vector<EdgeBasedNodeSegment> &edge_based_node_segments,
    std::vector<bool> &node_is_startpoint,
    std::vector<EdgeWeight> &edge_based_node_weights,
    util::DeallocatingVector<EdgeBasedEdge> &edge_based_edge_list,
    const std::string &intersection_class_output_file)
{
    util::NameTable name_table(config.GetPath(".osrm.names").string());

    EdgeBasedGraphFactory edge_based_graph_factory(node_based_graph,
                                                   edge_based_nodes_container,
                                                   compressed_edge_container,
                                                   barrier_nodes,
                                                   traffic_signals,
                                                   coordinates,
                                                   name_table,
                                                   segregated_edges,
                                                   turn_lane_map);

    const auto create_edge_based_edges = [&]() {
        // scoped to relase intermediate datastructures right after the call
        std::vector<TurnRestriction> node_restrictions;
        for (auto const &t : turn_restrictions)
            if (t.Type() == RestrictionType::NODE_RESTRICTION)
                node_restrictions.push_back(t);

        std::vector<ConditionalTurnRestriction> conditional_node_restrictions;
        for (auto const &t : conditional_turn_restrictions)
            if (t.Type() == RestrictionType::NODE_RESTRICTION)
                conditional_node_restrictions.push_back(t);

        RestrictionMap via_node_restriction_map(node_restrictions, IndexNodeByFromAndVia());
        WayRestrictionMap via_way_restriction_map(conditional_turn_restrictions);
        ConditionalRestrictionMap conditional_node_restriction_map(conditional_node_restrictions,
                                                                   IndexNodeByFromAndVia());

        edge_based_graph_factory.Run(scripting_environment,
                                     config.GetPath(".osrm.edges").string(),
                                     config.GetPath(".osrm.tld").string(),
                                     config.GetPath(".osrm.turn_weight_penalties").string(),
                                     config.GetPath(".osrm.turn_duration_penalties").string(),
                                     config.GetPath(".osrm.turn_penalties_index").string(),
                                     config.GetPath(".osrm.cnbg_to_ebg").string(),
                                     config.GetPath(".osrm.restrictions").string(),
                                     via_node_restriction_map,
                                     conditional_node_restriction_map,
                                     via_way_restriction_map);
        return edge_based_graph_factory.GetNumberOfEdgeBasedNodes();
    };

    const auto number_of_edge_based_nodes = create_edge_based_edges();

    {
        std::vector<std::uint32_t> turn_lane_offsets;
        std::vector<guidance::TurnLaneType::Mask> turn_lane_masks;
        std::tie(turn_lane_offsets, turn_lane_masks) =
            guidance::transformTurnLaneMapIntoArrays(turn_lane_map);
        files::writeTurnLaneDescriptions(
            config.GetPath(".osrm.tls"), turn_lane_offsets, turn_lane_masks);
    }

    edge_based_graph_factory.GetEdgeBasedEdges(edge_based_edge_list);
    edge_based_graph_factory.GetEdgeBasedNodeSegments(edge_based_node_segments);
    edge_based_graph_factory.GetStartPointMarkers(node_is_startpoint);
    edge_based_graph_factory.GetEdgeBasedNodeWeights(edge_based_node_weights);

    util::Log() << "Writing Intersection Classification Data";
    TIMER_START(write_intersections);
    files::writeIntersections(
        intersection_class_output_file,
        IntersectionBearingsContainer{edge_based_graph_factory.GetBearingClassIds(),
                                      edge_based_graph_factory.GetBearingClasses()},
        edge_based_graph_factory.GetEntryClasses());
    TIMER_STOP(write_intersections);
    util::Log() << "ok, after " << TIMER_SEC(write_intersections) << "s";

    return number_of_edge_based_nodes;
}

/**
    \brief Building rtree-based nearest-neighbor data structure

    Saves tree into '.ramIndex' and leaves into '.fileIndex'.
 */
void Extractor::BuildRTree(std::vector<EdgeBasedNodeSegment> edge_based_node_segments,
                           std::vector<bool> node_is_startpoint,
                           const std::vector<util::Coordinate> &coordinates)
{
    util::Log() << "Constructing r-tree of " << edge_based_node_segments.size()
                << " segments build on-top of " << coordinates.size() << " coordinates";

    BOOST_ASSERT(node_is_startpoint.size() == edge_based_node_segments.size());

    // Filter node based edges based on startpoint
    auto out_iter = edge_based_node_segments.begin();
    auto in_iter = edge_based_node_segments.begin();
    for (auto index : util::irange<std::size_t>(0UL, node_is_startpoint.size()))
    {
        BOOST_ASSERT(in_iter != edge_based_node_segments.end());
        if (node_is_startpoint[index])
        {
            *out_iter = *in_iter;
            out_iter++;
        }
        in_iter++;
    }
    auto new_size = out_iter - edge_based_node_segments.begin();
    if (new_size == 0)
    {
        throw util::exception("There are no snappable edges left after processing.  Are you "
                              "setting travel modes correctly in the profile?  Cannot continue." +
                              SOURCE_REF);
    }
    edge_based_node_segments.resize(new_size);

    TIMER_START(construction);
    util::StaticRTree<EdgeBasedNodeSegment> rtree(edge_based_node_segments,
                                                  config.GetPath(".osrm.ramIndex").string(),
                                                  config.GetPath(".osrm.fileIndex").string(),
                                                  coordinates);

    TIMER_STOP(construction);
    util::Log() << "finished r-tree construction in " << TIMER_SEC(construction) << " seconds";
}

void Extractor::WriteCompressedNodeBasedGraph(const std::string &path,
                                              const util::NodeBasedDynamicGraph &graph,
                                              const std::vector<util::Coordinate> &coordinates)
{
    const auto fingerprint = storage::io::FileWriter::GenerateFingerprint;

    storage::io::FileWriter writer{path, fingerprint};

    // Writes:  | Fingerprint | #e | #n | edges | coordinates |
    // - uint64: number of edges (from, to) pairs
    // - uint64: number of nodes and therefore also coordinates
    // - (uint32_t, uint32_t): num_edges * edges
    // - (int32_t, int32_t: num_nodes * coordinates (lon, lat)

    const auto num_edges = graph.GetNumberOfEdges();
    const auto num_nodes = graph.GetNumberOfNodes();

    BOOST_ASSERT_MSG(num_nodes == coordinates.size(), "graph and embedding out of sync");

    writer.WriteElementCount64(num_edges);
    writer.WriteElementCount64(num_nodes);

    // For all nodes iterate over its edges and dump (from, to) pairs
    for (const NodeID from_node : util::irange(0u, num_nodes))
    {
        for (const EdgeID edge : graph.GetAdjacentEdgeRange(from_node))
        {
            const auto to_node = graph.GetTarget(edge);

            writer.WriteOne(from_node);
            writer.WriteOne(to_node);
        }
    }

    // FIXME this is unneccesary: We have this data
    for (const auto &qnode : coordinates)
    {
        writer.WriteOne(qnode.lon);
        writer.WriteOne(qnode.lat);
    }
}

struct EdgeInfo
{
    NodeID node;

    util::StringView name;

    // 0 - outgoing (forward), 1 - incoming (reverse), 2 - both outgoing and incoming
    int direction;

    ClassData road_class;

    guidance::RoadPriorityClass::Enum road_priority_class;

    struct LessName
    {
        bool operator()(EdgeInfo const &e1, EdgeInfo const &e2) const { return e1.name < e2.name; }
    };
};

bool IsSegregated(std::vector<EdgeInfo> v1,
                  std::vector<EdgeInfo> v2,
                  EdgeInfo const &current,
                  double edgeLength)
{
    if (v1.size() < 2 || v2.size() < 2)
        return false;

    auto const sort_by_name_fn = [](std::vector<EdgeInfo> &v) {
        std::sort(v.begin(), v.end(), EdgeInfo::LessName());
    };

    sort_by_name_fn(v1);
    sort_by_name_fn(v2);

    // Internal edge with the name should be connected with any other neibour edge with the same
    // name, e.g. isolated edge with unique name is not segregated.
    //              b - 'b' road continues here
    //              |
    //      - - a - |
    //              b - segregated edge
    //      - - a - |
    if (!current.name.empty())
    {
        auto const findNameFn = [&current](std::vector<EdgeInfo> const &v) {
            return std::binary_search(v.begin(), v.end(), current, EdgeInfo::LessName());
        };

        if (!findNameFn(v1) && !findNameFn(v2))
            return false;
    }

    // set_intersection like routine to get equal result pairs
    std::vector<std::pair<EdgeInfo const *, EdgeInfo const *>> commons;

    auto i1 = v1.begin();
    auto i2 = v2.begin();

    while (i1 != v1.end() && i2 != v2.end())
    {
        if (i1->name == i2->name)
        {
            if (!i1->name.empty())
                commons.push_back(std::make_pair(&(*i1), &(*i2)));

            ++i1;
            ++i2;
        }
        else if (i1->name < i2->name)
            ++i1;
        else
            ++i2;
    }

    if (commons.size() < 2)
        return false;

    auto const check_equal_class = [](std::pair<EdgeInfo const *, EdgeInfo const *> const &e) {
        // Or (e.first->road_class & e.second->road_class != 0)
        return e.first->road_class == e.second->road_class;
    };

    size_t equal_class_count = 0;
    for (auto const &e : commons)
        if (check_equal_class(e))
            ++equal_class_count;

    if (equal_class_count < 2)
        return false;

    auto const get_length_threshold = [](EdgeInfo const *e) {
        switch (e->road_priority_class)
        {
        case guidance::RoadPriorityClass::MOTORWAY:
        case guidance::RoadPriorityClass::TRUNK:
            return 30.0;
        case guidance::RoadPriorityClass::PRIMARY:
            return 20.0;
        case guidance::RoadPriorityClass::SECONDARY:
        case guidance::RoadPriorityClass::TERTIARY:
            return 10.0;
        default:
            return 5.0;
        }
    };

    double threshold = std::numeric_limits<double>::max();
    for (auto const &e : commons)
        threshold =
            std::min(threshold, get_length_threshold(e.first) + get_length_threshold(e.second));

    return edgeLength <= threshold;
}

std::unordered_set<EdgeID> Extractor::FindSegregatedNodes(NodeBasedGraphFactory &factory)
{
    util::NameTable names(config.GetPath(".osrm.names").string());

    auto const &graph = factory.GetGraph();
    auto const &annotation = factory.GetAnnotationData();

    guidance::CoordinateExtractor coordExtractor(
        graph, factory.GetCompressedEdges(), factory.GetCoordinates());

    auto const get_edge_length = [&](NodeID from_node, EdgeID edgeID, NodeID to_node) {
        auto const geom = coordExtractor.GetCoordinatesAlongRoad(from_node, edgeID, false, to_node);
        double length = 0.0;
        for (size_t i = 1; i < geom.size(); ++i)
        {
            length += osrm::util::coordinate_calculation::haversineDistance(geom[i - 1], geom[i]);
        }
        return length;
    };

    auto const get_edge_info = [&](NodeID node, auto const &edgeData) -> EdgeInfo {
        /// @todo Make string normalization/lowercase/trim for comparison ...

        auto const id = annotation[edgeData.annotation_data].name_id;
        BOOST_ASSERT(id != INVALID_NAMEID);
        auto const name = names.GetNameForID(id);

        return {node,
                name,
                edgeData.reversed ? 1 : 0,
                annotation[edgeData.annotation_data].classes,
                edgeData.flags.road_classification.GetClass()};
    };

    auto const collect_edge_info_fn = [&](auto const &edges1, NodeID node2) {
        std::vector<EdgeInfo> info;

        for (auto const &e : edges1)
        {
            NodeID const target = graph.GetTarget(e);
            if (target == node2)
                continue;

            info.push_back(get_edge_info(target, graph.GetEdgeData(e)));
        }

        if (info.empty())
            return info;

        std::sort(info.begin(), info.end(), [](EdgeInfo const &e1, EdgeInfo const &e2) {
            return e1.node < e2.node;
        });

        // Merge equal infos with correct direction.
        auto curr = info.begin();
        auto next = curr;
        while (++next != info.end())
        {
            if (curr->node == next->node)
            {
                BOOST_ASSERT(curr->name == next->name);
                BOOST_ASSERT(curr->road_class == next->road_class);
                BOOST_ASSERT(curr->direction != next->direction);
                curr->direction = 2;
            }
            else
                curr = next;
        }

        info.erase(
            std::unique(info.begin(),
                        info.end(),
                        [](EdgeInfo const &e1, EdgeInfo const &e2) { return e1.node == e2.node; }),
            info.end());

        return info;
    };

    auto const isSegregatedFn = [&](auto const &edgeData,
                                    auto const &edges1,
                                    NodeID node1,
                                    auto const &edges2,
                                    NodeID node2,
                                    double edgeLength) {
        return IsSegregated(collect_edge_info_fn(edges1, node2),
                            collect_edge_info_fn(edges2, node1),
                            get_edge_info(node1, edgeData),
                            edgeLength);
    };

    std::unordered_set<EdgeID> segregated_edges;

    for (NodeID sourceID = 0; sourceID < graph.GetNumberOfNodes(); ++sourceID)
    {
        auto const sourceEdges = graph.GetAdjacentEdgeRange(sourceID);
        for (EdgeID edgeID : sourceEdges)
        {
            auto const &edgeData = graph.GetEdgeData(edgeID);

            if (edgeData.reversed)
                continue;

            NodeID const targetID = graph.GetTarget(edgeID);
            auto const targetEdges = graph.GetAdjacentEdgeRange(targetID);

            double const length = get_edge_length(sourceID, edgeID, targetID);
            if (isSegregatedFn(edgeData, sourceEdges, sourceID, targetEdges, targetID, length))
                segregated_edges.insert(edgeID);
        }
    }

    return segregated_edges;
}

} // namespace extractor
} // namespace osrm
