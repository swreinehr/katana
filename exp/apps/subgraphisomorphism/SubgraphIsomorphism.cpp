/** Subgraph isomorphism -*- C++ -*-
 * @file
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2013, The University of Texas at Austin. All rights reserved.
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
 *
 * @section Description
 *
 * Subgraph isomorphism.
 *
 * @author Yi-Shan Lu <yishanlu@cs.utexas.edu>
 */
#include "Galois/Galois.h"
#include "Galois/Bag.h"
#include "Galois/Statistic.h"
#include "Galois/Timer.h"
#include "Galois/Graphs/Graph.h"
#include "Galois/Graphs/TypeTraits.h"
#include "llvm/Support/CommandLine.h"
#include "Lonestar/BoilerPlate.h"
#include "Galois/Accumulator.h"
#include "Galois/PerThreadContainer.h"

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <atomic>

#include <chrono>
#include <random>

namespace cll = llvm::cl;

static const char* name = "Subgraph Isomorphism";
static const char* desc =
  "Computes up to k isomorphism on data graph for each query graph";
static const char* url = "subgraph_isomorphism";

enum Algo {
  ullmann, 
  vf2
};

static cll::opt<unsigned int> kFound("kFound", cll::desc("stop when k instances found"), cll::init(10));
static cll::opt<bool> undirected("undirected", cll::desc("undirected data and query graphs"), cll::init(false));

static cll::opt<std::string> graphD("graphD", cll::desc("<data graph file>"));
static cll::opt<std::string> graphQ("graphQ", cll::desc("<query graph file>"));

static cll::opt<unsigned int> numLabels("numLabels", cll::desc("# labels"), cll::init(2));

static cll::opt<bool> rndSeedQByTime("rndSeedQByTime", cll::desc("rndSeedQ generated by system time"), cll::init(false));
static cll::opt<unsigned int> rndSeedQ("rndSeedQ", cll::desc("random seed Q"), cll::init(0));

static cll::opt<bool> rndSeedDByTime("rndSeedDByTime", cll::desc("rndSeedD generated by system time"), cll::init(false));
static cll::opt<unsigned int> rndSeedD("rndSeedD", cll::desc("random seed D"), cll::init(0));

static cll::opt<Algo> algo("algo", cll::desc("Choose an algorithm:"),
    cll::values(
      clEnumValN(Algo::ullmann, "ullmann", "Ullmann (default)"),
      clEnumValN(Algo::vf2, "vf2", "VF2"), 
      clEnumValEnd), cll::init(Algo::ullmann));

struct DNode {
  char label;
  unsigned int id;
};

typedef Galois::Graph::LC_CSR_Graph<DNode, void> InnerDGraph; // graph with DNode nodes and typeless edges
typedef Galois::Graph::LC_InOut_Graph<InnerDGraph> DGraph;
typedef DGraph::GraphNode DGNode;

struct QNode {
  char label;
  unsigned int id;
  std::vector<DGNode> candidate;
};

typedef Galois::Graph::LC_CSR_Graph<QNode, void> InnerQGraph;
typedef Galois::Graph::LC_InOut_Graph<InnerQGraph> QGraph;
typedef QGraph::GraphNode QGNode;

struct NodeMatch {
  QGNode nQ;
  DGNode nD;
  NodeMatch(const QGNode q, const DGNode d): nQ(q), nD(d) {}
  NodeMatch(): nQ(), nD() {}
};

typedef std::vector<NodeMatch> Matching;
typedef Galois::InsertBag<Matching> MatchingVector;

static std::minstd_rand0 generator;
static std::uniform_int_distribution<unsigned> distribution;

static std::atomic_uint currentlyFound;

template<typename Graph>
void printGraph(Graph& g) {
  for(auto ni = g.begin(), ne = g.end(); ni != ne; ++ni) {
    auto& data = g.getData(*ni);
    std::cout << "node " << data.id << ": " << data.label << std::endl;
    for(auto ei = g.edge_begin(*ni), ee = g.edge_end(*ni); ei != ee; ++ei) {
      auto& dstData = g.getData(g.getEdgeDst(ei));
      std::cout << "  edge to node " << dstData.id << std::endl;
    }
  }
  std::cout << std::endl;
}

