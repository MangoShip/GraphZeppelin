#pragma once

#include <cmath>
#include <map>
#include "mc_sketch_alg.h"
#include "cuda_kernel.cuh"
#include "mc_subgraph.h"

class MCGPUSketchAlg : public MCSketchAlg {
private:
  MCSubgraph<MCGPUSketchAlg>** subgraphs;
  SketchParams sketchParams;

  CudaKernel cudaKernel;

  node_id_t num_nodes;
  int k;
  int sketches_factor;

  // Number of threads and thread blocks for CUDA kernel
  int num_device_threads = 1024;

  // Number of CPU's graph workers
  int num_host_threads;

  // Number of CPU threads that read edge stream
  int num_reader_threads;

  // Maximum number of edge updates in one batch
  int num_batch_per_buffer = 1080;

  // Number of subgraphs
  int num_graphs;
  
  // Number of subgraphs in sketch representation
  int max_sketch_graphs; // Max. number of subgraphs that can be in sketch graphs
  std::atomic<int> num_sketch_graphs;

  // Number of adj. list subgraphs
  int min_adj_graphs; // Number of subgraphs that will always be in adj. list
  std::atomic<int> num_adj_graphs;
  
  double sketch_bytes; // Bytes of sketch graph
  double adjlist_edge_bytes; // Bytes of one edge in adj. list

  // Indicate if trimming spanning forest, only apply sketch updates to one sketch subgraph
  bool trim_enabled;
  int trim_graph_id;

public:
  std::atomic<int> batch_sizes;
  MCGPUSketchAlg(node_id_t _num_nodes, size_t num_updates, int num_threads,
                               int _num_reader_threads, SketchParams _sketchParams,
                               int _num_graphs, int _min_adj_graphs, int _max_sketch_graphs, int _k,
                               double _sketch_bytes, double _adjlist_edge_bytes,
                               CCAlgConfiguration config)
    : MCSketchAlg(_num_nodes, _sketchParams.cudaUVM_enabled, _sketchParams.seed, _sketchParams.cudaUVM_buckets, _max_sketch_graphs, config) {
    // Start timer for initializing
    auto init_start = std::chrono::steady_clock::now();
    batch_sizes = 0;

    sketchParams = _sketchParams;
    num_nodes = _num_nodes;
    k = _k;
    sketches_factor = config.get_sketches_factor();
    num_host_threads = num_threads;
    num_reader_threads = _num_reader_threads;

    num_graphs = _num_graphs;

    max_sketch_graphs = _max_sketch_graphs;
    num_sketch_graphs = 0;

    min_adj_graphs = _min_adj_graphs;
    num_adj_graphs = num_graphs;

    sketch_bytes = _sketch_bytes;
    adjlist_edge_bytes = _adjlist_edge_bytes;

    // Create a bigger batch size to apply edge updates when subgraph is turning into sketch
    // representation
    std::cout << "Batch Size: " << get_desired_updates_per_batch() << "\n";

    int device_id = cudaGetDevice(&device_id);
    int device_count = 0;
    cudaGetDeviceCount(&device_count);
    cudaDeviceProp deviceProp;
    cudaGetDeviceProperties(&deviceProp, device_id);
    std::cout << "CUDA Device Count: " << device_count << "\n";
    std::cout << "CUDA Device ID: " << device_id << "\n";
    std::cout << "CUDA Device Number of SMs: " << deviceProp.multiProcessorCount << "\n"; 

    // Initialize all subgraphs
    subgraphs = new MCSubgraph<MCGPUSketchAlg>*[num_graphs];
    for (int graph_id = 0; graph_id < num_graphs; graph_id++) {
      if (graph_id < max_sketch_graphs) {  // subgraphs that can be turned into adj. list
        subgraphs[graph_id] = new MCSubgraph<MCGPUSketchAlg>(this, graph_id, num_nodes, num_host_threads, num_device_threads, num_batch_per_buffer,
                                             sketchParams, ADJLIST);
      } else {  // subgraphs that are always going to be in adj. list
        subgraphs[graph_id] = new MCSubgraph<MCGPUSketchAlg>(graph_id, num_nodes, num_host_threads, FIXED_ADJLIST);
      }
    }

    if (max_sketch_graphs > 0) {  // If max_sketch_graphs is 0, there will never be any sketch graphs
      // Calculate the num_buckets assigned to the last thread block
      /*size_t num_last_tb_buckets =
          (subgraphs[0]->get_cudaUpdateParams()->num_tb_columns[num_device_blocks - 1] *
           bkt_per_col) +
          1;

      // Set maxBytes for GPU kernel's shared memory
      size_t maxBytes =
          (num_last_tb_buckets * sizeof(vec_t_cu)) + (num_last_tb_buckets * sizeof(vec_hash_t));*/
      size_t maxBytes = (sketchParams.num_buckets * sizeof(vec_t_cu)) + (sketchParams.num_buckets * sizeof(vec_hash_t));
      cudaKernel.updateSharedMemory(maxBytes);
      std::cout << "Allocated Shared Memory of: " << maxBytes << "\n";
    }

    trim_enabled = false;
    trim_graph_id = -1;

    std::cout << "Finished MCGPUSketchAlg's Initialization\n";
    std::chrono::duration<double> init_time = std::chrono::steady_clock::now() - init_start;
    std::cout << "MCGPUSketchAlg's Initialization Duration: " << init_time.count() << std::endl;
  }

  /**
   * Update all the sketches for a node, given a batch of updates.
   * @param thr_id         The id of the thread performing the update [0, num_threads)
   * @param src_vertex     The vertex where the edges originate.
   * @param dst_vertices   A vector of destinations.
   */
  void apply_update_batch(int thr_id, node_id_t src_vertex,
                          const std::vector<node_id_t> &dst_vertices);

  void flush_buffers();
  void convert_adj_to_sketch();

  void print_subgraph_edges() {
    std::cout << "Number of inserted updates for each subgraph:\n";
    for (int graph_id = 0; graph_id < num_graphs; graph_id++) {
      if (subgraphs[graph_id]->get_type() == SKETCH) {
        std::cout << "  S" << graph_id << " (Sketch): " << subgraphs[graph_id]->get_num_updates() << "\n";
      }
      else {
        std::cout << "  S" << graph_id << " (Adj. list): " << subgraphs[graph_id]->get_num_updates() << "\n";
      }
    }
  }

  //void traverse_DFS(std::vector<Edge> *forest, int graph_id, node_id_t node_id, std::vector<int> *visited);
  std::vector<Edge> get_adjlist_spanning_forests(int graph_id, int k);
  int get_num_sketch_graphs() { return num_sketch_graphs; }
  int get_num_adj_graphs() { return num_adj_graphs; }

  void set_trim_enbled(bool enabled, int graph_id) {
    trim_enabled = enabled;
    trim_graph_id = graph_id;

    if (trim_graph_id < 0 || trim_graph_id > num_graphs) {
      std::cout << "INVALID trim_graph_id: " << trim_graph_id << "\n";
    }
  }
};