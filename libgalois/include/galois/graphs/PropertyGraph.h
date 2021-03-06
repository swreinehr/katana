#ifndef GALOIS_LIBGALOIS_GALOIS_GRAPHS_PROPERTYGRAPH_H_
#define GALOIS_LIBGALOIS_GALOIS_GRAPHS_PROPERTYGRAPH_H_

#include <tuple>

#include <arrow/type_fwd.h>
#include <boost/iterator/counting_iterator.hpp>

#include "galois/NoDerefIterator.h"
#include "galois/Properties.h"
#include "galois/Result.h"
#include "galois/Traits.h"
#include "galois/graphs/Details.h"
#include "galois/graphs/PropertyFileGraph.h"
#include "galois/graphs/PropertyViews.h"

namespace galois::graphs {

/// A property graph is a graph that has properties associated with its nodes
/// and edges. A property has a name and value. Its value may be a primitive
/// type, a list of values or a composition of properties.
///
/// A PropertyGraph is a representation of a property graph that imposes a
/// typed view on top of an underlying \ref PropertyFileGraph. A
/// PropertyFileGraph is appropriate for cases where the graph is largely
/// uninterpreted and can be manipulated as a collection of bits. A
/// PropertyGraph is appropriate for cases where computation needs to be done
/// on the properties themselves.
///
/// \tparam NodeProps A tuple of property types (\ref Properties.h) for nodes
/// \tparam EdgeProps A tuple of property types for edges
template <typename NodeProps, typename EdgeProps>
class PropertyGraph {
  using NodeView = PropertyViewTuple<NodeProps>;
  using EdgeView = PropertyViewTuple<EdgeProps>;

  PropertyFileGraph* pfg_;

  NodeView node_view_;
  EdgeView edge_view_;

  PropertyGraph(PropertyFileGraph* pfg, NodeView node_view, EdgeView edge_view)
      : pfg_(pfg),
        node_view_(std::move(node_view)),
        edge_view_(std::move(edge_view)) {}

public:
  using node_properties = NodeProps;
  using edge_properties = EdgeProps;
  using node_iterator = boost::counting_iterator<uint32_t>;
  using edge_iterator = boost::counting_iterator<uint64_t>;
  using edges_iterator = StandardRange<NoDerefIterator<edge_iterator>>;
  using iterator = node_iterator;
  using Node = uint32_t;

  // Standard container concepts

  node_iterator begin() const { return node_iterator(0); }

  node_iterator end() const { return node_iterator(num_nodes()); }

  size_t size() const { return num_nodes(); }

  bool empty() const { return num_nodes() == 0; }

  // Graph accessors

  /**
   * Gets the node data.
   *
   * @param node node to get the data of
   * @returns reference to the node data
   */
  template <typename NodeIndex>
  PropertyReferenceType<NodeIndex> GetData(const Node& node) {
    constexpr size_t prop_index = find_trait<NodeIndex, NodeProps>();
    return std::get<prop_index>(node_view_).GetValue(node);
  }
  template <typename NodeIndex>
  PropertyReferenceType<NodeIndex> GetData(const node_iterator& node) {
    return GetData<NodeIndex>(*node);
  }

  /**
   * Gets the node data.
   *
   * @param node node to get the data of
   * @returns const reference to the node data
   */
  template <typename NodeIndex>
  PropertyConstReferenceType<NodeIndex> GetData(const Node& node) const {
    constexpr size_t prop_index = find_trait<NodeIndex, NodeProps>();
    return std::get<prop_index>(node_view_).GetValue(node);
  }
  template <typename NodeIndex>
  PropertyConstReferenceType<NodeIndex> GetData(
      const node_iterator& node) const {
    return GetData<NodeIndex>(*node);
  }

  /**
   * Gets the edge data.
   *
   * @param edge edge iterator to get the data of
   * @returns reference to the edge data
   */
  template <typename EdgeIndex>
  PropertyReferenceType<EdgeIndex> GetEdgeData(const edge_iterator& edge) {
    constexpr size_t prop_index = find_trait<EdgeIndex, EdgeProps>();
    return std::get<prop_index>(edge_view_).GetValue(*edge);
  }

