/*
 * This file belongs to the Galois project, a C++ library for exploiting
 * parallelism. The code is being released under the terms of the 3-Clause BSD
 * License (a copy is located in LICENSE.txt at the top-level directory).
 *
 * Copyright (C) 2018, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 */

#ifndef LONESTAR_K_SSSP_H
#define LONESTAR_K_SSSP_H

#include <atomic>
#include <cstdlib>
#include <iostream>

#include "Lonestar/Utils.h"

template <
    typename Graph, typename Distance, typename Path, bool UseEdgeWt,
    ptrdiff_t EdgeTileSize = 256>
struct K_SSSP {
  constexpr static const Distance kDistInfinity =
      std::numeric_limits<Distance>::max() / 4;

  using GNode = typename Graph::Node;
  using EI = typename Graph::edge_iterator;

  struct UpdateRequest {
    GNode src;
    Distance distance;
    Path* path;
    UpdateRequest(const GNode& N, Distance W, Path* P)
        : src(N), distance(W), path(P) {}
    UpdateRequest() : src(), distance(0), path(NULL) {}

    friend bool operator<(
        const UpdateRequest& left, const UpdateRequest& right) {
      return left.distance == right.distance ? left.src < right.src
                                             : left.distance < right.distance;
    }
  };

  struct UpdateRequestIndexer {
    unsigned shift;

    template <typename R>
    unsigned int operator()(const R& req) const {
      unsigned int t = req.distance >> shift;
      return t;
    }
  };

  struct SrcEdgeTile {
    GNode src;
    Distance distance;
    Path* path;
    EI beg;
    EI end;

    friend bool operator<(const SrcEdgeTile& left, const SrcEdgeTile& right) {
      return left.distance == right.distance ? left.src < right.src
                                             : left.distance < right.distance;
    }
  };

  struct SrcEdgeTileMaker {
    GNode src;
    Distance distance;
    Path* path;

    SrcEdgeTile operator()(const EI& beg, const EI& end) const {
      return SrcEdgeTile{src, distance, path, beg, end};
    }
  };

  template <typename WL, typename TileMaker>
  static void PushEdgeTiles(WL& wl, EI beg, const EI end, const TileMaker& f) {
    assert(beg <= end);

    if ((end - beg) > EdgeTileSize) {
      for (; beg + EdgeTileSize < end;) {
        auto ne = beg + EdgeTileSize;
        assert(ne < end);
        wl.push(f(beg, ne));
        beg = ne;
      }
    }

    if ((end - beg) > 0) {
      wl.push(f(beg, end));
    }
  }

  template <typename WL, typename TileMaker>
  static void PushEdgeTiles(
      WL& wl, Graph* graph, GNode src, const TileMaker& f) {
    auto beg = graph->edge_begin(src);
    const auto end = graph->edge_end(src);

    PushEdgeTiles(wl, beg, end, f);
  }

  template <typename WL, typename TileMaker>
  static void PushEdgeTilesParallel(
      WL& wl, Graph* graph, GNode src, const TileMaker& f) {
    auto beg = graph->edge_begin(src);
    const auto end = graph->edge_end(src);

    if ((end - beg) > EdgeTileSize) {
      galois::on_each(
          [&](const unsigned tid, const unsigned numT) {
            auto p = galois::block_range(beg, end, tid, numT);

            auto b = p.first;
            const auto e = p.second;

            PushEdgeTiles(wl, b, e, f);
          },
          galois::loopname("Init-Tiling"));

    } else if ((end - beg) > 0) {
      wl.push(f(beg, end));
    }
  }

  struct ReqPushWrap {
    template <typename C>
    void operator()(
        C& cont, const GNode& n, const Distance& distance, const Path* path,
        const char* const) const {
      (*this)(cont, n, distance, path);
    }

    template <typename C>
    void operator()(
        C& cont, const GNode& n, const Distance& distance,
        const Path* path) const {
      cont.push(UpdateRequest(n, distance, path));
    }
  };

  struct SrcEdgeTilePushWrap {
    Graph* graph;

