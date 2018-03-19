/** Single source shortest paths -*- C++ -*-
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
 * Graph Simulation.
 *
 * @author Roshan Dathathri <roshan@cs.utexas.edu>
 * @author Yi-Shan Lu <yishanlu@cs.utexas.edu>
 */
#include "galois/Galois.h"
#include "galois/graphs/LCGraph.h"

#include <string>

//#include <vector>
//#include <unordered_map>
//typedef std::string KeyTy;
//typedef std::string ValTy;
//typedef std::unordered_map<KeyTy, ValTy> Attr;

struct Node {
  uint32_t label; // maximum of 32 node labels
  uint32_t id;
  uint64_t matched; // maximum of 64 nodes in the query graph
  // TODO: make matched a dynamic bitset
};

struct EdgeData {
  uint32_t label; // maximum  of 32 edge labels
  uint64_t timestamp; // range of timestamp is limited
  EdgeData(uint32_t l, uint64_t t) : label(l), timestamp(t) {}
};

struct MatchedNode {
  uint32_t id;
  const char* label;
  const char* name;
};

typedef galois::graphs::LC_CSR_Graph<Node, EdgeData>::with_no_lockable<true>::type::with_numa_alloc<true>::type Graph;
typedef Graph::GraphNode GNode;

void runGraphSimulation(Graph& queryGraph, Graph& dataGraph);
void reportGraphSimulation(Graph& queryGraph, Graph& dataGraph, std::string outputFile);

struct AttributedGraph {
  Graph graph;
  std::vector<std::string> nodeLabelNames; // maps ID to Name
  std::map<std::string, uint32_t> nodeLabelIDs; // maps Name to ID
  std::vector<std::string> edgeLabelNames; // maps ID to Name
  std::map<std::string, uint32_t> edgeLabelIDs; // maps Name to ID
  std::vector<std::string> nodeNames; // cannot use LargeArray because serialize does not do deep-copy
  // custom attributes
  std::map<std::string, std::vector<std::string>> nodeAttributes;
  std::map<std::string, std::vector<std::string>> edgeAttributes;
};

unsigned rightmostSetBitPos(uint32_t n);
void reportGraphSimulation(AttributedGraph& queryGraph, AttributedGraph& dataGraph, std::string outputFile);

void matchNodeWithRepeatedActions(Graph &graph, uint32_t nodeLabel, uint32_t action);
void matchNodeWithTwoActions(Graph &graph, uint32_t nodeLabel, uint32_t action1, uint32_t dstNodeLabel1, uint32_t action2, uint32_t dstNodeLabel2);

void matchNeighbors(Graph& graph, uint32_t uuid, uint32_t nodeLabel, uint32_t action, uint32_t neighborLabel);

size_t countMatchedNodes(Graph& graph);
size_t countMatchedNeighbors(Graph& graph, uint32_t uuid);

extern "C" {
void returnMatchedNodes(AttributedGraph& graph, MatchedNode* matchedNodes);
void reportMatchedNodes(AttributedGraph& graph, std::string outputFile);
void returnMatchedNeighbors(AttributedGraph& graph, uint32_t uuid, MatchedNode* matchedNeighbors);
void reportMatchedNeighbors(AttributedGraph& graph, uint32_t uuid, std::string outputFile);
} // extern "C"