template<typename Graph>
void initializeGraph(Graph& g, unsigned int seed) {
  typedef typename Graph::node_data_type Node;

  generator.seed(seed);

  unsigned int i = 0;
  for(auto ni = g.begin(), ne = g.end(); ni != ne; ++ni) {
    Node& data = g.getData(*ni);
    data.id = i++;
    data.label = 'A' + distribution(generator) % numLabels;

    g.sortEdgesByDst(*ni);
  }
}

// assume gQ is connected
struct VF2Algo {
  std::string name() const { return "VF2"; }

  // query state
  Galois::PerThreadSet<QGNode> qFrontier;
  Galois::PerThreadSet<QGNode> qMatched;

  // data state
  Galois::PerThreadSet<DGNode> dFrontier;
  Galois::PerThreadSet<DGNode> dMatched;

  // instrumented stat
  Galois::GReduceMax<size_t> dFrontierSize;

  struct FilterCandidatesInternal {
    DGraph& gD;
    QGraph& gQ;
    Galois::GReduceLogicalOR& nodeEmpty;
    FilterCandidatesInternal(DGraph& d, QGraph& q, Galois::GReduceLogicalOR& lor): gD(d), gQ(q), nodeEmpty(lor) {}

    void operator()(const QGNode n) const {
      auto& dQ = gQ.getData(n);

      for(auto di = gD.begin(), de = gD.end(); di != de; ++di) {
        auto& dD = gD.getData(*di);

        if(dQ.label != dD.label) {
          continue;
        }

        // self loop for n but not for *di
        if(gQ.findEdge(n, n) != gQ.edge_end(n) && gD.findEdge(*di, *di) == gD.edge_end(*di)) {
          continue;
        }

        dQ.candidate.push_back(*di);
      }

      std::sort(dQ.candidate.begin(), dQ.candidate.end());
      nodeEmpty.update(dQ.candidate.empty());
    }
  };

  // return true if at least one node has an empty set of candidates
  bool filterCandidates(DGraph& gD, QGraph& gQ) {
    Galois::GReduceLogicalOR isSomeNodeEmpty;
    Galois::do_all_local(gQ, FilterCandidatesInternal(gD, gQ, isSomeNodeEmpty), Galois::loopname("filter"), Galois::do_all_steal<true>());
    return isSomeNodeEmpty.reduce();
  }

  QGNode nextQueryNode() {
    return *(qFrontier.get().begin());
  }

  template<typename Graph, typename Set>
  void countInNeighbors(Graph& g, typename Graph::GraphNode n, Set& matched, Set& frontier, long int *numFrontier, long int *numOther) {
    for(auto ei = g.in_edge_begin(n), ee = g.in_edge_end(n); ei != ee; ++ei) {
      auto ngh = g.getInEdgeDst(ei);
      if(frontier.count(ngh)) {
        *numFrontier += 1;
      } else { 
        *numOther += (1 - matched.count(ngh));
      }
    }
  }

  template<typename Graph, typename Set>
  void countNeighbors(Graph& g, typename Graph::GraphNode n, Set& matched, Set& frontier, long int *numFrontier, long int *numOther) {
    for(auto ei = g.edge_begin(n), ee = g.edge_end(n); ei != ee; ++ei) { 
      auto ngh = g.getEdgeDst(ei);
      if(frontier.count(ngh)) {
        *numFrontier += 1;
      } else {
        *numOther += (1 - matched.count(ngh));
      }
    }
  } 

