#include <vector>
#include <cuda_xxhash64.cuh>
#include <graph.h>

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
    // List of node ids that thread will be responsible for updating 
    int *nodeUpdates;

    // List of edge ids that thread will be responsble for updating
    vec_t *edgeUpdates;

    // List of num updates for each node
    int *nodeNumUpdates;

    // List of starting index for each node's update
    int *nodeStartIndex;

    // Parameter for entire graph
    node_id_t num_nodes;
    size_t num_updates;
    
    // Parameter for each supernode (consistent with other supernodes)
    int num_sketches;
    
    // Parameter for each sketch (consistent with other sketches)
    size_t num_elems;
    size_t num_columns;
    size_t num_guesses;

    // Default Constructor of CudaUpdateParams
    CudaUpdateParams():nodeUpdates(nullptr), edgeUpdates(nullptr), nodeNumUpdates(nullptr), nodeStartIndex(nullptr) {};
    
    CudaUpdateParams(node_id_t num_nodes, size_t num_updates, int num_sketches, size_t num_elems, size_t num_columns, size_t num_guesses):
      num_nodes(num_nodes), num_updates(num_updates), num_sketches(num_sketches), num_elems(num_elems), num_columns(num_columns), num_guesses(num_guesses) {
      
      // Allocate memory space for GPU
      gpuErrchk(cudaMallocManaged(&nodeUpdates, 2 * num_updates * sizeof(node_id_t)));
      gpuErrchk(cudaMallocManaged(&edgeUpdates, 2 * num_updates * sizeof(vec_t)));
      gpuErrchk(cudaMallocManaged(&nodeNumUpdates, num_nodes * sizeof(int)));
      gpuErrchk(cudaMallocManaged(&nodeStartIndex, num_nodes * sizeof(int)));
    };
};

struct CudaQuery {
  vec_t non_zero;
  SampleSketchRet ret_code;
};

class CudaCCParams {
  public:
    // List of node ids that need to be sampled
    node_id_t* reps;

    // List of querys
    CudaQuery* query;

    // List of sketch ids for each thread
    int* sketchIds;

    // Number of remaining supernodes in a graph
    node_id_t num_nodes;

    // Parameter for each supernode (consistent with other supernodes)
    int num_sketches;

    // Parameter for each sketch (consistent with other sketches)
    size_t num_elems;
    size_t num_columns;
    size_t num_guesses;

    CudaCCParams(): reps(nullptr), query(nullptr), sketchIds(nullptr) {};

    CudaCCParams(node_id_t num_nodes, int num_sketches, size_t num_elems, size_t num_columns, size_t num_guesses): 
      num_nodes(num_nodes), num_sketches(num_sketches), num_elems(num_elems), num_columns(num_columns), num_guesses(num_guesses) {

      // Allocate memory space for GPU
      gpuErrchk(cudaMallocManaged(&reps, num_nodes * sizeof(node_id_t)));
      gpuErrchk(cudaMallocManaged(&query, num_nodes * sizeof(CudaQuery)));
      gpuErrchk(cudaMallocManaged(&sketchIds, num_nodes * sizeof(int)));
    };
};

/*
*   
*   Bucket Functions
*
*/

// Source: http://graphics.stanford.edu/~seander/bithacks.html#ZerosOnRightLinear
__device__ int ctzll(col_hash_t v) {
  int c;
  if (v) {
    v = (v ^ (v - 1)) >> 1;
    for (c = 0; v; c++) {
      v >>= 1;
    }
  }
  else {
    c = 8 * sizeof(v);
  }
  return c;
}

__device__ col_hash_t bucket_get_index_depth(const vec_t_cu update_idx, const long seed_and_col, const vec_hash_t max_depth) {
  // Update CUDA_XXH, confirm they are correct with xxhash_test.cu
  col_hash_t depth_hash = CUDA_XXH64(&update_idx, sizeof(vec_t), seed_and_col);
  depth_hash |= (1ull << max_depth); // assert not > max_depth by ORing

  return ctzll(depth_hash);
}

__device__ vec_hash_t bucket_get_index_hash(const vec_t update_idx, const long sketch_seed) {
  return CUDA_XXH64(&update_idx, sizeof(vec_t), sketch_seed);
}

