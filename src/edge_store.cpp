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
      max_edges(sketch_bytes / store_edge_bytes),
      default_buffer_allocation(max_edges / num_vertices) {
  num_edges = 0;
  adj_mutex = new std::mutex[num_vertices];

  cur_subgraph = start_subgraph;
  true_min_subgraph = start_subgraph;

  // reserve some space for each
  for (node_id_t i = 0; i < num_vertices; i++) {
    adjlist[i].reserve(default_buffer_allocation);
  }
#ifdef VERIFY_SAMPLES_F
  num_inserted = 0;
  num_duplicate = 0;
  num_returned = 0;
#endif
}

EdgeStore::~EdgeStore() {
  delete[] adj_mutex;
#ifdef VERIFY_SAMPLES_F
  std::cerr << "EdgeStore: Deconstructor" << std::endl;
  std::cerr << "    num_edges     = " << num_edges << std::endl;
  std::cerr << "    max_edges     = " << max_edges << std::endl;
  std::cerr << "    num_inserted  = " << num_inserted << std::endl;
  std::cerr << "    num_duplicate = " << num_duplicate << std::endl;
  std::cerr << "    num_returned  = " << num_returned << std::endl;
#endif
}

// caller_first_es_subgraph is implied to be 0 when calling this function
TaggedUpdateBatch EdgeStore::insert_adj_edges(node_id_t src,
                                                   const std::vector<node_id_t> &dst_vertices) {
  
  std::vector<SubgraphTaggedUpdate> tagged_updates;
  tagged_updates.resize(dst_vertices.size());
  for (node_id_t i = 0; i < dst_vertices.size(); i++) {
    node_id_t dst = dst_vertices[i];
    auto idx = concat_pairing_fn(src, dst);
    tagged_updates[i] = {Bucket_Boruvka::get_index_depth(idx, seed, num_subgraphs), dst};
  }
  return insert_adj_edges(src, 0, tagged_updates.data(), tagged_updates.size());
}

