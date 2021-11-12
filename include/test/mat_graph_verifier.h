#pragma once
#include <set>
#include "../supernode.h"
#include "../dsu.h"
#include "graph_verifier.h"

/**
 * A plugin for the Graph class that runs Boruvka alongside the graph algorithm
 * and verifies the edges and connected components that the graph algorithm
 * generates. Takes a reference graph from a packed in-memory adjacency matrix.
 */
class MatGraphVerifier : public GraphVerifier {
  std::vector<std::set<node_t>> kruskal_ref;
  std::vector<std::set<node_t>> boruvka_cc;
  std::vector<bool>& det_graph;
  DisjointSetUnion<node_t> sets;

public:
  MatGraphVerifier(node_t n, std::vector<bool>& input);

  void verify_edge(Edge edge);
  void verify_cc(node_t node);
  void verify_soln(vector<set<node_t>>& retval);

  /**
   * Runs Kruskal's (deterministic) CC algo.
   * @param input_file the file to read input from.
   * @return an array of connected components.
   */
  static std::vector<std::set<node_t>> kruskal(node_t n, const std::vector<bool>& input);

  // Returns a unique identifier for each edge using diagonalization
  static inline uint64_t get_uid(node_t i, node_t j) {
    // swap i,j if necessary
    if (i > j) {
      std::swap(i,j);
    }
    node_t jm = j-1;
    if ((j & 1) == 0) j>>=1;
    else jm>>=1;
    j*=jm;
    return i+j;
  }

  static inline std::pair<node_id_t, node_id_t> inv_uid(node_t uid) {
    node_t eidx = 8*uid + 1;
    eidx = sqrt(eidx)+1;
    eidx/=2;
    node_id_t i,j = eidx;
    if ((j & 1) == 0) i = uid-(j>>1)*(j-1);
    else i = uid-j*((j-1)>>1);
    return {i, j};
  }
};