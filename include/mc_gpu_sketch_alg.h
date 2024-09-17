#pragma once

#include <cmath>
#include <map>
#include "mc_sketch_alg.h"
#include "cuda_kernel.cuh"
#include "edge_store.h"

class SketchSubgraph {
 private:
  std::atomic<size_t> num_updates;
  CudaStream<MCGPUSketchAlg>** cuda_streams = nullptr;

  int num_streams;

 public:

  ~SketchSubgraph() {
    if (cuda_streams != nullptr) {
      for (int i = 0; i < num_streams; i++) {
        delete cuda_streams[i];
      }
      delete[] cuda_streams;
    }
  }

  void initialize(int num_host_threads) {
    num_updates = 0;
    num_streams = num_host_threads;
    cuda_streams = new CudaStream<MCGPUSketchAlg>*[num_host_threads];

    // TODO: Figure out what to do with this
    // Rewrite address for buckets 
    sketchParams = _sketchParams;
    if (sketchParams.cudaUVM_enabled) {
      sketchParams.cudaUVM_buckets =
          &sketchParams.cudaUVM_buckets[graph_id * num_nodes * sketchParams.num_buckets];
    }

    for (int i = 0; i < num_streams; i++) {
      // TODO: Where do parameters come from?
      cudaStreams[i] =
          new CudaStream<MCGPUSketchAlg>(sketching_alg, graph_id, num_nodes, num_device_threads,
                                         num_batch_per_buffer, sketchParams);
    }
  }


  void apply_update_batch(int thr_id, node_id_t src, const std::vector<node_id_t> &dst_vertices) {
    if (cuda_streams == nullptr) 
      throw std::runtime_error("ERROR: Cannot call apply_update_batch() on uninit sketch subgraph");
    num_updates += dst_vertices.size();
    cudaStreams[thr_id]->process_batch(src, dst_vertices);
  }

  void flush() {
    for (int thr_id = 0; thr_id < num_streams; thr_id++) {
      cudaStreams[thr_id]->flush_buffers();
    }
  }

  size_t get_num_updates() {
    return num_updates;
  }
};

class MCGPUSketchAlg : public MCSketchAlg {
private:
  SketchParams sketchParams;

  CudaKernel cudaKernel;

  node_id_t num_nodes;
  int k;
  int sketches_factor;

  // Maximum number of subgraphs
  size_t num_subgraphs;

  // Current number of initialized sketch subgraphs. Starts at 0.
  std::atomic<node_id_t> cur_subgraphs;

  // Number of threads and thread blocks for CUDA kernel
  int num_device_threads = 1024;

  // Number of CPU's graph workers
  int num_host_threads;

  // Number of CPU threads that read edge stream
  int num_reader_threads;

  // Maximum number of edge updates in one batch
  int num_batch_per_buffer = 1080;
  
  // Number of subgraphs in sketch representation
  int max_sketch_graphs; // Max. number of subgraphs that can be in sketch graphs

  // sketch subgraphs
  SketchSubgraph *subgraphs;

  // lossless edge storage
  EdgeStore edge_store;

  // TODO: DO WE NEED THIS?
  // Vector for storing information for each CUDA Stream
  std::vector<CudaStream> streams;

  CudaKernel cudaKernel;

  // helper functions for apply_update_batch()
  size_t get_and_apply_finished_stream(int thr_id);
  void complete_update_batch(int thr_id, const TaggedUpdateBatch &updates);

  std::mutex sketch_creation_lock;
public:
  MCGPUSketchAlg(node_id_t num_vertices, int num_threads, int _num_reader_threads,
                SketchParams _sketchParams, int _num_subgraphs,
                int _max_sketch_graphs, int _k, size_t _sketch_bytes, int _initial_sketch_graphs,
                CCAlgConfiguration config)
     : MCSketchAlg(num_vertices, _sketchParams.cudaUVM_enabled, _sketchParams.seed,
                   _sketchParams.cudaUVM_buckets, _max_sketch_graphs, config),
       edge_store(seed, num_vertices, _sketch_bytes, _num_subgraphs, _initial_sketch_graphs) {
    // Start timer for initializing
    auto init_start = std::chrono::steady_clock::now();

    sketchParams = _sketchParams;
    num_nodes = num_vertices;
    k = _k;
    sketches_factor = config.get_sketches_factor();
    num_host_threads = num_threads;
    num_reader_threads = _num_reader_threads;

    if (_max_sketch_graphs < _initial_sketch_graphs) {
      std::cerr << "ERROR: Cannot have initial sketch graphs > max sketch graphs" << std::endl;
      exit(EXIT_FAILURE);
    }
    num_subgraphs = _num_subgraphs;
    cur_subgraphs = _initial_sketch_graphs;
    max_sketch_graphs = _max_sketch_graphs;
    subgraphs = new SketchSubgraph[max_sketch_graphs];

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

    size_t maxBytes = (sketchParams.num_buckets * sizeof(vec_t_cu)) +
                      (sketchParams.num_buckets * sizeof(vec_hash_t));
    cudaKernel.updateSharedMemory(maxBytes);
    std::cout << "Allocated Shared Memory of: " << maxBytes << "\n";

    // Initialize Sketch Graphs
    for (int i = 0; i < cur_subgraphs; i++) {
      create_sketch_graph(i);
      subgraphs[i].initialize(num_host_threads);
    }

    std::chrono::duration<double> init_time = std::chrono::steady_clock::now() - init_start;
    std::cout << "MCGPUSketchAlg's Initialization Duration: " << init_time.count() << std::endl;
  }

  ~MCGPUSketchAlg() {
    for (size_t i = 0; i < cur_subgraphs; i++)
      delete subgraphs[i];
    delete[] subgraphs;
  }

  /**
   * Update all the sketches for a node, given a batch of updates.
   * @param thr_id         The id of the thread performing the update [0, num_threads)
   * @param src_vertex     The vertex where the edges originate.
   * @param dst_vertices   A vector of destinations.
   */
  void apply_update_batch(int thr_id, node_id_t src_vertex,
                          const std::vector<node_id_t> &dst_vertices);

  // Update with the delta sketches that haven't been applied yet.
  void apply_flush_updates();

  void print_subgraph_edges() {
    std::cout << "Number of inserted updates for each subgraph:\n";
    for (int graph_id = 0; graph_id < cur_subgraphs.load(); graph_id++) {
    std::cout << "  Sub-Graph " << graph_id << "(Sketch): " << subgraphs[graph_id].get_num_updates()
                << std::endl;
    }
    std::cout << "  Adjacency list:      " << edge_store.get_num_edges() << std::endl;
  }

  std::vector<Edge> get_adjlist_spanning_forests();
  int get_num_sketch_graphs() { return cur_subgraphs; }

  size_t get_num_adjlist_edges() { return edge_store.get_num_edges(); }
};
