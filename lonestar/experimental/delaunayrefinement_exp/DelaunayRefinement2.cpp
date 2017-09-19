/** Delaunay refinement -*- C++ -*-
 * @file
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2012, The University of Texas at Austin. All rights reserved.
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
 * Refinement of an initial, unrefined Delaunay mesh to eliminate triangles
 * with angles < 30 degrees, using a variation of Chew's algorithm.
 *
 * @author Andrew Lenharth <andrewl@lenharth.org>
 * @author Gurbinder Gill <gill@cs.utexas.edu>
 */
#include "Mesh.h"
#include "Cavity.h"
#include "Verifier.h"

#include "Galois/Galois.h"
#include "Galois/ParallelSTL/ParallelSTL.h"
#include "Galois/Graphs/Bag.h"
#include "Galois/Timer.h"

#include "llvm/Support/CommandLine.h"
#include "Lonestar/BoilerPlate.h"

#include "Galois/WorkList/WorkListAlt.h"
#include "Galois/WorkList/WorkListDebug.h"

#include <iostream>
#include <string.h>
#include <cassert>

namespace cll = llvm::cl;

static const char* name = "Delaunay Mesh Refinement";
static const char* desc = "Refines a Delaunay triangulation mesh such that no angle in the mesh is less than 30 degrees\n";
static const char* url = "delaunay_mesh_refinement";

static cll::opt<std::string> filename(cll::Positional, cll::desc("<input file>"), cll::Required);

/*
struct Process : public Galois::Runtime::Lockable {
  Graphp   graph;

  typedef int tt_needs_per_iter_alloc;

  Process(Graphp g): graph(g) {}
  Process() {}

  void operator()(GNode node, Galois::UserContext<GNode>& ctx) {
    if (!graph->containsNode(node))
      return;

    Cavity cav(graph, ctx.getPerIterAlloc());
    cav.initialize(node);
    cav.build();
    cav.computePost();
    cav.update(node, ctx);
  }

  // serialization functions
  typedef int tt_has_serialize;
  void serialize(Galois::Runtime::SerializeBuffer& s) const {
    gSerialize(s,graph);
  }
  void deserialize(Galois::Runtime::DeSerializeBuffer& s) {
    gDeserialize(s,graph);
  }
};

struct Preprocess : public Galois::Runtime::Lockable {
  Graphp   graph;
  Galois::Graph::Bag<GNode>::pointer wl;

  Preprocess(Graphp g, Galois::Graph::Bag<GNode>::pointer wlg): graph(g), wl(wlg) {}
  Preprocess() {}

  void operator()(GNode item, Galois::UserContext<GNode>& ctx) const {
    if (graph->getData(item).isBad())
      wl->push(item);
  }

  // serialization functions
  typedef int tt_has_serialize;
  void serialize(Galois::Runtime::SerializeBuffer& s) const {
    gSerialize(s,graph,wl);
  }
  void deserialize(Galois::Runtime::DeSerializeBuffer& s) {
    gDeserialize(s,graph,wl);
  }
};

struct Verification : public Galois::Runtime::Lockable {
  Graphp   graph;

  Verification(Graphp g): graph(g) {}
  Verification() {}

  void operator()(GNode item, Galois::UserContext<GNode>& ctx) const {
    if (graph->getData(item).isBad()) {
      printf("Found bad triangle\n");
    }
  }

  //serialization functions
  typedef int tt_has_serialize;
  void serialize(Galois::Runtime::SerializeBuffer& s) const {
    gSerialize(s,graph);
  }
  void deserialize(Galois::Runtime::DeSerializeBuffer& s) {
    gDeserialize(s,graph);
  }
};

struct Prefetch : public Galois::Runtime::Lockable {
  Graphp   graph;

  Prefetch(Graphp g): graph(g) {}
  Prefetch() {}

  void operator()(GNode item, Galois::UserContext<GNode>& ctx) const {
    (void)graph->getData(item).isBad();
  }

  // serialization functions
  typedef int tt_has_serialize;
  void serialize(Galois::Runtime::SerializeBuffer& s) const {
    gSerialize(s,graph);
  }
  void deserialize(Galois::Runtime::DeSerializeBuffer& s) {
    gDeserialize(s,graph);
  }
};
*/

int main(int argc, char** argv) {

  LonestarStart(argc, argv, name, desc, url);
  Galois::StatManager statManager;

  //check the host id and initialise the network
  Galois::Runtime::getSystemNetworkInterface().start();
  //Galois::Runtime::setTrace(false);
  Graphp g;
  g = Graph::allocate();
  {
    Mesh m;
    m.read(g, filename.c_str());
  }

  //  std::cout << "start configuration: " << NThirdGraphSize(graph) << " total triangles, ";
  //  std::cout << Galois::ParallelSTL::count_if_local(graph, is_bad(graph)) << " bad triangles\n";
  //ThirdGraphSize(graph);

  // master_terminate();
  Galois::Runtime::getSystemNetworkInterface().terminate();

  return 0;
}