    template <typename C>
    void operator()(
        C& cont, const GNode& n, const Distance& distance, const Path* path,
        const char* const) const {
      PushEdgeTilesParallel(
          cont, graph, n, SrcEdgeTileMaker{n, distance, path});
    }

    template <typename C>
    void operator()(
        C& cont, const GNode& n, const Distance& distance,
        const Path* path) const {
      PushEdgeTiles(cont, graph, n, SrcEdgeTileMaker{n, distance, path});
    }
  };

  struct OutEdgeRangeFn {
    Graph* graph;
    auto operator()(const GNode& n) const { return graph->edges(n); }

    auto operator()(const UpdateRequest& req) const {
      return graph->edges(req.src);
    }
  };

  struct TileRangeFn {
    template <typename T>
    auto operator()(const T& tile) const {
      return galois::makeIterRange(tile.beg, tile.end);
    }
  };

  template <typename NodeProp, typename EdgeProp>
  struct SanityCheck {
    Graph* g;
    std::atomic<bool>& refb;
    SanityCheck(Graph* g, std::atomic<bool>& refb) : g(g), refb(refb) {}

    template <bool useWt, typename iiTy>
    Distance GetEdgeWeight(
        iiTy, typename std::enable_if<!useWt>::type* = nullptr) const {
      return 1;
    }

    template <bool useWt, typename iiTy>
    Distance GetEdgeWeight(
        iiTy ii, typename std::enable_if<useWt>::type* = nullptr) const {
      return g->template GetEdgeData<EdgeProp>(ii);
    }

    void operator()(typename Graph::Node node) const {
      Distance sd = g->template GetData<NodeProp>(node);
      if (sd == kDistInfinity) {
        return;
      }

      for (auto ii : g->edges(node)) {
        auto dest = g->GetEdgeDst(ii);
        Distance dd = g->template GetData<NodeProp>(*dest);
        Distance ew = GetEdgeWeight<UseEdgeWt>(ii);
        if (dd > sd + ew) {
          galois::gPrint(
              "Wrong label: ", dd, ", on node: ", *dest,
              ", correct label from src node ", node, " is ", sd + ew, "\n");
          refb = true;
          // return;
        }
      }
    }
  };

  template <typename NodeProp>
  struct MaxDist {
    Graph* g;
    galois::GReduceMax<Distance>& m;

    MaxDist(Graph* g, galois::GReduceMax<Distance>& m) : g(g), m(m) {}

    void operator()(typename Graph::Node node) const {
      Distance d = g->template GetData<NodeProp>(node);
      if (d != kDistInfinity) {
        m.update(d);
      }
    }
  };

  template <typename NodeProp, typename EdgeProp = galois::PODProperty<int64_t>>
  static bool Verify(Graph* graph, GNode source) {
    if (graph->template GetData<NodeProp>(source) != 0) {
      GALOIS_LOG_ERROR(
          "ERROR: source has non-zero dist value == ",
          graph->template GetData<NodeProp>(source), "\n");
      return false;
    }

    std::atomic<size_t> not_visited(0);
    galois::do_all(galois::iterate(*graph), [&not_visited, &graph](GNode node) {
      if (graph->template GetData<NodeProp>(node) >= kDistInfinity) {
        ++not_visited;
      }
    });

    if (not_visited) {
      GALOIS_LOG_WARN(
          "{} unvisited nodes; this is an error if the graph is strongly "
          "connected\n",
          not_visited);
    }

    std::atomic<bool> not_c(false);
    galois::do_all(
        galois::iterate(*graph), SanityCheck<NodeProp, EdgeProp>(graph, not_c));

    if (not_c) {
      GALOIS_LOG_ERROR("node found with incorrect distance\n");
      return false;
    }

    galois::GReduceMax<Distance> m;
    galois::do_all(galois::iterate(*graph), MaxDist<NodeProp>(graph, m));

    galois::gPrint("max dist: ", m.reduce(), "\n");

    return true;
  }
};
#endif  //  LONESTAR_K_SSSP_H
