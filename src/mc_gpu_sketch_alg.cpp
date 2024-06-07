#include "mc_gpu_sketch_alg.h"

#include <iostream>
#include <thread>
#include <vector>

void MCGPUSketchAlg::apply_update_batch(int thr_id, node_id_t src_vertex,
                                        const std::vector<node_id_t>& dst_vertices) {
  if (MCSketchAlg::get_update_locked()) throw UpdateLockedException();

  // If trim enabled, perform sketch updates in CPU
  if (trim_enabled) {
    if (trim_graph_id < 0 || trim_graph_id >= num_graphs) {
      std::cout << "INVALID trim_graph_id: " << trim_graph_id << "\n";
    }

    if (subgraphs[trim_graph_id]->get_type() != SKETCH) {
      std::cout << "Current trim_graph_id isn't SKETCH data structure: " << trim_graph_id << "\n";
    }

    apply_update_batch_single_graph(thr_id, trim_graph_id, src_vertex, dst_vertices);
  }

  else {
    int stream_id = thr_id * stream_multiplier;
    int stream_offset = 0;
    while (true) {
      if (cudaStreamQuery(streams[stream_id + stream_offset].stream) == cudaSuccess) {
        // Update stream_id
        stream_id += stream_offset;

        // CUDA Stream is available, check if it has any delta sketch
        if (streams[stream_id].delta_applied == 0) {
          for (int graph_id = 0; graph_id < streams[stream_id].num_graphs; graph_id++) {
            if (subgraphs[graph_id]->get_type() != SKETCH) {
              break;
            }

            size_t bucket_offset = thr_id * num_buckets;
            for (size_t i = 0; i < num_buckets; i++) {
              delta_buckets[bucket_offset + i].alpha =
                  subgraphs[graph_id]
                      ->get_cudaUpdateParams()
                      ->h_bucket_a[(stream_id * num_buckets) + i];
              delta_buckets[bucket_offset + i].gamma =
                  subgraphs[graph_id]
                      ->get_cudaUpdateParams()
                      ->h_bucket_c[(stream_id * num_buckets) + i];
            }

            int prev_src = streams[stream_id].src_vertex;

            if (prev_src == -1) {
              std::cout << "Stream #" << stream_id << ": Shouldn't be here!\n";
            }

            // Apply the delta sketch
            apply_raw_buckets_update((graph_id * num_nodes) + prev_src,
                                     &delta_buckets[bucket_offset]);
          }
          streams[stream_id].delta_applied = 1;
          streams[stream_id].src_vertex = -1;
          streams[stream_id].num_graphs = -1;
        } else {
          if (streams[stream_id].src_vertex != -1) {
            std::cout << "Stream #" << stream_id
                      << ": not applying but has delta sketch: " << streams[stream_id].src_vertex
                      << " deltaApplied: " << streams[stream_id].delta_applied << "\n";
          }
        }
        break;
      }
      stream_offset++;
      if (stream_offset == stream_multiplier) {
        stream_offset = 0;
      }
    }

    int start_index = stream_id * batch_size;
    std::vector<int> sketch_update_size;
    std::vector<std::vector<node_id_t>> local_adj_list;
    sketch_update_size.assign(max_sketch_graphs, 0);
    local_adj_list.assign(num_graphs, std::vector<node_id_t>());
    int max_depth = 0;

    for (vec_t i = 0; i < dst_vertices.size(); i++) {
      // Determine the depth of current edge -- how many subgraphs it applies to
      vec_t edge_id = static_cast<vec_t>(concat_pairing_fn(src_vertex, dst_vertices[i]));
      int depth = Bucket_Boruvka::get_index_depth(edge_id, 0, num_graphs - 1);
      max_depth = std::max(depth, max_depth);

      for (int graph_id = 0; graph_id <= depth; graph_id++) {
        // Fill in both local buffer for sketch and adj list

        if (graph_id < max_sketch_graphs) {
          // Sketch Graphs
          subgraphs[graph_id]
              ->get_cudaUpdateParams()
              ->h_edgeUpdates[start_index + sketch_update_size[graph_id]] = edge_id;
          sketch_update_size[graph_id]++;
        }

        // Adj. list
        local_adj_list[graph_id].push_back(dst_vertices[i]);
      }
    }

    streams[stream_id].src_vertex = src_vertex;
    streams[stream_id].delta_applied = 0;
    streams[stream_id].num_graphs = max_depth + 1;

    // Go every subgraph and apply updates
    for (int graph_id = 0; graph_id <= max_depth; graph_id++) {
      if (graph_id >= max_sketch_graphs) {  // Fixed Adj. list
        subgraphs[graph_id]->insert_adj_edges(thr_id, src_vertex, local_adj_list[graph_id]);
      } else {
        if (subgraphs[graph_id]->get_type() == SKETCH) {  // Perform Sketch updates
          subgraphs[graph_id]->increment_num_sketch_updates(sketch_update_size[graph_id]);

          // Regular sketch updates
          CudaUpdateParams* cudaUpdateParams = subgraphs[graph_id]->get_cudaUpdateParams();
          cudaMemcpyAsync(&cudaUpdateParams->d_edgeUpdates[start_index],
                          &cudaUpdateParams->h_edgeUpdates[start_index],
                          sketch_update_size[graph_id] * sizeof(vec_t), cudaMemcpyHostToDevice,
                          streams[stream_id].stream);
          cudaKernel.k_sketchUpdate(
              num_device_threads, num_device_blocks, streams[stream_id].stream,
              cudaUpdateParams->d_edgeUpdates, start_index, sketch_update_size[graph_id],
              stream_id * num_buckets, cudaUpdateParams, cudaUpdateParams->d_bucket_a,
              cudaUpdateParams->d_bucket_c, sketchSeed);
          cudaMemcpyAsync(&cudaUpdateParams->h_bucket_a[stream_id * num_buckets],
                          &cudaUpdateParams->d_bucket_a[stream_id * num_buckets],
                          num_buckets * sizeof(vec_t), cudaMemcpyDeviceToHost,
                          streams[stream_id].stream);
          cudaMemcpyAsync(&cudaUpdateParams->h_bucket_c[stream_id * num_buckets],
                          &cudaUpdateParams->d_bucket_c[stream_id * num_buckets],
                          num_buckets * sizeof(vec_hash_t), cudaMemcpyDeviceToHost,
                          streams[stream_id].stream);
        } else {  // Perform Adj. list updates
          subgraphs[graph_id]->insert_adj_edges(thr_id, src_vertex, local_adj_list[graph_id]);

          // Check the size of adj. list after insertion
          double adjlist_bytes = subgraphs[graph_id]->get_num_updates() * adjlist_edge_bytes;

          // With size of current adj. list, it is more
          // space-efficient to convert into sketch graph
          if (adjlist_bytes > sketch_bytes) {
            if (subgraphs[graph_id]->try_acq_conversion()) {  // -- enforce only one thread converts
              // Init sketches
              create_sketch_graph(graph_id);

              // convert_sketch = graph_id;
              num_adj_graphs--;
              num_sketch_graphs++;

              subgraphs[graph_id]->set_type(SKETCH);
            }
          }
        }
      }
    }
  }
};