// TODO: Switch this over to take in a vector. Arrays are so Ohio
TaggedUpdateBatch EdgeStore::insert_adj_edges(node_id_t src, node_id_t caller_first_es_subgraph,
                                              SubgraphTaggedUpdate *dst_data,
                                              size_t dst_data_size) {
  std::vector<SubgraphTaggedUpdate> ret;
  if (dst_data_size == 0) return {src, cur_subgraph - 1, cur_subgraph, ret};
  node_id_t cur_first_es_subgraph;

#ifdef VERIFY_SAMPLES_F
  num_inserted += dst_data_size;
#endif


  // Sort the input data
  std::sort(dst_data, dst_data + dst_data_size);

  // merge the input data into the vertex buffer
  {
    std::lock_guard<std::mutex> lk(adj_mutex[src]);
    cur_first_es_subgraph = cur_subgraph;

    if (true_min_subgraph < cur_first_es_subgraph && !vertex_contracted[src]) {
      ret = vertex_contract(src);
    }

    auto &data_buffer = adjlist[src];
    size_t orig_size = data_buffer.size();

    size_t out_ptr = 0;
    size_t buffer_ptr = 0;
    size_t update_ptr = 0;

    // if the caller constructed the update buffer with bad info
    // copy the update buffer into ret
    if (caller_first_es_subgraph < cur_first_es_subgraph) {
      ret.insert(ret.end(), dst_data, dst_data + dst_data_size);
    }

    // place any updates that go to a smaller subgraph into ret
    while (dst_data[update_ptr].subgraph < cur_first_es_subgraph) {
      ++update_ptr;
    }

#ifdef VERIFY_SAMPLES_F
    num_returned += update_ptr;
    size_t local_ignored = update_ptr;
#endif

    // merge new updates in
    while (buffer_ptr < data_buffer.size()) {
      if (data_buffer[buffer_ptr] > dst_data[update_ptr]) {
        SubgraphTaggedUpdate temp = data_buffer[buffer_ptr];
        data_buffer[out_ptr] = dst_data[update_ptr];
        dst_data[update_ptr] = temp;
        ++buffer_ptr;
        ++out_ptr;
      }
      else if (data_buffer[buffer_ptr] < dst_data[update_ptr]) {
        data_buffer[out_ptr] = data_buffer[buffer_ptr];
        ++buffer_ptr;
        ++out_ptr;
      } else { // they are equal! Delete them both
        ++buffer_ptr;
        ++update_ptr;

#ifdef VERIFY_SAMPLES_F
        num_duplicate += 2;
        local_ignored += 2;
#endif

        // edge case introduced here. update_ptr can exceed bounds of dst_data
        if (update_ptr >= dst_data_size) break;
      }
    }

    // place all remaining updates into the buffer
    size_t new_size = out_ptr + (dst_data_size - update_ptr) + (orig_size - buffer_ptr);
    data_buffer.resize(std::max(new_size, orig_size));
    while (update_ptr < dst_data_size) {
      data_buffer[out_ptr++] = dst_data[update_ptr++];
    }
    while (buffer_ptr < orig_size) {
      data_buffer[out_ptr++] = data_buffer[buffer_ptr++];
    }
    data_buffer.resize(new_size);

#ifdef VERIFY_SAMPLES_F
    // verify sorted order
    SubgraphTaggedUpdate prev = data_buffer[0];
    for (size_t i = 1; i < data_buffer.size(); i++) {
      SubgraphTaggedUpdate data = data_buffer[i];
      if (data < prev || !(data < prev || prev < data)) {
        std::cerr << "ERROR: Buffer not sort good!" << std::endl;
        std::cerr << "cur " << i << " = {" << data.subgraph << "," << data.dst << "} should be >= ";
        std::cerr << "prev = {" << prev.subgraph << "," << prev.dst << "}" << std::endl;
        exit(EXIT_FAILURE);
      }
    }

    if (out_ptr != orig_size + dst_data_size - local_ignored) {
      std::cerr << "ERROR: Number of updates incorrect!" << std::endl;
      std::cerr << "Expected: " << orig_size + dst_data_size - local_ignored << std::endl;
      std::cerr << "Got: " << out_ptr << std::endl;

      std::cerr << "orig_size     = " << orig_size << std::endl;
      std::cerr << "dst_data_size = " << dst_data_size << std::endl;
      std::cerr << "local_ignored = " << local_ignored << std::endl;
      exit(EXIT_FAILURE);
    }
#endif

    num_edges += out_ptr - orig_size;
  }

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
    for (auto data : adjlist[src]) {
      ret.push_back({src, data.dst});
    }
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
  auto &data_buffer = adjlist[src];
  size_t orig_size = data_buffer.size();

  if (data_buffer.size() == 0) {
    return ret;
  }

  ret = data_buffer;
  size_t keep_idx = 0;

  for (size_t i = 0; i < data_buffer.size(); i++) {
    if (data_buffer[i].subgraph >= cur_subgraph) {
      data_buffer[keep_idx++] = data_buffer[i];
    }
  }

  data_buffer.resize(keep_idx);

  num_edges += data_buffer.size() - orig_size;
#ifdef VERIFY_SAMPLES_F
  num_returned += ret.size();
#endif
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
  if (num_edges < max_edges) {
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

    cur_subgraph++;
  }

#ifdef VERIFY_SAMPLES_F
  std::cerr << "EdgeStore: Contracting to subgraphs " << cur_subgraph << " and above" << std::endl;
  std::cerr << "    num_edges     = " << num_edges << std::endl;
  std::cerr << "    max_edges     = " << max_edges << std::endl;
  std::cerr << "    num_inserted  = " << num_inserted << std::endl;
  std::cerr << "    num_duplicate = " << num_duplicate << std::endl;
  std::cerr << "    num_returned  = " << num_returned << std::endl;
#endif
}