  /**
   * Gets the edge data.
   *
   * @param edge edge iterator to get the data of
   * @returns const reference to the edge data
   */
  template <typename EdgeIndex>
  PropertyConstReferenceType<EdgeIndex> GetEdgeData(
      const edge_iterator& edge) const {
    constexpr size_t prop_index = find_trait<EdgeIndex, EdgeProps>();
    return std::get<prop_index>(edge_view_).GetValue(*edge);
  }

  /**
   * Gets the destination for an edge.
   *
   * @param edge edge iterator to get the destination of
   * @returns node iterator to the edge destination
   */
  node_iterator GetEdgeDest(const edge_iterator& edge) const {
    auto node_id = pfg_->topology().out_dests->Value(*edge);
    return node_iterator(node_id);
  }

  uint64_t num_nodes() const { return pfg_->topology().num_nodes(); }
  uint64_t num_edges() const { return pfg_->topology().num_edges(); }

  /**
   * Gets the edge range of some node.
   *
   * @param node node to get the edge range of
   * @returns iterator to edges of node
   */
  edges_iterator edges(const node_iterator& node) const {
    auto [begin_edge, end_edge] = pfg_->topology().edge_range(*node);
    return internal::make_no_deref_range(
        edge_iterator(begin_edge), edge_iterator(end_edge));
  }

  /**
   * Gets the first edge of some node.
   *
   * @param node node to get the edge of
   * @returns iterator to first edge of node
   */
  edge_iterator edge_begin(Node node) const { return *edges(node).begin(); }

  /**
   * Gets the end edge boundary of some node.
   *
   * @param node node to get the edge of
   * @returns iterator to the end of the edges of node, i.e. the first edge of
   *     the next node (or an "end" iterator if there is no next node)
   */
  edge_iterator edge_end(Node node) const { return *edges(node).end(); }

  /**
   * Accessor for the underlying PropertyFileGraph.
   *
   * @returns pointer to the underlying PropertyFileGraph.
   */
  const PropertyFileGraph& GetPropertyFileGraph() const { return *pfg_; }

  // Graph constructors
  static Result<PropertyGraph<NodeProps, EdgeProps>> Make(
      PropertyFileGraph* pfg, const std::vector<std::string>& node_properties,
      const std::vector<std::string>& edge_properties);
  static Result<PropertyGraph<NodeProps, EdgeProps>> Make(
      PropertyFileGraph* pfg);
};

/**
   * Finds a node in the sorted edgelist of some other node using binary search.
   *
   * @param graph graph to search in the topology of
   * @param node node to find in the edgelist of
   * @param node_to_find node id of the node to find in the edgelist of "node"
   * @returns iterator to the edge with id "node_to_find" if present else return "end" iterator
   */
template <typename GraphTy>
GALOIS_EXPORT typename GraphTy::edge_iterator
FindEdgeSortedByDest(
    const GraphTy& graph, typename GraphTy::Node node,
    typename GraphTy::Node node_to_find) {
  auto edge_matched = galois::graphs::FindEdgeSortedByDest(
      graph.GetPropertyFileGraph(), node, node_to_find);
  return typename GraphTy::edge_iterator(edge_matched);
}

template <typename NodeProps, typename EdgeProps>
Result<PropertyGraph<NodeProps, EdgeProps>>
PropertyGraph<NodeProps, EdgeProps>::Make(
    PropertyFileGraph* pfg, const std::vector<std::string>& node_properties,
    const std::vector<std::string>& edge_properties) {
  auto node_view_result =
      internal::MakeNodePropertyViews<NodeProps>(pfg, node_properties);
  if (!node_view_result) {
    return node_view_result.error();
  }

  auto edge_view_result =
      internal::MakeEdgePropertyViews<EdgeProps>(pfg, edge_properties);
  if (!edge_view_result) {
    return edge_view_result.error();
  }

  return PropertyGraph(
      pfg, std::move(node_view_result.value()),
      std::move(edge_view_result.value()));
}

template <typename NodeProps, typename EdgeProps>
Result<PropertyGraph<NodeProps, EdgeProps>>
PropertyGraph<NodeProps, EdgeProps>::Make(PropertyFileGraph* pfg) {
  return PropertyGraph<NodeProps, EdgeProps>::Make(
      pfg, pfg->node_schema()->field_names(),
      pfg->edge_schema()->field_names());
}

}  // namespace galois::graphs

#endif
