#pragma once
#include <graph.h>
#include <sketch.h>
#include "../src/cuda_library.cu"

typedef unsigned long long int uint64_cu;
typedef uint64_cu vec_t_cu;

// CUDA API Check
// Source: https://stackoverflow.com/questions/14038589/what-is-the-canonical-way-to-check-for-errors-using-the-cuda-runtime-api
#define gpuErrchk(ans) { gpuAssert((ans), __FILE__, __LINE__); }
inline void gpuAssert(cudaError_t code, const char *file, int line, bool abort=true)
{
   if (code != cudaSuccess) 
   {
      fprintf(stderr,"GPUassert: %s %s %d\n", cudaGetErrorString(code), file, line);
      if (abort) exit(code);
   }
}

/*
*   
*   Helper Classes for sketches
*
*/

class CudaSketch {
  public:
    vec_t* d_bucket_a;
    vec_hash_t* d_bucket_c;

    uint64_t seed;

    // Default Constructor of CudaSketch
    CudaSketch():d_bucket_a(nullptr), d_bucket_c(nullptr) {};

    CudaSketch(vec_t* d_bucket_a, vec_hash_t* d_bucket_c, uint64_t seed): d_bucket_a(d_bucket_a), d_bucket_c(d_bucket_c), seed(seed) {};
};

class CudaUpdateParams {
  public:
    // List of edge ids that thread will be responsble for updating
    //node_id_t *edgeUpdates;
    vec_t *edgeUpdates;

    volatile int *edgeWriteEnabled;

    // Parameter for entire graph
    node_id_t num_nodes;
    vec_t num_updates;
    
    // Parameter for each supernode (consistent with other supernodes)
    int num_sketches;
    
    // Parameter for each sketch (consistent with other sketches)
    size_t num_elems;
    size_t num_columns;
    size_t num_guesses;

    int num_host_threads;
    int batch_size;

    // Default Constructor of CudaUpdateParams
    CudaUpdateParams():edgeUpdates(nullptr) {};
    
    CudaUpdateParams(node_id_t num_nodes, size_t num_updates, int num_sketches, size_t num_elems, size_t num_columns, size_t num_guesses, int num_host_threads, int batch_size):
      num_nodes(num_nodes), num_updates(num_updates), num_sketches(num_sketches), num_elems(num_elems), num_columns(num_columns), num_guesses(num_guesses), num_host_threads(num_host_threads), batch_size(batch_size) {
      
      // Allocate memory space for GPU
      gpuErrchk(cudaMallocManaged(&edgeUpdates, num_host_threads + num_host_threads * batch_size * sizeof(vec_t)));
      gpuErrchk(cudaMallocManaged(&edgeWriteEnabled, num_host_threads * sizeof(int)));

      for (int i = 0; i < num_host_threads; i++) {
        edgeWriteEnabled[i] = 1;
      }
      //gpuErrchk(cudaMallocManaged(&edgeUpdates, 2 * num_updates * sizeof(node_id_t)));
    };
};

struct CudaQuery {
  Edge edge;
  SampleSketchRet ret_code;
};

struct CudaToMerge {
  node_id_t* children;
  int* size;
};

class CudaCCParams {
  public:
    // List of node ids that need to be sampled
    node_id_t* reps;

    node_id_t* temp_reps;

    // List of querys
    CudaQuery* query;

    // List of parent of each node id
    node_id_t* parent;

    // List of parent of each node id
    node_id_t* size;

    // List of node ids to be merged
    CudaToMerge* to_merge;
    node_id_t* merge_children;
    int* merge_size;

    // Number of remaining supernodes in a graph
    // [0]: Current reps size
    // [1]: num_nodes of the graph
    node_id_t* num_nodes;

    // Indicate if supernode has been merged or not
    bool* modified; 

    // List of sample_idx and merged_sketches for all nodes
    size_t* sample_idxs;
    size_t* merged_sketches;

    // Parameter for each supernode (consistent with other supernodes)
    int num_sketches;

    // Parameter for each sketch (consistent with other sketches)
    size_t num_elems;
    size_t num_columns;
    size_t num_guesses;

    CudaCCParams(node_id_t total_nodes, int num_sketches, size_t num_elems, size_t num_columns, size_t num_guesses): 
      num_sketches(num_sketches), num_elems(num_elems), num_columns(num_columns), num_guesses(num_guesses) {

      gpuErrchk(cudaMallocManaged(&num_nodes, 2 * sizeof(node_id_t)));
      num_nodes[0] = total_nodes;
      num_nodes[1] = total_nodes;

      // Allocate memory space for GPU
      gpuErrchk(cudaMallocManaged(&reps, num_nodes[0] * sizeof(node_id_t)));
      gpuErrchk(cudaMallocManaged(&temp_reps, num_nodes[0] * sizeof(node_id_t)));
      gpuErrchk(cudaMallocManaged(&query, num_nodes[0] * sizeof(CudaQuery)));
      gpuErrchk(cudaMallocManaged(&parent, num_nodes[0] * sizeof(node_id_t)));
      gpuErrchk(cudaMallocManaged(&size, num_nodes[0] * sizeof(node_id_t)));

      gpuErrchk(cudaMallocManaged(&to_merge, num_nodes[0] * sizeof(CudaToMerge)));

      gpuErrchk(cudaMallocManaged(&modified, sizeof(bool)));
      gpuErrchk(cudaMallocManaged(&sample_idxs, num_nodes[0] * sizeof(size_t)));
      gpuErrchk(cudaMallocManaged(&merged_sketches, num_nodes[0] * sizeof(size_t)));

      gpuErrchk(cudaMallocManaged(&merge_children, num_nodes[0] * num_nodes[0] * sizeof(node_id_t)));
      memset(merge_children, 0, num_nodes[0] * num_nodes[0] * sizeof(node_id_t));

      gpuErrchk(cudaMallocManaged(&merge_size, num_nodes[0] * sizeof(int)));
      memset(merge_size, 0, num_nodes[0] * sizeof(int));

      for (size_t i = 0; i < num_nodes[0]; i++) {
        to_merge[i] = CudaToMerge{&merge_children[i * num_nodes[0]], &merge_size[i]};
      }
      modified[0] = false;
    };

    void reset() {
      for (size_t i = 0; i < num_nodes[0]; i++) {
        temp_reps[i] = 0;
        for (int j = 0; j < to_merge[i].size[0]; j++) {
          to_merge[i].children[j] = 0;
        }
        to_merge[i].size[0] = 0;
      }
    }
};

class CudaKernel {
  public:
    /*
    *   
    *   Sketch's Update Functions
    *
    */

    void gtsStreamUpdate(int num_threads, int num_blocks, int id, node_id_t src, cudaStream_t stream, vec_t prev_offset, size_t update_size, CudaUpdateParams* cudaUpdateParams, CudaSketch* cudaSketches, long* sketchSeeds);
    void streamUpdate(int num_threads, int num_blocks, CudaUpdateParams* cudaUpdateParams, CudaSketch* cudaSketches, long* sketchSeeds);

    /*
    *   
    *   Sketch's Query Functions
    *
    */

    void cuda_sample_supernodes(int num_threads, int num_blocks, CudaCCParams* cudaCCParams, CudaSketch* cudaSketches);
    void cuda_supernodes_to_merge(int num_threads, int num_blocks, CudaCCParams* cudaCCParams);
    void cuda_merge_supernodes(int num_threads, int num_blocks, CudaCCParams* cudaCCParams, CudaSketch* cudaSketches);
};