  void refineCandidates(DGraph& gD, QGraph& gQ, QGNode nQuery, std::vector<DGNode>& refined) {
    auto numNghQ = std::distance(gQ.edge_begin(nQuery), gQ.edge_end(nQuery));
    long int numFrontierNghQ = 0, numOtherNghQ = 0;
    countNeighbors(gQ, nQuery, qMatched.get(), qFrontier.get(), &numFrontierNghQ, &numOtherNghQ);

    long int numInNghQ = 0, numFrontierInNghQ = 0, numOtherInNghQ = 0;
    if(!undirected) {
      numInNghQ = std::distance(gQ.in_edge_begin(nQuery), gQ.in_edge_end(nQuery));
      countInNeighbors(gQ, nQuery, qMatched.get(), qFrontier.get(), &numFrontierInNghQ, &numOtherInNghQ);
    }

    // consider all nodes in data frontier
    auto& dQ = gQ.getData(nQuery);
    for(auto ii = dFrontier.get().begin(), ie = dFrontier.get().end(); ii != ie; ++ii) {
      // not a candidate for nQuery
      if(!std::binary_search(dQ.candidate.begin(), dQ.candidate.end(), *ii)) {
        continue;
      }

      auto numNghD = std::distance(gD.edge_begin(*ii), gD.edge_end(*ii));
      if(numNghD < numNghQ) {
        continue;
      }

      long int numFrontierNghD = 0, numOtherNghD = 0;
      countNeighbors(gD, *ii, dMatched.get(), dFrontier.get(), &numFrontierNghD, &numOtherNghD);
      if(numFrontierNghD < numFrontierNghQ) {
        continue;
      }
      if(numOtherNghD < numOtherNghQ) {
        continue;
      }

      if(undirected) {
        refined.push_back(*ii);
        continue;
      }

      auto numInNghD = std::distance(gD.in_edge_begin(*ii), gD.in_edge_end(*ii));
      if(numInNghD < numInNghQ) {
        continue;
      }

      long int numFrontierInNghD = 0, numOtherInNghD = 0;    
      countInNeighbors(gD, *ii, dMatched.get(), dFrontier.get(), &numFrontierInNghD, &numOtherInNghD);
      if(numFrontierInNghD < numFrontierInNghQ) {
        continue;
      }
      if(numOtherInNghD < numOtherInNghQ) {
        continue;
      }

      refined.push_back(*ii);
    }
  }

  bool isJoinable(DGraph& gD, QGraph& gQ, DGNode nD, QGNode nQ, Matching& matching) {
    for(auto mi = matching.begin(), me = matching.end(); mi != me; ++mi) {
      // nD is already matched
      if(nD == mi->nD) {
        return false;
      }

      // nQ => (mi->nQ) exists but not nD => (mi->nD)
      if(gQ.findEdge(nQ, mi->nQ) != gQ.edge_end(nQ) && gD.findEdge(nD, mi->nD) == gD.edge_end(nD)) {
        return false;
      }

      // (mi->nQ) => nQ exists but not (mi->nD) => nD
      // skip if both data and query graphs are directed
      if(!undirected) {
        if(gQ.findEdge(mi->nQ, nQ) != gQ.edge_end(mi->nQ) && gD.findEdge(mi->nD, nD) == gD.edge_end(mi->nD)) {
          return false;
        }
      }
    }

    return true;
  }

  // TODO: manipulate sets with incoming neighbors
  struct SubgraphSearchInternal {
    typedef int tt_does_not_need_aborts;
    typedef int tt_does_not_need_push;
    typedef int tt_needs_parallel_break;
    typedef int tt_does_not_need_stats;

    DGraph& gD;
    QGraph& gQ;
    MatchingVector& report;
    VF2Algo *algo;
    SubgraphSearchInternal(DGraph& d, QGraph& q, MatchingVector& r, VF2Algo *algo): gD(d), gQ(q), report(r), algo(algo) {}