// Part of prepping a query.
// This function moves all edges in lossless representations into the sketch if the
// subgraph has a sketch. (We perform the conversion lazily)
void MCGPUSketchAlg::convert_adj_to_sketch() {
  if (num_sketch_graphs == 0) {
    return;
  }

  int num_threads = num_host_threads + num_reader_threads;

  if (num_threads > (num_host_threads * stream_multiplier)) {
    std::cout << "ERROR in convert_adj_to_sketch(): Not enough CUDA Streams!\n";
    std::cout << "  # of CUDA Streams: " << num_host_threads * stream_multiplier << "\n";
    std::cout << "  # of threasd being used in this function: " << num_threads << "\n";
  }

  std::cout << "Converting adj.list graphs (" << num_sketch_graphs << ") into sketch graphs\n";
  std::vector<std::chrono::duration<double>> indiv_conversion_time;
  auto conversion_start = std::chrono::steady_clock::now();

  // Allocate buffer with the max. batch size
  vec_t *convert_h_edgeUpdates, *convert_d_edgeUpdates;

  gpuErrchk(cudaMallocHost(&convert_h_edgeUpdates, num_threads * num_nodes * sizeof(vec_t)));
  gpuErrchk(cudaMalloc(&convert_d_edgeUpdates, num_threads * num_nodes * sizeof(vec_t)));

  for (node_id_t i = 0; i < num_threads * num_nodes; i++) {
    convert_h_edgeUpdates[i] = 0;
  }

  // delta buckets for conversion
  Bucket* convert_delta_buckets = new Bucket[num_buckets * num_threads];

  // task converts the lossless edges into sketches
  auto task = [&](int thr_id, int graph_id) {
    int stream_id = thr_id;
    int start_index = stream_id * num_nodes;
    size_t current_index = 0;

    auto adjlist = subgraphs[graph_id]->get_adjlist();
    if (adjlist.size() == 0) return;

    node_id_t cur_src = adjlist.begin()->src;

    subgraphs[graph_id]->increment_num_sketch_updates(adjlist.size());

    for (auto edge : adjlist) {
      if (edge.src != cur_src) {
        // apply the updates

        // Start sketch updates
        CudaUpdateParams* cudaUpdateParams = subgraphs[graph_id]->get_cudaUpdateParams();
        cudaMemcpyAsync(&convert_d_edgeUpdates[start_index], &convert_h_edgeUpdates[start_index],
                        current_index * sizeof(vec_t), cudaMemcpyHostToDevice,
                        streams[stream_id].stream);
        cudaKernel.k_sketchUpdate(
            num_device_threads, num_device_blocks, streams[stream_id].stream, convert_d_edgeUpdates,
            start_index, current_index, stream_id * num_buckets, cudaUpdateParams,
            cudaUpdateParams->convert_d_bucket_a, cudaUpdateParams->convert_d_bucket_c, sketchSeed);
        cudaMemcpyAsync(&cudaUpdateParams->convert_h_bucket_a[stream_id * num_buckets],
                        &cudaUpdateParams->convert_d_bucket_a[stream_id * num_buckets],
                        num_buckets * sizeof(vec_t), cudaMemcpyDeviceToHost,
                        streams[stream_id].stream);
        cudaMemcpyAsync(&cudaUpdateParams->convert_h_bucket_c[stream_id * num_buckets],
                        &cudaUpdateParams->convert_d_bucket_c[stream_id * num_buckets],
                        num_buckets * sizeof(vec_hash_t), cudaMemcpyDeviceToHost,
                        streams[stream_id].stream);

        // Wait until GPU kernel ends and transfer buckets back
        cudaStreamSynchronize(streams[stream_id].stream);

        // Apply delta sketch
        size_t bucket_offset = thr_id * num_buckets;
        for (size_t i = 0; i < num_buckets; i++) {
          convert_delta_buckets[bucket_offset + i].alpha =
              cudaUpdateParams->convert_h_bucket_a[(stream_id * num_buckets) + i];
          convert_delta_buckets[bucket_offset + i].gamma =
              cudaUpdateParams->convert_h_bucket_c[(stream_id * num_buckets) + i];
        }

        // Apply the delta sketch
        apply_raw_buckets_update((graph_id * num_nodes) + cur_src, &convert_delta_buckets[bucket_offset]);
        current_index = 0;
        cur_src = edge.src;
      }

      convert_h_edgeUpdates[start_index + current_index] = 
          static_cast<vec_t>(concat_pairing_fn(edge.src, edge.dst));
      current_index++;
    }
  };

  for (int graph_id = 0; graph_id < num_sketch_graphs; graph_id++) {
    std::cout << "Graph #" << graph_id << "\n";
    auto indiv_conversion_start = std::chrono::steady_clock::now();

    // TODO: Make this multi-threaded and divide up the edge updates between the threads
    // need to figure out how std::set iterators work
    // num_threads specifies the number of threads to use here
    std::vector<std::thread> threads;
    for (int i = 0; i < 1; i++) threads.emplace_back(task, i, graph_id);

    // wait for threads to finish
    for (int i = 0; i < 1; i++) threads[i].join();
    indiv_conversion_time.push_back(std::chrono::steady_clock::now() - indiv_conversion_start);
    std::cout << "Finished Graph #" << graph_id << "\n";
  }

  std::chrono::duration<double> conversion_time =
      std::chrono::steady_clock::now() - conversion_start;
  std::cout << "Finished converting adj.list graphs (" << num_sketch_graphs
            << ") into sketch graphs. Total Elpased time: " << conversion_time.count() << "\n";
  for (int graph_id = 0; graph_id < num_sketch_graphs; graph_id++) {
    std::cout << "  S" << graph_id << ": " << indiv_conversion_time[graph_id].count() << "\n";
  }
}

