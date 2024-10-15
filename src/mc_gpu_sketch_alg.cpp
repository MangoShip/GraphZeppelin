#include "mc_gpu_sketch_alg.h"

#include <iostream>
#include <thread>
#include <vector>

// call this function after we have found the depth of each update
void MCGPUSketchAlg::complete_update_batch(int thr_id, const TaggedUpdateBatch &updates) {
  node_id_t min_subgraph = updates.min_subgraph;
  node_id_t first_es_subgraph = updates.first_es_subgraph;

  if (first_es_subgraph == 0) {
    std::cerr << "Why are we here??" << std::endl;
    throw std::runtime_error("gross");
  }

  // do we need to allocate more sketches due to edge_store contraction
  if (first_es_subgraph > cur_subgraphs) {
    sketch_creation_lock.lock();

    // double check to ensure no one else performed the allocation 
    if (first_es_subgraph > cur_subgraphs) {
      create_sketch_graph(cur_subgraphs, sketchParams);
      subgraphs[cur_subgraphs].initialize(this, cur_subgraphs, num_nodes, num_host_threads, num_device_threads, num_batch_per_buffer,
             sketchParams);
      cur_subgraphs++; // do this last so that threads only touch params/sketches when initialized
    }

    sketch_creation_lock.unlock();
  }

  node_id_t src_vertex = updates.src;
  auto &dsts_data = updates.dsts_data;

  size_t cur_pos = 0;

  while (cur_pos < dsts_data.size()) {
    // TODO: Make this memory allocation less sad
    // TODO: More accurately, probably want to use StandAloneGutters for each subgraph
    //       and directly insert to that instead of any buffering here. But I'm lazy.
    std::vector<std::vector<node_id_t>> update_buffers(max_sketch_graphs);
    node_id_t max_subgraph = 0;

    // limit amount we process here to a single batch
    size_t num_to_process = std::min(size_t(batch_size), dsts_data.size() - cur_pos);
    for (size_t i = cur_pos; i < cur_pos + num_to_process; i++) {
      auto &dst_data = dsts_data[i];
      node_id_t update_subgraphs = std::min(dst_data.subgraph, first_es_subgraph - 1);
      max_subgraph = std::max(update_subgraphs, max_subgraph);

      for (size_t graph_id = min_subgraph; graph_id <= update_subgraphs; graph_id++) {
        update_buffers[graph_id].push_back(dst_data.dst);
      }
    }
    cur_pos += num_to_process;

    for (size_t graph_id = 0; graph_id <= max_subgraph; graph_id++) {
      subgraphs[graph_id].apply_update_batch(thr_id, src_vertex, update_buffers[graph_id]);
    }
  }
}

void MCGPUSketchAlg::apply_update_batch(int thr_id, node_id_t src_vertex,
                                     const std::vector<node_id_t> &dst_vertices) {
  if (MCSketchAlg::get_update_locked()) throw UpdateLockedException();

  node_id_t first_es_subgraph = edge_store.get_first_store_subgraph();

  // We only have an adjacency list so just directly insert
  if (first_es_subgraph == 0) {
    TaggedUpdateBatch more_upds = edge_store.insert_adj_edges(src_vertex, dst_vertices);
    if (more_upds.dsts_data.size() > 0) complete_update_batch(thr_id, more_upds);
    return;
  }

  std::vector<SubgraphTaggedUpdate> &store_edges = store_buffers[thr_id];
  std::vector<SubgraphTaggedUpdate> &sketch_edges = sketch_buffers[thr_id];

  for (vec_t i = 0; i < dst_vertices.size(); i++) {
    // Determine the depth of current edge
    vec_t edge_id = static_cast<vec_t>(concat_pairing_fn(src_vertex, dst_vertices[i]));
    size_t subgraph = Bucket_Boruvka::get_index_depth(edge_id, 0, num_subgraphs-1);

    if (subgraph >= first_es_subgraph) {
      // Adj. list
      store_edges.push_back({subgraph, dst_vertices[i]});
    }
    sketch_edges.push_back({subgraph, dst_vertices[i]});
  }

  // Perform adjacency list updates
  TaggedUpdateBatch more_upds =
      edge_store.insert_adj_edges(src_vertex, first_es_subgraph, store_edges);
  if (sketch_edges.size() > 0)
    complete_update_batch(thr_id, {src_vertex, 0, first_es_subgraph, sketch_edges});
  if (more_upds.dsts_data.size() > 0)
    complete_update_batch(thr_id, more_upds);

  store_edges.clear();
  sketch_edges.clear();
}

void MCGPUSketchAlg::apply_flush_updates() {

  // first ensure that all pending contractions are moved out of the edge store.
  while (edge_store.contract_in_progress()) {
    TaggedUpdateBatch more_upds =
        edge_store.vertex_advance_subgraph(edge_store.get_first_store_subgraph());

    if (more_upds.dsts_data.size() > 0) complete_update_batch(0, more_upds);
  }

  for (size_t graph_id = 0; graph_id < cur_subgraphs; graph_id++) {
    subgraphs[graph_id].flush();
  }
  

  // ensure streams have finished applying updates
  cudaDeviceSynchronize();
}

std::vector<Edge> MCGPUSketchAlg::get_adjlist_spanning_forests() {
  return edge_store.get_edges();
}
