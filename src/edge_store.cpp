#include "edge_store.h"
#include "bucket.h"
#include "util.h"

#include <chrono>

// Constructor
EdgeStore::EdgeStore(size_t seed, node_id_t num_vertices, size_t sketch_bytes, size_t num_subgraphs, size_t start_subgraph)
    : seed(seed),
      num_vertices(num_vertices),
      num_subgraphs(num_subgraphs),
      adjlist(num_vertices),
      vertex_contracted(num_vertices, true),
      sketch_bytes(sketch_bytes) {
  num_edges = 0;
  adj_mutex = new std::mutex[num_vertices];

  cur_subgraph = start_subgraph;
  true_min_subgraph = start_subgraph;
}

EdgeStore::~EdgeStore() {
  delete[] adj_mutex;
}

// caller_first_es_subgraph is implied to be 0 when calling this function
TaggedUpdateBatch EdgeStore::insert_adj_edges(node_id_t src,
                                                   const std::vector<node_id_t> &dst_vertices) {
  int edges_delta = 0;
  std::vector<SubgraphTaggedUpdate> ret;
  node_id_t cur_first_es_subgraph;
  {
    std::lock_guard<std::mutex> lk(adj_mutex[src]);
    cur_first_es_subgraph = cur_subgraph;
    if (true_min_subgraph < cur_first_es_subgraph && !vertex_contracted[src]) {
      ret = vertex_contract(src);
    }

    for (auto dst : dst_vertices) {
      auto idx = concat_pairing_fn(src, dst);
      SubgraphTaggedUpdate data = {Bucket_Boruvka::get_index_depth(idx, seed, num_subgraphs), dst};

      if (cur_first_es_subgraph > 0) {
        ret.push_back(data); // add everything in dst_vertices to ret
        if (data.subgraph < cur_first_es_subgraph) continue; // skip stuff that shouldn't be added
      }

      if (!adjlist[src].insert(data).second) {
        // Current edge already exist, so delete
        if (adjlist[src].erase(data) == 0) {
          std::cerr << "ERROR: We found a duplicate but couldn't remove???" << std::endl;
          exit(EXIT_FAILURE);
        }
        edges_delta--;
      } else {
        edges_delta++;
      }
    }
  }
  num_edges += edges_delta;

  if (ret.size() == 0 && true_min_subgraph < cur_first_es_subgraph) {
    return vertex_advance_subgraph(cur_first_es_subgraph);
  } else {
    check_if_too_big();
    return {src, cur_first_es_subgraph - 1, cur_first_es_subgraph, ret};
  }
}

TaggedUpdateBatch EdgeStore::insert_adj_edges(node_id_t src, node_id_t caller_first_es_subgraph,
                                              const std::vector<SubgraphTaggedUpdate> &dst_data) {
  int edges_delta = 0;
  std::vector<SubgraphTaggedUpdate> ret;
  node_id_t cur_first_es_subgraph;
  {
    std::lock_guard<std::mutex> lk(adj_mutex[src]);
    cur_first_es_subgraph = cur_subgraph;
    if (true_min_subgraph < cur_first_es_subgraph && !vertex_contracted[src]) {
      ret = vertex_contract(src);
    }

    for (auto data : dst_data) {
      if (cur_first_es_subgraph > caller_first_es_subgraph) {
        ret.push_back(data); // add everything in dst_vertices to ret

        if (data.subgraph < cur_first_es_subgraph) continue; // skip stuff that shouldn't be added
      }

      if (!adjlist[src].insert(data).second) {
        // Current edge already exist, so delete
        if (adjlist[src].erase(data) == 0) {
          std::cerr << "ERROR: We found a duplicate but couldn't remove???" << std::endl;
          exit(EXIT_FAILURE);
        }
        edges_delta--;
      } else {
        edges_delta++;
      }
    }
  }
  num_edges += edges_delta;

  if (ret.size() == 0 && true_min_subgraph < cur_first_es_subgraph) {
    return vertex_advance_subgraph(cur_first_es_subgraph);
  } else {
    check_if_too_big();
    return {src, cur_first_es_subgraph - 1, cur_first_es_subgraph, ret};
  }
}

