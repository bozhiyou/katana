/*
 * This file belongs to the Galois project, a C++ library for exploiting parallelism.
 * The code is being released under the terms of the 3-Clause BSD License (a
 * copy is located in LICENSE.txt at the top-level directory).
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

#include "galois/DistGalois.h"
#include "DistBenchStart.h"
#include "galois/gstl.h"
#include "galois/DReducible.h"
#ifdef __GALOIS_HET_ASYNC__
#include "galois/DTerminationDetector.h"
#endif
#include "galois/runtime/Tracer.h"

#ifdef __GALOIS_HET_CUDA__
#include "pagerank_pull_cuda.h"
struct CUDA_Context* cuda_ctx;
#endif

#include <iostream>
#include <limits>
#include <algorithm>
#include <vector>

constexpr static const char* const REGION_NAME = "PageRank";

/******************************************************************************/
/* Declaration of command line arguments */
/******************************************************************************/
namespace cll = llvm::cl;

static cll::opt<float> tolerance("tolerance",
                                 cll::desc("tolerance for residual"),
                                 cll::init(0.000001));
static cll::opt<unsigned int>
    maxIterations("maxIterations",
                  cll::desc("Maximum iterations: Default 1000"),
                  cll::init(1000));

/******************************************************************************/
/* Graph structure declarations + other initialization */
/******************************************************************************/

static const float alpha = (1.0 - 0.85);
struct NodeData {
  float value;
  std::atomic<uint32_t> nout;
  float residual;
  float delta;
};

galois::DynamicBitSet bitset_residual;
galois::DynamicBitSet bitset_nout;
uint32_t numThreadBlocks;

typedef galois::graphs::DistGraph<NodeData, void> Graph;
typedef typename Graph::GraphNode GNode;

#include "pagerank_pull_sync.hh"

/******************************************************************************/
/* Algorithm structures */
/******************************************************************************/

/* (Re)initialize all fields to 0 except for residual which needs to be 0.15
 * everywhere */
struct ResetGraph {
  const float& local_alpha;
  Graph* graph;

  ResetGraph(const float& _local_alpha, Graph* _graph)
      : local_alpha(_local_alpha), graph(_graph) {}

  void static go(Graph& _graph) {
    const auto& allNodes = _graph.allNodesRange();
#ifdef __GALOIS_HET_CUDA__
    if (personality == GPU_CUDA) {
      std::string impl_str("ResetGraph_" + (_graph.get_run_identifier()));
      galois::StatTimer StatTimer_cuda(impl_str.c_str(), REGION_NAME);
      StatTimer_cuda.start();
      ResetGraph_allNodes_cuda(alpha, cuda_ctx);
      StatTimer_cuda.stop();
    } else if (personality == CPU)
#endif
      galois::do_all(
          galois::iterate(allNodes.begin(), allNodes.end()),
          ResetGraph{alpha, &_graph}, galois::no_stats(),
          galois::loopname(_graph.get_run_identifier("ResetGraph").c_str()));
  }

  void operator()(GNode src) const {
    auto& sdata    = graph->getData(src);
    sdata.value    = 0;
    sdata.nout     = 0;
    sdata.delta    = 0;
    sdata.residual = local_alpha;
  }
};

#ifdef __GALOIS_HET_CUDA__
#if DIST_PER_ROUND_TIMER
void ReportThreadBlockWork(uint32_t iteration_num, std::string run_identifier, std::string tb_identifer){

	std::string str = get_thread_block_work_into_string(cuda_ctx);
	galois::runtime::reportParam(REGION_NAME, run_identifier, str);

	if (galois::runtime::getSystemNetworkInterface().ID == 0 && iteration_num == 0) {
		//Assumption: The number of thread blocks in all the iterations
		std::string num_thread_blocks = get_num_thread_blocks(cuda_ctx);
		galois::runtime::reportParam(REGION_NAME, tb_identifer, num_thread_blocks);
	}
}
#endif
#endif

struct InitializeGraph {
  Graph* graph;