    void doSearch(Matching& matching) {
      if(currentlyFound.load() >= kFound) {
          return;
      }

      if(matching.size() == gQ.size()) {
        report.push_back(matching);
        currentlyFound += 1;
        return;
      }

      auto nQ = algo->nextQueryNode();

      std::vector<DGNode> refined;
      algo->refineCandidates(gD, gQ, nQ, refined);

      // update query state
      algo->qMatched.get().insert(nQ);
      algo->qFrontier.get().erase(nQ);

      std::vector<QGNode> qAdd2Frontier;
      for(auto ei = gQ.edge_begin(nQ), ee = gQ.edge_end(nQ); ei != ee; ++ei) {
        auto ngh = gQ.getEdgeDst(ei);
        if(algo->qMatched.get().count(ngh)) {
          continue;
        }
        if(true == algo->qFrontier.get().insert(ngh).second) {
          qAdd2Frontier.push_back(ngh);
        }
      }
      for(auto ei = gQ.in_edge_begin(nQ), ee = gQ.in_edge_end(nQ); ei != ee; ++ei) {
        auto ngh = gQ.getInEdgeDst(ei);
        if(algo->qMatched.get().count(ngh)) {
          continue;
        }
        if(true == algo->qFrontier.get().insert(ngh).second) {
          qAdd2Frontier.push_back(ngh);
        }
      }

      // search for all possible candidate data nodes
      for(auto ri = refined.begin(), re = refined.end(); ri != re; ++ri) {
        if(!algo->isJoinable(gD, gQ, *ri, nQ, matching)) {
          continue;
        }

        // add (nQ, *ri) to matching 
        matching.push_back(NodeMatch(nQ, *ri));

        // update data state
        algo->dMatched.get().insert(*ri);
        algo->dFrontier.get().erase(*ri);

        std::vector<DGNode> dAdd2Frontier;
        for(auto ei = gD.edge_begin(*ri), ee = gD.edge_end(*ri); ei != ee; ++ei) {
          auto ngh = gD.getEdgeDst(ei);
          if(algo->dMatched.get().count(ngh)) { 
            continue;
          }
          if(true == algo->dFrontier.get().insert(ngh).second) {
            dAdd2Frontier.push_back(ngh);
          }
        }
        for(auto ei = gD.in_edge_begin(*ri), ee = gD.in_edge_end(*ri); ei != ee; ++ei) {
          auto ngh = gD.getInEdgeDst(ei);
          if(algo->dMatched.get().count(ngh)) { 
            continue;
          }
          if(true == algo->dFrontier.get().insert(ngh).second) {
            dAdd2Frontier.push_back(ngh);
          }
        }
        algo->dFrontierSize.update(algo->dFrontier.get().size());

        doSearch(matching);
        if(currentlyFound.load() >= kFound) {
          return;
        }

        // restore data state
        algo->dMatched.get().erase(*ri);
        algo->dFrontier.get().insert(*ri);
        for(auto ii = dAdd2Frontier.begin(), ie = dAdd2Frontier.end(); ii != ie; ++ii) {
          algo->dFrontier.get().erase(*ii);
        }
        dAdd2Frontier.clear();
 
        // remove (nQ, *ri) from matching
        matching.pop_back();
      }

      // restore query state
      algo->qMatched.get().erase(nQ);
      algo->qFrontier.get().insert(nQ);
      for(auto ii = qAdd2Frontier.begin(), ie = qAdd2Frontier.end(); ii != ie; ++ii) {
        algo->qFrontier.get().erase(*ii);
      }
      qAdd2Frontier.clear();
    }

    // Galois::for_each expects cxt
    void operator()(Matching& matching, Galois::UserContext<Matching>& cxt) {
      auto nQ = matching.begin()->nQ;
      algo->qMatched.get().insert(nQ);

      for(auto ei = gQ.edge_begin(nQ), ee = gQ.edge_end(nQ); ei != ee; ++ei) {
        auto ngh = gQ.getEdgeDst(ei);
        algo->qFrontier.get().insert(ngh);
      }
      for(auto ei = gQ.in_edge_begin(nQ), ee = gQ.in_edge_end(nQ); ei != ee; ++ei) {
        auto ngh = gQ.getInEdgeDst(ei);
        algo->qFrontier.get().insert(ngh);
      }

      auto nD = matching.begin()->nD;
      algo->dMatched.get().insert(nD);

      for(auto ei = gD.edge_begin(nD), ee = gD.edge_end(nD); ei != ee; ++ei) {
        auto ngh = gD.getEdgeDst(ei);
        algo->dFrontier.get().insert(ngh);
      }
      for(auto ei = gD.in_edge_begin(nD), ee = gD.in_edge_end(nD); ei != ee; ++ei) {
        auto ngh = gD.getInEdgeDst(ei);
        algo->dFrontier.get().insert(ngh);
      }
      algo->dFrontierSize.update(algo->dFrontier.get().size());

      doSearch(matching);

      algo->qMatched.get().clear();
      algo->qFrontier.get().clear();
      algo->dMatched.get().clear();
      algo->dFrontier.get().clear();

      if(currentlyFound.load() >= kFound) {
        cxt.breakLoop();
      }
    }
  };