void MCGPUSketchAlg::apply_flush_updates() {
  for (int stream_id = 0; stream_id < num_host_threads * stream_multiplier; stream_id++) {
    if (streams[stream_id].delta_applied == 0) {
      for (int graph_id = 0; graph_id < streams[stream_id].num_graphs; graph_id++) {
        if (subgraphs[graph_id]->get_type() != SKETCH) {
          break;
        }

        for (size_t i = 0; i < num_buckets; i++) {
          delta_buckets[i].alpha = subgraphs[graph_id]
                                       ->get_cudaUpdateParams()
                                       ->h_bucket_a[(stream_id * num_buckets) + i];
          delta_buckets[i].gamma = subgraphs[graph_id]
                                       ->get_cudaUpdateParams()
                                       ->h_bucket_c[(stream_id * num_buckets) + i];
        }

        int prev_src = streams[stream_id].src_vertex;

        if (prev_src == -1) {
          std::cout << "Stream #" << stream_id << ": Shouldn't be here!\n";
        }

        // Apply the delta sketch
        apply_raw_buckets_update((graph_id * num_nodes) + prev_src, delta_buckets);
      }
      streams[stream_id].delta_applied = 1;
      streams[stream_id].src_vertex = -1;
      streams[stream_id].num_graphs = -1;
    }
  }
}

std::vector<Edge> MCGPUSketchAlg::get_adjlist_spanning_forests(int graph_id, int k) {
  if (subgraphs[graph_id]->get_type() == SKETCH) {
    std::cout << "Subgraph with graph_id: " << graph_id << " is Sketch graph!\n";
  }

  auto adjlist = subgraphs[graph_id]->get_adjlist();
  return std::vector<Edge>(adjlist.begin(), adjlist.end());
}