  InitializeGraph(Graph* _graph) : graph(_graph) {}

  void static go(Graph& _graph) {
    // init graph
    ResetGraph::go(_graph);

    const auto& nodesWithEdges = _graph.allNodesWithEdgesRange();

#ifdef __GALOIS_HET_CUDA__
    if (personality == GPU_CUDA) {
      std::string impl_str("InitializeGraph_" + (_graph.get_run_identifier()));
      galois::StatTimer StatTimer_cuda(impl_str.c_str(), REGION_NAME);
      StatTimer_cuda.start();
      InitializeGraph_nodesWithEdges_cuda(cuda_ctx);
      StatTimer_cuda.stop();
    } else if (personality == CPU)
#endif
      // doing a local do all because we are looping over edges
      galois::do_all(galois::iterate(nodesWithEdges), InitializeGraph{&_graph},
                     galois::steal(), galois::no_stats(),
                     galois::loopname(
                         _graph.get_run_identifier("InitializeGraph").c_str()));

    _graph.sync<writeDestination, readAny, Reduce_add_nout,
                Bitset_nout>("InitializeGraph");
  }

  // Calculate "outgoing" edges for destination nodes (note we are using
  // the tranpose graph for pull algorithms)
  void operator()(GNode src) const {
    for (auto nbr : graph->edges(src)) {
      GNode dst   = graph->getEdgeDst(nbr);
      auto& ddata = graph->getData(dst);
      galois::atomicAdd(ddata.nout, (uint32_t)1);
      bitset_nout.set(dst);
    }
  }
};

struct PageRank_delta {
  const float& local_alpha;
  cll::opt<float>& local_tolerance;
  Graph* graph;

#ifdef __GALOIS_HET_ASYNC__
  using DGAccumulatorTy = galois::DGTerminator<unsigned int>;
#else
  using DGAccumulatorTy = galois::DGAccumulator<unsigned int>;
#endif

  DGAccumulatorTy& active_vertices;

  PageRank_delta(const float& _local_alpha, cll::opt<float>& _local_tolerance,
                 Graph* _graph, DGAccumulatorTy& _dga)
      : local_alpha(_local_alpha), local_tolerance(_local_tolerance),
        graph(_graph), active_vertices(_dga) {}

  void static go(Graph& _graph, DGAccumulatorTy& dga) {
    const auto& allNodes = _graph.allNodesRange();

#ifdef __GALOIS_HET_CUDA__
    if (personality == GPU_CUDA) {
      std::string impl_str("PageRank_" + (_graph.get_run_identifier()));
      galois::StatTimer StatTimer_cuda(impl_str.c_str(), REGION_NAME);
      StatTimer_cuda.start();
      unsigned int __retval = 0;
      PageRank_delta_allNodes_cuda(__retval, alpha, tolerance, cuda_ctx);
      dga += __retval;
      StatTimer_cuda.stop();
    } else if (personality == CPU)
#endif
      galois::do_all(galois::iterate(allNodes.begin(), allNodes.end()),
                     PageRank_delta{alpha, tolerance, &_graph, dga},
                     galois::no_stats(),
                     galois::loopname(
                         _graph.get_run_identifier("PageRank_delta").c_str()));
  }

  void operator()(GNode src) const {
    auto& sdata = graph->getData(src);
    sdata.delta = 0;

    if (sdata.residual > 0) {
      sdata.value += sdata.residual;
      if (sdata.residual > this->local_tolerance) {
        if (sdata.nout > 0) {
          sdata.delta = sdata.residual * (1 - local_alpha) / sdata.nout;
          active_vertices += 1;
        }
      }
      sdata.residual = 0;
    }
  }
};

// TODO: GPU code operator does not match CPU's operator (cpu accumulates sum
// and adds all at once, GPU adds each pulled value individually/atomically)
struct PageRank {
  Graph* graph;

#ifdef __GALOIS_HET_ASYNC__
  using DGAccumulatorTy = galois::DGTerminator<unsigned int>;
#else
  using DGAccumulatorTy = galois::DGAccumulator<unsigned int>;
#endif