  void subgraphSearch(DGraph& gD, QGraph& gQ, MatchingVector& report) {
    MatchingVector works;
    Matching matching;

    // parallelize the search for candidates of gQ.begin()
    auto nQ = *(gQ.begin());
    auto& dQ = gQ.getData(nQ);
    for(auto ci = dQ.candidate.begin(), ce = dQ.candidate.end(); ci != ce; ++ci) {
      matching.push_back(NodeMatch(nQ, *ci));
      works.push_back(matching);
      matching.pop_back();
    }

    Galois::for_each_local(works, SubgraphSearchInternal(gD, gQ, report, this), Galois::loopname("search_for_each"));
    std::cout << "max size for dFrontier is " << dFrontierSize.reduce() << std::endl; 
  }
};

struct UllmannAlgo {
  std::string name() const { return "Ullmann"; }

  struct FilterCandidatesInternal {
    DGraph& gD;
    QGraph& gQ;
    Galois::GReduceLogicalOR& nodeEmpty;
    FilterCandidatesInternal(DGraph& d, QGraph& q, Galois::GReduceLogicalOR& lor): gD(d), gQ(q), nodeEmpty(lor) {}

    void operator()(const QGNode n) const {
      auto& dQ = gQ.getData(n);

      for(auto di = gD.begin(), de = gD.end(); di != de; ++di) {
        auto& dD = gD.getData(*di);

        if(dQ.label != dD.label) {
          continue;
        }

        // self loop for n but not for *di
        if(gQ.findEdge(n, n) != gQ.edge_end(n) && gD.findEdge(*di, *di) == gD.edge_end(*di)) {
          continue;
        }

        dQ.candidate.push_back(*di);
      }

      nodeEmpty.update(dQ.candidate.empty());
    }
  };

  // return true if at least one node has an empty set of candidates
  bool filterCandidates(DGraph& gD, QGraph& gQ) {
    Galois::GReduceLogicalOR isSomeNodeEmpty;
    Galois::do_all_local(gQ, FilterCandidatesInternal(gD, gQ, isSomeNodeEmpty), Galois::loopname("filter"), Galois::do_all_steal<true>());
    return isSomeNodeEmpty.reduce();
  }

  QGNode nextQueryNode(QGraph& gQ, Matching& matching) {
    auto qi = gQ.begin();
    std::advance(qi, matching.size());
    return *qi;
  }

  void refineCandidates(DGraph& gD, QGraph& gQ, QGNode nQuery, Matching& matching, std::vector<DGNode>& refined) {
    auto& dQ = gQ.getData(nQuery);
    auto numNghQ = std::distance(gQ.edge_begin(nQuery), gQ.edge_end(nQuery));
    auto numInNghQ = std::distance(gQ.in_edge_begin(nQuery), gQ.in_edge_end(nQuery));

    for(auto ii = dQ.candidate.begin(), ie = dQ.candidate.end(); ii != ie; ++ii) {
      auto numNghD = std::distance(gD.edge_begin(*ii), gD.edge_end(*ii));
      auto numInNghD = std::distance(gD.in_edge_begin(*ii), gD.in_edge_end(*ii));

      if(numNghD >= numNghQ && numInNghD >= numInNghQ) {
        refined.push_back(*ii);
      }
    }
  }

  bool isJoinable(DGraph& gD, QGraph& gQ, DGNode nD, QGNode nQ, Matching& matching) {
    for(auto mi = matching.begin(), me = matching.end(); mi != me; ++mi) {
      // nD is already matched
      if(nD == mi->nD) {
        return false;
      }

      // nQ => (mi->nQ) exists but not nD => (mi->nD)
      if(gQ.findEdge(nQ, mi->nQ) != gQ.edge_end(nQ) && gD.findEdge(nD, mi->nD) == gD.edge_end(nD)) {
        return false;
      }

      // (mi->nQ) => nQ exists but not (mi->nD) => nD
      // skip if both data and query graphs are directed
      if(!undirected) {
        if(gQ.findEdge(mi->nQ, nQ) != gQ.edge_end(mi->nQ) && gD.findEdge(mi->nD, nD) == gD.edge_end(mi->nD)) {
          return false;
        }
      }
    }

    return true;
  }