__device__ bool bucket_is_good(const vec_t a, const vec_hash_t c, const long sketch_seed) {
  return c == bucket_get_index_hash(a, sketch_seed);
}

__device__ void bucket_update(vec_t_cu& a, vec_hash_t& c, const vec_t_cu& update_idx, const vec_hash_t& update_hash) {
  atomicXor(&a, update_idx);
  atomicXor(&c, update_hash);
}

/*
*   
*   Sketch's Update Functions
*
*/

// Version 6: Kernel code of handling all the stream updates
// Two threads will be responsible for one edge update -> one thread is only modifying one node's sketches.
// Placing sketches in shared memory, each thread is doing log n updates on one slice of sketch.
// Applying newest verison of sketch update function
__global__ void doubleStream_update(int* nodeUpdates, vec_t* edgeUpdates, int* nodeNumUpdates, int* nodeStartIndex, node_id_t num_nodes, size_t num_updates,
    int num_sketches, size_t num_elems, size_t num_columns, size_t num_guesses, CudaSketch* cudaSketches, long* sketchSeeds) {

  if (blockIdx.x < num_nodes){
    
    extern __shared__ vec_t_cu sketches[];
    vec_t_cu* bucket_a = sketches;
    vec_hash_t* bucket_c = (vec_hash_t*)&bucket_a[num_elems * num_sketches];
    int node = blockIdx.x;
    int startIndex = nodeStartIndex[node];

    // Each thread will initialize a bucket
    for (int i = threadIdx.x; i < num_sketches * num_elems; i += blockDim.x) {
      bucket_a[i] = 0;
      bucket_c[i] = 0;
    }

    __syncthreads();

    // Update node's sketches
    for (int id = threadIdx.x; id < nodeNumUpdates[node] + num_sketches; id += blockDim.x) {
      
      int sketch_offset = id % num_sketches;
      int update_offset = ((id / num_sketches) * num_sketches);
      
      for (int i = 0; i < num_sketches; i++) {

        if ((startIndex + update_offset + i) >= startIndex + nodeNumUpdates[node]) {
          break;
        }

        vec_hash_t checksum = bucket_get_index_hash(edgeUpdates[startIndex + update_offset + i], sketchSeeds[(node * num_sketches) + sketch_offset]);

        // Update depth 0 bucket
        bucket_update(bucket_a[(sketch_offset * num_elems) + num_elems - 1], bucket_c[(sketch_offset * num_elems) + num_elems - 1], edgeUpdates[startIndex + update_offset + i], checksum);

        // Update higher depth buckets
        for (unsigned j = 0; j < num_columns; ++j) {
          col_hash_t depth = bucket_get_index_depth(edgeUpdates[startIndex + update_offset + i], sketchSeeds[(node * num_sketches) + sketch_offset] + j*5, num_guesses);
          size_t bucket_id = j * num_guesses + depth;
          if(depth < num_guesses)
            bucket_update(bucket_a[(sketch_offset * num_elems) + bucket_id], bucket_c[(sketch_offset * num_elems) + bucket_id], edgeUpdates[startIndex + update_offset + i], checksum);
        }
      }
    }

    __syncthreads();

    // Each thread will trasfer a bucket back to global memory
    for (int i = threadIdx.x; i < num_sketches * num_elems; i += blockDim.x) {
        int sketch_offset = i / num_elems; 
        int elem_id = i % num_elems;

        CudaSketch curr_cudaSketch = cudaSketches[(node * num_sketches) + sketch_offset];

        vec_t_cu* curr_bucket_a = (vec_t_cu*)curr_cudaSketch.d_bucket_a;
        vec_hash_t* curr_bucket_c = curr_cudaSketch.d_bucket_c;

        curr_bucket_a[elem_id] = bucket_a[i];
        curr_bucket_c[elem_id] = bucket_c[i];
        
    }
  }
}

// Function that calls sketch update kernel code.
void sketchUpdate(int num_threads, int num_blocks, int num_updates, vec_t* update_indexes, CudaSketch* cudaSketches) {
  return;
}