  PageRank(Graph* _graph) : graph(_graph) {}

  void static go(Graph& _graph, DGAccumulatorTy& dga) {
    unsigned _num_iterations   = 0;
    const auto& nodesWithEdges = _graph.allNodesWithEdgesRange();

    // unsigned int reduced = 0;

    do {
      _graph.set_num_round(_num_iterations);
      dga.reset();
      PageRank_delta::go(_graph, dga);
      // reset residual on mirrors
      _graph.reset_mirrorField<Reduce_add_residual>();

#ifdef __GALOIS_HET_CUDA__
      if (personality == GPU_CUDA) {
        std::string impl_str("PageRank_" + (_graph.get_run_identifier()));
        galois::StatTimer StatTimer_cuda(impl_str.c_str(), REGION_NAME);
        StatTimer_cuda.start();
        PageRank_nodesWithEdges_cuda(cuda_ctx);
        StatTimer_cuda.stop();
#if DIST_PER_ROUND_TIMER
        std::string identifer(_graph.get_run_identifier("GPUThreadBlocksWork_Host", galois::runtime::getSystemNetworkInterface().ID));
        std::string tb_identifer(_graph.get_run_identifier("ThreadBlocks_Host", galois::runtime::getSystemNetworkInterface().ID));
        ReportThreadBlockWork(_num_iterations, identifer, tb_identifer);
        
#endif
      } else if (personality == CPU)
#endif
        galois::do_all(
            galois::iterate(nodesWithEdges), PageRank{&_graph}, galois::steal(),
            galois::no_stats(),
            galois::loopname(_graph.get_run_identifier("PageRank").c_str()));

#ifdef __GALOIS_HET_ASYNC__
      _graph.sync<writeSource, readDestination, Reduce_add_residual,
                  Bitset_residual, true>("PageRank");
#else
      _graph.sync<writeSource, readDestination, Reduce_add_residual,
                  Bitset_residual>("PageRank");
#endif

      galois::runtime::reportStat_Tsum(
          REGION_NAME, "NumWorkItems_" + (_graph.get_run_identifier()),
          (unsigned long)_graph.sizeEdges());

      ++_num_iterations;
    } while (
#ifndef __GALOIS_HET_ASYNC__
             (_num_iterations < maxIterations) &&
#endif
             dga.reduce(_graph.get_run_identifier()));

    galois::runtime::reportStat_Tmax(
        REGION_NAME, "NumIterations_" + std::to_string(_graph.get_run_num()),
        (unsigned long)_num_iterations);
  }

  // Pull deltas from neighbor nodes, then add to self-residual
  void operator()(GNode src) const {
    auto& sdata = graph->getData(src);

    for (auto nbr : graph->edges(src)) {
      GNode dst   = graph->getEdgeDst(nbr);
      auto& ddata = graph->getData(dst);

      if (ddata.delta > 0) {
        galois::add(sdata.residual, ddata.delta);

        bitset_residual.set(src);
      }
    }
  }
};

/******************************************************************************/
/* Sanity check operators */
/******************************************************************************/

// Gets various values from the pageranks values/residuals of the graph
struct PageRankSanity {
  cll::opt<float>& local_tolerance;
  Graph* graph;

  galois::DGAccumulator<float>& DGAccumulator_sum;
  galois::DGAccumulator<float>& DGAccumulator_sum_residual;
  galois::DGAccumulator<uint64_t>& DGAccumulator_residual_over_tolerance;

  galois::DGReduceMax<float>& max_value;
  galois::DGReduceMin<float>& min_value;
  galois::DGReduceMax<float>& max_residual;
  galois::DGReduceMin<float>& min_residual;