  struct SubgraphSearchInternal {
    typedef int tt_does_not_need_aborts;
    typedef int tt_does_not_need_push;
    typedef int tt_needs_parallel_break;
    typedef int tt_does_not_need_stats;

    DGraph& gD;
    QGraph& gQ;
    MatchingVector& report;
    UllmannAlgo *algo;
    SubgraphSearchInternal(DGraph& d, QGraph& q, MatchingVector& r, UllmannAlgo *algo): gD(d), gQ(q), report(r), algo(algo) {}

    void doSearch(Matching& matching) {
      if(currentlyFound.load() >= kFound) {
          return;
      }

      if(matching.size() == gQ.size()) {
        report.push_back(matching);
        currentlyFound += 1;
        return;
      }

      auto nQ = algo->nextQueryNode(gQ, matching);

      std::vector<DGNode> refined;
      algo->refineCandidates(gD, gQ, nQ, matching, refined);

      for(auto ri = refined.begin(), re = refined.end(); ri != re; ++ri) {
        if(!algo->isJoinable(gD, gQ, *ri, nQ, matching)) {
          continue;
        }

        // add (nQ, *ri) to matching 
        matching.push_back(NodeMatch(nQ, *ri));

        doSearch(matching);
        if(currentlyFound.load() >= kFound) {
          return;
        }

        // remove (nQ, *ri) from matching
        matching.pop_back();
      }
    }

    // Galois::for_each expects cxt
    void operator()(Matching& matching, Galois::UserContext<Matching>& cxt) {
      doSearch(matching);
      if(currentlyFound.load() >= kFound) {
        cxt.breakLoop();
      }
    }
  };

  void subgraphSearch(DGraph& gD, QGraph& gQ, MatchingVector& report) {
    MatchingVector works;
    Matching matching;

    // parallelize the search for candidates of gQ.begin()
    auto nQ = *(gQ.begin());
    auto& dQ = gQ.getData(nQ);
    for(auto ci = dQ.candidate.begin(), ce = dQ.candidate.end(); ci != ce; ++ci) {
      matching.push_back(NodeMatch(nQ, *ci));
      works.push_back(matching);
      matching.pop_back();
    }

    Galois::for_each_local(works, SubgraphSearchInternal(gD, gQ, report, this), Galois::loopname("search_for_each"));
  }
};

// check if the first matching is correct
void verifyMatching(Matching& matching, DGraph& gD, QGraph& gQ) {
  bool isFailed = false;

  for(auto m1 = matching.begin(), me = matching.end(); m1 != me; ++m1) {
    auto& dQ1 = gQ.getData(m1->nQ);
    auto& dD1 = gD.getData(m1->nD);

    if(dQ1.label != dD1.label) {
      isFailed = true;
      std::cerr << "label not match: gQ(" << dQ1.id << ") = " << dQ1.label;
      std::cerr << ", gD(" << dD1.id << ") = " << dD1.label << std::endl;
    }

    for(auto m2 = matching.begin(); m2 != me; ++m2) {
      auto& dQ2 = gQ.getData(m2->nQ);
      auto& dD2 = gD.getData(m2->nD);

      // two distinct query nodes map to the same data node
      if(m1->nQ != m2->nQ && m1->nD == m2->nD) {
        isFailed = true;
        std::cerr << "inconsistent mapping to data node: gQ(" << dQ1.id;
        std::cerr << ") to gD(" << dD1.id << "), gQ(" << dQ2.id;
        std::cerr << ") to gD(" << dD2.id << ")" << std::endl;
      }

      // a query node mapped to different data nodes
      if(m1->nQ == m2->nQ && m1->nD != m2->nD) {
        isFailed = true;
        std::cerr << "inconsistent mapping from query node: gQ(" << dQ1.id;
        std::cerr << ") to gD(" << dD1.id << "), gQ(" << dQ2.id;
        std::cerr << ") to gD(" << dD2.id << ")" << std::endl;
      }

      // query edge not matched to data edge
      if(gQ.findEdge(m1->nQ, m2->nQ) != gQ.edge_end(m1->nQ) && gD.findEdge(m1->nD, m2->nD) == gD.edge_end(m1->nD)) {
        isFailed = true;
        std::cerr << "edge not match: gQ(" << dQ1.id << " => " << dQ2.id;
        std::cerr << "), but no gD(" << dD1.id << " => " << dD2.id << ")" << std::endl;
      }
    }
  }

  if(isFailed) {
    GALOIS_DIE("Verification failed");
  } else {
    std::cout << "Verification succeeded" << std::endl;
  }
}