// IMPORTANT: We must have completed any pending contractions before we call this function
std::vector<Edge> EdgeStore::get_edges() {
  std::vector<Edge> ret;
  ret.reserve(num_edges);

  for (node_id_t src = 0; src < num_vertices; src++) {
    for (auto data : adjlist[src])
      ret.push_back({src, data.dst});
  }

  return ret;
}

#ifdef VERIFY_SAMPLES_F
void EdgeStore::verify_contract_complete() {
  for (size_t i = 0; i < num_vertices; i++) {
    std::lock_guard<std::mutex> lk(adj_mutex[i]);
    if (adjlist[i].size() == 0) continue;

    auto it = adjlist[i].begin();
    if (it->subgraph < cur_subgraph) {
      std::cerr << "ERROR: Found " << it->subgraph << ", " << it->dst << " which should have been deleted by contraction to " << cur_subgraph << std::endl;
      exit(EXIT_FAILURE);
    }
  }
  std::cerr << "Contraction verified!" << std::endl;
}
#endif

// the thread MUST hold the lock on src before calling this function
std::vector<SubgraphTaggedUpdate> EdgeStore::vertex_contract(node_id_t src) {
  std::vector<SubgraphTaggedUpdate> ret;
  // someone already contacted this vertex
  if (vertex_contracted[src])
    return ret;

  vertex_contracted[src] = true;

  if (adjlist[src].size() == 0) {
    return ret;
  }

  ret.reserve(adjlist[src].size());
  int edges_delta = 0;
  auto it_begin = adjlist[src].begin();
  auto it = it_begin;
  auto delete_it = it_begin;
  for (; it != adjlist[src].end(); it++) {
    if (it->subgraph < cur_subgraph) {
      delete_it++;
      edges_delta--;
    }
    ret.push_back(*it);
  }

  // now perform the deletion
  adjlist[src].erase(it_begin, delete_it);
  num_edges += edges_delta;
  return ret;
}

TaggedUpdateBatch EdgeStore::vertex_advance_subgraph(node_id_t cur_first_es_subgraph) {
  node_id_t src = 0;
  while (true) {
    src = needs_contraction.fetch_add(1);
    
    if (src >= num_vertices) {
      if (src == num_vertices) {
        std::lock_guard<std::mutex> lk(contract_lock);
#ifdef VERIFY_SAMPLES_F
        verify_contract_complete();
#endif
        ++true_min_subgraph;
        std::cerr << "EdgeStore: Contraction complete" << std::endl;
      }
      return {0, cur_first_es_subgraph - 1, cur_first_es_subgraph, std::vector<SubgraphTaggedUpdate>()};
    }

    std::lock_guard<std::mutex> lk(adj_mutex[src]);
    if (adjlist[src].size() > 0 && !vertex_contracted[src])
      break;

    vertex_contracted[src] = true;
  }

  std::lock_guard<std::mutex> lk(adj_mutex[src]);
  return {src, cur_first_es_subgraph - 1, cur_first_es_subgraph, vertex_contract(src)};
}

// checks if we should perform a contraction and begins the process if so
void EdgeStore::check_if_too_big() {
  if (num_edges * store_edge_bytes < sketch_bytes) {
    // no contraction needed
    return;
  }

  // we may need to perform a contraction
  {
    std::lock_guard<std::mutex> lk(contract_lock);
    if (true_min_subgraph < cur_subgraph) {
      // another thread already started contraction
      return;
    }

    for (node_id_t i = 0; i < num_vertices; i++) {
      vertex_contracted[i] = false;
    }
    needs_contraction = 0;
  }

  cur_subgraph++;

  std::cerr << "EdgeStore: Contracting to subgraphs " << cur_subgraph << " and above" << std::endl;
  std::cerr << "    num_edges = " << num_edges << std::endl;
  std::cerr << "    store_edge_bytes = " << store_edge_bytes << std::endl; 
  std::cerr << "    sketch_bytes = " << sketch_bytes << std::endl;

}