  PageRankSanity(
      cll::opt<float>& _local_tolerance, Graph* _graph,
      galois::DGAccumulator<float>& _DGAccumulator_sum,
      galois::DGAccumulator<float>& _DGAccumulator_sum_residual,
      galois::DGAccumulator<uint64_t>& _DGAccumulator_residual_over_tolerance,
      galois::DGReduceMax<float>& _max_value,
      galois::DGReduceMin<float>& _min_value,
      galois::DGReduceMax<float>& _max_residual,
      galois::DGReduceMin<float>& _min_residual)
      : local_tolerance(_local_tolerance), graph(_graph),
        DGAccumulator_sum(_DGAccumulator_sum),
        DGAccumulator_sum_residual(_DGAccumulator_sum_residual),
        DGAccumulator_residual_over_tolerance(
            _DGAccumulator_residual_over_tolerance),
        max_value(_max_value), min_value(_min_value),
        max_residual(_max_residual), min_residual(_min_residual) {}

  void static go(Graph& _graph, galois::DGAccumulator<float>& DGA_sum,
                 galois::DGAccumulator<float>& DGA_sum_residual,
                 galois::DGAccumulator<uint64_t>& DGA_residual_over_tolerance,
                 galois::DGReduceMax<float>& max_value,
                 galois::DGReduceMin<float>& min_value,
                 galois::DGReduceMax<float>& max_residual,
                 galois::DGReduceMin<float>& min_residual) {
    DGA_sum.reset();
    DGA_sum_residual.reset();
    max_value.reset();
    max_residual.reset();
    min_value.reset();
    min_residual.reset();
    DGA_residual_over_tolerance.reset();

#ifdef __GALOIS_HET_CUDA__
    if (personality == GPU_CUDA) {
      float _max_value;
      float _min_value;
      float _sum_value;
      float _sum_residual;
      uint64_t num_residual_over_tolerance;
      float _max_residual;
      float _min_residual;
      PageRankSanity_masterNodes_cuda(
          num_residual_over_tolerance, _sum_value, _sum_residual, _max_residual,
          _max_value, _min_residual, _min_value, tolerance, cuda_ctx);
      DGA_sum += _sum_value;
      DGA_sum_residual += _sum_residual;
      DGA_residual_over_tolerance += num_residual_over_tolerance;
      max_value.update(_max_value);
      max_residual.update(_max_residual);
      min_value.update(_min_value);
      min_residual.update(_min_residual);
    } else
#endif
    {
      galois::do_all(galois::iterate(_graph.masterNodesRange().begin(),
                                     _graph.masterNodesRange().end()),
                     PageRankSanity(tolerance, &_graph, DGA_sum,
                                    DGA_sum_residual,
                                    DGA_residual_over_tolerance, max_value,
                                    min_value, max_residual, min_residual),
                     galois::no_stats(), galois::loopname("PageRankSanity"));
    }

    float max_rank          = max_value.reduce();
    float min_rank          = min_value.reduce();
    float rank_sum          = DGA_sum.reduce();
    float residual_sum      = DGA_sum_residual.reduce();
    uint64_t over_tolerance = DGA_residual_over_tolerance.reduce();
    float max_res           = max_residual.reduce();
    float min_res           = min_residual.reduce();

    // Only node 0 will print data
    if (galois::runtime::getSystemNetworkInterface().ID == 0) {
      galois::gPrint("Max rank is ", max_rank, "\n");
      galois::gPrint("Min rank is ", min_rank, "\n");
      galois::gPrint("Rank sum is ", rank_sum, "\n");
      galois::gPrint("Residual sum is ", residual_sum, "\n");
      galois::gPrint("# nodes with residual over ", tolerance,
                     " (tolerance) is ", over_tolerance, "\n");
      galois::gPrint("Max residual is ", max_res, "\n");
      galois::gPrint("Min residual is ", min_res, "\n");
    }
  }

  /* Gets the max, min rank from all owned nodes and
   * also the sum of ranks */
  void operator()(GNode src) const {
    NodeData& sdata = graph->getData(src);

    max_value.update(sdata.value);
    min_value.update(sdata.value);
    max_residual.update(sdata.residual);
    min_residual.update(sdata.residual);

    DGAccumulator_sum += sdata.value;
    DGAccumulator_sum_residual += sdata.residual;

    if (sdata.residual > local_tolerance) {
      DGAccumulator_residual_over_tolerance += 1;
    }
  }
};