void reportMatchings(MatchingVector& report, DGraph& gD, QGraph& gQ) {
  auto output = std::ofstream("report.txt");
  auto ri = report.begin(), re = report.end();
  unsigned int i = 0; 
  while(ri != re) {
    output << i << ": { ";
    for(auto mi = ri->begin(), me = ri->end(); mi != me; ++mi) {
      output << "(" << gQ.getData(mi->nQ).id << ", " << gD.getData(mi->nD).id << ") ";
    }
    output << "}" << std::endl;
    ++ri;
    ++i;
  } 
  output.close();
}

template<typename Algo>
void run() {
  DGraph gD;
  if(graphD.size()) {
    Galois::Graph::readGraph(gD, graphD);
    std::cout << "Reading data graph..." << std::endl;
  } else {
    GALOIS_DIE("Failed to read data graph");
  }
  if(rndSeedDByTime) {
    rndSeedD = std::chrono::system_clock::now().time_since_epoch().count();
  }
  std::cout << "rndSeedD: " << rndSeedD << std::endl;
  initializeGraph(gD, rndSeedD);
  std::cout << "data graph initialized" << std::endl;
//  printGraph(gD);

  QGraph gQ;
  if(graphQ.size()) {
    Galois::Graph::readGraph(gQ, graphQ);
    std::cout << "Reading query graph..." << std::endl;
  } else {
    GALOIS_DIE("Failed to read query graph");
  }
  if(rndSeedQByTime) {
    rndSeedQ = std::chrono::system_clock::now().time_since_epoch().count();
  }
  std::cout << "rndSeedQ: " << rndSeedQ << std::endl;
  initializeGraph(gQ, rndSeedQ);
  std::cout << "query graph initialized" << std::endl;
//  printGraph(gQ);

  Algo algo;
  std::cout << "Running " << algo.name() << " Algorithm..." << std::endl;

  Galois::StatTimer T;
  T.start();

  Galois::StatTimer filterT("FilterCandidates");
  filterT.start();
  bool isSomeNodeUnmatched = algo.filterCandidates(gD, gQ);
  filterT.stop();

  if(isSomeNodeUnmatched) {
    T.stop();
    std::cout << "Some nodes have no candidates to match." << std::endl;
    return;
  }

  Galois::StatTimer searchT("SubgraphSearch");
  searchT.start();
  MatchingVector report;
  currentlyFound.store(0);
  algo.subgraphSearch(gD, gQ, report);
  searchT.stop();

  T.stop();
  std::cout << "Found " << currentlyFound << " instance(s) of the query graph." << std::endl;
  if(currentlyFound) {
    reportMatchings(report, gD, gQ);
    verifyMatching(*(report.begin()), gD, gQ);
  }
}

int main(int argc, char **argv) {
  Galois::StatManager statManager;
  LonestarStart(argc, argv, name, desc, url);

  Galois::StatTimer T("TotalTime");
  T.start();
  switch (algo) {
    case Algo::ullmann: run<UllmannAlgo>(); break;
    case Algo::vf2: run<VF2Algo>(); break;
    default: std::cerr << "Unknown algorithm\n"; abort();
  }
  T.stop();

  return 0;
}