// Function that calls stream update kernel code.
void streamUpdate(int num_threads, int num_blocks, CudaUpdateParams* cudaUpdateParams, CudaSketch* cudaSketches, long* sketchSeeds) {

  // Unwarp variables from cudaUpdateParams
  int *nodeUpdates = cudaUpdateParams[0].nodeUpdates;
  vec_t *edgeUpdates = cudaUpdateParams[0].edgeUpdates;
  int *nodeNumUpdates = cudaUpdateParams[0].nodeNumUpdates;
  int *nodeStartIndex = cudaUpdateParams[0].nodeStartIndex;

  node_id_t num_nodes = cudaUpdateParams[0].num_nodes;
  size_t num_updates = cudaUpdateParams[0].num_updates;
  
  int num_sketches = cudaUpdateParams[0].num_sketches;
  
  size_t num_elems = cudaUpdateParams[0].num_elems;
  size_t num_columns = cudaUpdateParams[0].num_columns;
  size_t num_guesses = cudaUpdateParams[0].num_guesses;

  int maxbytes = num_elems * num_sketches * sizeof(vec_t_cu) + num_elems * num_sketches * sizeof(vec_hash_t);
  
  // Increase maximum of dynamic shared memory size
  // Note: Only works if GPU has enough shared memory capacity to store sketches for each vertex
  cudaFuncSetAttribute(doubleStream_update, cudaFuncAttributeMaxDynamicSharedMemorySize, maxbytes);

  /*
    Note (Only when using shared memory): I have noticed that unwrapping variables within kernel code
      caused these parameter variables to stay within global memory, creating more latency. Therefore, unwrapping these 
      variables then passing as argument of the kernel code avoids that issue.
  */ 
  doubleStream_update<<<num_blocks, num_threads, maxbytes>>>(nodeUpdates, edgeUpdates, nodeNumUpdates, nodeStartIndex, num_nodes, num_updates, num_sketches, num_elems, num_columns, num_guesses, cudaSketches, sketchSeeds);

  cudaDeviceSynchronize();
}

/*
*   
*   Sketch's Query Functions
*
*/

__global__ void cuda_query(node_id_t* reps, CudaQuery* query, int* sketchIds, node_id_t num_nodes, int num_sketches, size_t num_elems, size_t num_columns, size_t num_guesses, CudaSketch* cudaSketches) {

  // Get thread id
  int tid = (blockIdx.x * blockDim.x) + threadIdx.x;

  if (tid < num_nodes) {
    int query_id = reps[tid];
    CudaSketch cudaSketch = cudaSketches[(query_id * num_sketches) + sketchIds[tid]];

    vec_t_cu* bucket_a = (vec_t_cu*)cudaSketch.d_bucket_a;
    vec_hash_t* bucket_c = cudaSketch.d_bucket_c;

    if (bucket_a[num_elems - 1] == 0 && bucket_c[num_elems - 1] == 0) {
      query[query_id] = {0, ZERO}; // the "first" bucket is deterministic so if all zero then no edges to return
      return;     
    }

    if (bucket_is_good(bucket_a[num_elems - 1], bucket_c[num_elems - 1], cudaSketch.seed)) {
      query[query_id] = {bucket_a[num_elems - 1], GOOD};
      return;      
    }

    for (unsigned i = 0; i < num_columns; ++i) {
      for (unsigned j = 0; j < num_guesses; ++j) {
        unsigned bucket_id = i * num_guesses + j;
        if (bucket_is_good(bucket_a[bucket_id], bucket_c[bucket_id], cudaSketch.seed)) {
          query[query_id] = {bucket_a[bucket_id], GOOD};
          return;          
        }
      }
    }
    query[query_id] = {0, FAIL};
  }
}

void cuda_sample_supernodes(int num_threads, int num_blocks, CudaCCParams* cudaCCParams, CudaSketch* cudaSketches) {
  // Unwarp variables from cudaCCParams
  node_id_t* reps = cudaCCParams[0].reps;
  CudaQuery* query = cudaCCParams[0].query;
  int* sketchIds = cudaCCParams[0].sketchIds;

  node_id_t num_nodes = cudaCCParams[0].num_nodes;

  int num_sketches = cudaCCParams[0].num_sketches;

  size_t num_elems = cudaCCParams[0].num_elems;
  size_t num_columns = cudaCCParams[0].num_columns;
  size_t num_guesses = cudaCCParams[0].num_guesses;

  // Call query kernel
  cuda_query<<<num_blocks, num_threads>>>(reps, query, sketchIds, num_nodes, num_sketches, num_elems, num_columns, num_guesses, cudaSketches);

  cudaDeviceSynchronize();
}