/******************************************************************************/
/* Main */
/******************************************************************************/

constexpr static const char* const name = "PageRank - Compiler Generated "
                                          "Distributed Heterogeneous";
constexpr static const char* const desc = "PageRank Residual Pull version on "
                                          "Distributed Galois.";
constexpr static const char* const url = 0;

int main(int argc, char** argv) {
  galois::DistMemSys G;
  DistBenchStart(argc, argv, name, desc, url);

  auto& net = galois::runtime::getSystemNetworkInterface();

  if (net.ID == 0) {
    galois::runtime::reportParam(REGION_NAME, "Max Iterations",
                                 (unsigned long)maxIterations);
    std::ostringstream ss;
    ss << tolerance;
    galois::runtime::reportParam(REGION_NAME, "Tolerance", ss.str());
  }

  galois::StatTimer StatTimer_total("TimerTotal", REGION_NAME);

  StatTimer_total.start();

#ifdef __GALOIS_HET_CUDA__
  Graph* hg = distGraphInitialization<NodeData, void, false>(&cuda_ctx);
#else
  Graph* hg = distGraphInitialization<NodeData, void, false>();
#endif

  bitset_residual.resize(hg->size());
  bitset_nout.resize(hg->size());

  galois::gPrint("[", net.ID, "] InitializeGraph::go called\n");

  InitializeGraph::go(*hg);
  galois::runtime::getHostBarrier().wait();

#ifdef __GALOIS_HET_ASYNC__
  galois::DGTerminator<unsigned int> PageRank_accum;
#else
  galois::DGAccumulator<unsigned int> PageRank_accum;
#endif

  galois::DGAccumulator<float> DGA_sum;
  galois::DGAccumulator<float> DGA_sum_residual;
  galois::DGAccumulator<uint64_t> DGA_residual_over_tolerance;
  galois::DGReduceMax<float> max_value;
  galois::DGReduceMin<float> min_value;
  galois::DGReduceMax<float> max_residual;
  galois::DGReduceMin<float> min_residual;

  for (auto run = 0; run < numRuns; ++run) {
    galois::gPrint("[", net.ID, "] PageRank::go run ", run, " called\n");
    std::string timer_str("Timer_" + std::to_string(run));
    galois::StatTimer StatTimer_main(timer_str.c_str(), REGION_NAME);

    StatTimer_main.start();
    PageRank::go(*hg, PageRank_accum);
    StatTimer_main.stop();

    // sanity check
    PageRankSanity::go(*hg, DGA_sum, DGA_sum_residual,
                       DGA_residual_over_tolerance, max_value, min_value,
                       max_residual, min_residual);

    if ((run + 1) != numRuns) {
#ifdef __GALOIS_HET_CUDA__
      if (personality == GPU_CUDA) {
        bitset_residual_reset_cuda(cuda_ctx);
        bitset_nout_reset_cuda(cuda_ctx);
      } else
#endif
      {
        bitset_residual.reset();
        bitset_nout.reset();
      }

      (*hg).set_num_run(run + 1);
      InitializeGraph::go(*hg);
      galois::runtime::getHostBarrier().wait();
    }
  }

  StatTimer_total.stop();

  // Verify
  if (verify) {
#ifdef __GALOIS_HET_CUDA__
    if (personality == CPU) {
#endif
      for (auto ii = (*hg).masterNodesRange().begin();
           ii != (*hg).masterNodesRange().end(); ++ii) {
        galois::runtime::printOutput("% %\n", (*hg).getGID(*ii),
                                     (*hg).getData(*ii).value);
      }
#ifdef __GALOIS_HET_CUDA__
    } else if (personality == GPU_CUDA) {
      for (auto ii = (*hg).masterNodesRange().begin();
           ii != (*hg).masterNodesRange().end(); ++ii) {
        galois::runtime::printOutput("% %\n", (*hg).getGID(*ii),
                                     get_node_value_cuda(cuda_ctx, *ii));
      }
    }
#endif
  }

  return 0;
}