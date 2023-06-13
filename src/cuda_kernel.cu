
#include <vector>
#include <cuda_xxhash64.cuh>
#include <graph.h>
#include <sketch.h>
#include "../include/cuda_kernel.cuh"

typedef unsigned long long int uint64_cu;
typedef uint64_cu vec_t_cu;

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

__device__ edge_id_t device_concat_pairing_fn(node_id_t i, node_id_t j) {
  // swap i,j if necessary
  if (i > j) {
    node_id_t temp = i;
    i = j;
    j = temp;
  }
  return ((edge_id_t)i << (sizeof(node_id_t) * 8)) | j;
}


/*
*   
*   Sketch's Update Functions
*
*/

__global__ void gtsStream_kernel(int stream_id, volatile int *edgeWriteEnabled, node_id_t src, vec_t* edgeUpdates, vec_t prev_offset, size_t update_size, node_id_t num_nodes,
    int num_sketches, size_t num_elems, size_t num_columns, size_t num_guesses, CudaSketch* cudaSketches, long* sketchSeeds) {
      
  extern __shared__ vec_t_cu sketches[];
  vec_t_cu* bucket_a = sketches;
  vec_hash_t* bucket_c = (vec_hash_t*)&bucket_a[num_elems * num_sketches];

  // Each thread will initialize a bucket
  for (int i = threadIdx.x; i < num_sketches * num_elems; i += blockDim.x) {
    bucket_a[i] = 0;
    bucket_c[i] = 0;
  }

  __syncthreads();

  // Update node's sketches
  for (int id = threadIdx.x; id < update_size + num_sketches; id += blockDim.x) {
    
    int sketch_offset = id % num_sketches;
    int update_offset = ((id / num_sketches) * num_sketches);
    
    for (int i = 0; i < num_sketches; i++) {

      if ((prev_offset + update_offset + i) >= prev_offset + update_size) {
        break;
      }

      vec_hash_t checksum = bucket_get_index_hash(edgeUpdates[prev_offset + update_offset + i], sketchSeeds[(src * num_sketches) + sketch_offset]);

      // Update depth 0 bucket
      bucket_update(bucket_a[(sketch_offset * num_elems) + num_elems - 1], bucket_c[(sketch_offset * num_elems) + num_elems - 1], edgeUpdates[prev_offset + update_offset + i], checksum);

      // Update higher depth buckets
      for (unsigned j = 0; j < num_columns; ++j) {
        col_hash_t depth = bucket_get_index_depth(edgeUpdates[prev_offset + update_offset + i], sketchSeeds[(src * num_sketches) + sketch_offset] + j*5, num_guesses);
        size_t bucket_id = j * num_guesses + depth;
        if(depth < num_guesses)
          bucket_update(bucket_a[(sketch_offset * num_elems) + bucket_id], bucket_c[(sketch_offset * num_elems) + bucket_id], edgeUpdates[prev_offset + update_offset + i], checksum);
      }
    }
  }

  __syncthreads();

  // Each thread will trasfer a bucket back to global memory
  for (int i = threadIdx.x; i < num_sketches * num_elems; i += blockDim.x) {
      int sketch_offset = i / num_elems; 
      int elem_id = i % num_elems;

      CudaSketch curr_cudaSketch = cudaSketches[(src * num_sketches) + sketch_offset];

      vec_t_cu* curr_bucket_a = (vec_t_cu*)curr_cudaSketch.d_bucket_a;
      vec_hash_t* curr_bucket_c = curr_cudaSketch.d_bucket_c;

      atomicXor(&curr_bucket_a[elem_id], bucket_a[i]);
      atomicXor(&curr_bucket_c[elem_id], bucket_c[i]);
  }

  __syncthreads();
  if (threadIdx.x == 0) {
    edgeWriteEnabled[stream_id] = 1;
  }
  
}

// Version 6: Kernel code of handling all the stream updates
// Two threads will be responsible for one edge update -> one thread is only modifying one node's sketches.
// Placing sketches in shared memory, each thread is doing log n updates on one slice of sketch.
// Applying newest verison of sketch update function
__global__ void doubleStream_update(vec_t* edgeUpdates, int* nodeNumUpdates, vec_t* nodeStartIndex, node_id_t num_nodes,
    int num_sketches, size_t num_elems, size_t num_columns, size_t num_guesses, CudaSketch* cudaSketches, long* sketchSeeds) {

  if (blockIdx.x < num_nodes){
    
    extern __shared__ vec_t_cu sketches[];
    vec_t_cu* bucket_a = sketches;
    vec_hash_t* bucket_c = (vec_hash_t*)&bucket_a[num_elems * num_sketches];
    int node = blockIdx.x;
    vec_t startIndex = nodeStartIndex[node];

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
    __syncthreads();
  }
}

// Function that calls sketch update kernel code.
void CudaKernel::gtsStreamUpdate(int num_threads, int num_blocks, int stream_id, node_id_t src, cudaStream_t stream, vec_t prev_offset, size_t update_size, CudaUpdateParams* cudaUpdateParams, CudaSketch* cudaSketches, long* sketchSeeds) {
  // Unwarp variables from cudaUpdateParams
  vec_t *edgeUpdates = cudaUpdateParams[0].edgeUpdates;
  volatile int *edgeWriteEnabled = cudaUpdateParams[0].edgeWriteEnabled;

  node_id_t num_nodes = cudaUpdateParams[0].num_nodes;
  
  int num_sketches = cudaUpdateParams[0].num_sketches;

  size_t num_elems = cudaUpdateParams[0].num_elems;
  size_t num_columns = cudaUpdateParams[0].num_columns;
  size_t num_guesses = cudaUpdateParams[0].num_guesses;

  int maxbytes = num_elems * num_sketches * sizeof(vec_t_cu) + num_elems * num_sketches * sizeof(vec_hash_t);

  cudaFuncSetAttribute(gtsStream_kernel, cudaFuncAttributeMaxDynamicSharedMemorySize, maxbytes);
  gtsStream_kernel<<<num_blocks, num_threads, maxbytes, stream>>>(stream_id, edgeWriteEnabled, src, edgeUpdates, prev_offset, update_size, num_nodes, num_sketches, num_elems, num_columns, num_guesses, cudaSketches, sketchSeeds);
}

// Function that calls stream update kernel code.
/*void CudaKernel::streamUpdate(int num_threads, int num_blocks, CudaUpdateParams* cudaUpdateParams, CudaSketch* cudaSketches, long* sketchSeeds) {

  // Unwarp variables from cudaUpdateParams
  vec_t *edgeUpdates = cudaUpdateParams[0].edgeUpdates;
  int *nodeNumUpdates = cudaUpdateParams[0].nodeNumUpdates;
  vec_t *nodeStartIndex = cudaUpdateParams[0].nodeStartIndex;

  node_id_t num_nodes = cudaUpdateParams[0].num_nodes;

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
  /*doubleStream_update<<<num_blocks, num_threads, maxbytes>>>(edgeUpdates, nodeNumUpdates, nodeStartIndex, num_nodes, num_sketches, num_elems, num_columns, num_guesses, cudaSketches, sketchSeeds);

  cudaDeviceSynchronize();
}*/

/*
*   
*   Sketch's Query Functions
*
*/

__device__ Edge cuda_inv_concat_pairing_fn(uint64_t idx) {
  uint8_t num_bits = sizeof(node_id_t) * 8;
  node_id_t j = idx & 0xFFFFFFFF;
  node_id_t i = idx >> num_bits;
  return {i, j};
}

__device__ node_id_t get_parent(node_id_t* parent, node_id_t node) {
  if (parent[node] == node) return node;
  return parent[node] = get_parent(parent, parent[node]);
}

__global__ void sketch_query(node_id_t* reps, CudaQuery* query, size_t* sample_idxs, node_id_t num_nodes, int num_sketches, size_t num_elems, size_t num_columns, size_t num_guesses, CudaSketch* cudaSketches) {

  // Get thread id
  int tid = (blockIdx.x * blockDim.x) + threadIdx.x;

  if (tid < num_nodes) {
    int query_id = reps[tid];
    CudaSketch cudaSketch = cudaSketches[(query_id * num_sketches) + sample_idxs[query_id]];

    vec_t_cu* bucket_a = (vec_t_cu*)cudaSketch.d_bucket_a;
    vec_hash_t* bucket_c = cudaSketch.d_bucket_c;

    if (bucket_a[num_elems - 1] == 0 && bucket_c[num_elems - 1] == 0) {
      query[query_id] = {cuda_inv_concat_pairing_fn(0), ZERO}; // the "first" bucket is deterministic so if all zero then no edges to return
      return;     
    }

    if (bucket_is_good(bucket_a[num_elems - 1], bucket_c[num_elems - 1], cudaSketch.seed)) {
      query[query_id] = {cuda_inv_concat_pairing_fn(bucket_a[num_elems - 1]), GOOD};
      return;      
    }

    for (unsigned i = 0; i < num_columns; ++i) {
      for (unsigned j = 0; j < num_guesses; ++j) {
        unsigned bucket_id = i * num_guesses + j;
        if (bucket_is_good(bucket_a[bucket_id], bucket_c[bucket_id], cudaSketch.seed)) {
          query[query_id] = {cuda_inv_concat_pairing_fn(bucket_a[bucket_id]), GOOD};
          return;          
        }
      }
    }
    query[query_id] = {cuda_inv_concat_pairing_fn(0), FAIL};
  }
}

void CudaKernel::cuda_sample_supernodes(int num_threads, int num_blocks, CudaCCParams* cudaCCParams, CudaSketch* cudaSketches) {
  // Unwarp variables from cudaCCParams
  node_id_t* reps = cudaCCParams[0].reps;
  CudaQuery* query = cudaCCParams[0].query;
  size_t* sample_idxs = cudaCCParams[0].sample_idxs;

  node_id_t num_nodes = cudaCCParams[0].num_nodes[0];

  int num_sketches = cudaCCParams[0].num_sketches;

  size_t num_elems = cudaCCParams[0].num_elems;
  size_t num_columns = cudaCCParams[0].num_columns;
  size_t num_guesses = cudaCCParams[0].num_guesses;

  // Call query kernel
  sketch_query<<<num_blocks, num_threads>>>(reps, query, sample_idxs, num_nodes, num_sketches, num_elems, num_columns, num_guesses, cudaSketches);

  cudaDeviceSynchronize();
}

__global__ void supernodes_to_merge(node_id_t* reps, node_id_t* temp_reps, CudaQuery* query, node_id_t* parent, node_id_t* size, CudaToMerge* to_merge, node_id_t* num_nodes, bool* modified) {
  // Get thread id
  int tid = (blockIdx.x * blockDim.x) + threadIdx.x;

  // Note: Have 1 thread to handle all workload (Temporary)
  if (tid == 0) {
    int temp_reps_id = 0;

    for (int i = 0; i < num_nodes[0]; i++) {
      int query_id = reps[i];

      // unpack query result
      Edge edge = query[query_id].edge;
      SampleSketchRet ret_code = query[query_id].ret_code;

      if (ret_code == ZERO) {
        continue;
      }
      else if (ret_code == FAIL) {
        modified[0] = true;
        temp_reps[temp_reps_id] = query_id;
        temp_reps_id++;
        continue;
      }
      else { // ret_code == GOOD
        // query dsu
        node_id_t a = get_parent(parent, edge.src);
        node_id_t b = get_parent(parent, edge.dst);
        if (a == b) continue;

        // make a the parent of b
        if (size[a] < size[b]) {
          node_id_t temp = a;
          a = b;
          b = temp;
        }
        parent[b] = a;
        size[a] += size[b];

        // add b and any of the nodes to merge with it to a's vector
        CudaToMerge a_merge = to_merge[a];
        CudaToMerge b_merge = to_merge[b];

        a_merge.children[a_merge.size[0]] = b;
        a_merge.size[0]++;

        // Fill b's children to a
        for (int j = 0; j < b_merge.size[0]; j++) {
          a_merge.children[a_merge.size[0]] = b_merge.children[j];
          a_merge.size[0]++;
          b_merge.children[j] = 0;
        }

        // Clear b
        b_merge.size[0] = 0;
        modified[0] = true;
      }
    }

    // remove nodes added to new_reps due to sketch failures that
    // did end up being able to merge after all
    int temp_reps_size = temp_reps_id;
    int reps_id = 0;

    for (int i = 0; i < temp_reps_size; i++) {
      node_id_t a = temp_reps[i];
      if (to_merge[a].size[0] == 0) {
        reps[reps_id] = a;
        reps_id++;
      }
    }

    for (int i = 0; i < num_nodes[1]; i++) {
      if (to_merge[i].size[0] != 0) {
        reps[reps_id] = i;
        reps_id++;
      }
    }

    num_nodes[0] = reps_id;
  }
}

void CudaKernel::cuda_supernodes_to_merge(int num_threads, int num_blocks, CudaCCParams* cudaCCParams) {
  // Unwarp variables from cudaCCParams
  node_id_t* reps = cudaCCParams[0].reps;
  node_id_t* temp_reps = cudaCCParams[0].temp_reps;
  CudaQuery* query = cudaCCParams[0].query;

  node_id_t* parent = cudaCCParams[0].parent;
  node_id_t* size = cudaCCParams[0].size;

  CudaToMerge* to_merge = cudaCCParams[0].to_merge;

  node_id_t* num_nodes = cudaCCParams[0].num_nodes;

  bool* modified = cudaCCParams[0].modified;

  // Call supernodes_to_merge kernel
  supernodes_to_merge<<<num_blocks, num_threads>>>(reps, temp_reps, query, parent, size, to_merge, num_nodes, modified);
  cudaDeviceSynchronize();
}

__global__ void merge_supernodes(node_id_t* reps, CudaToMerge* to_merge, node_id_t* num_nodes, size_t* sample_idxs, size_t* merged_sketches, int num_sketches, size_t num_elems, CudaSketch* cudaSketches) {
  // Get thread id
  int tid = (blockIdx.x * blockDim.x) + threadIdx.x;

  if (tid < num_nodes[0]) {
    node_id_t a = reps[tid];

    // perform merging of nodes b into node a
    for (int i = 0; i < to_merge[a].size[0]; i++) {
      node_id_t b = to_merge[a].children[i];

      if (sample_idxs[b] > sample_idxs[a]) {
        sample_idxs[a] = sample_idxs[b];
      }
      if (merged_sketches[b] < merged_sketches[a]) {
        merged_sketches[a] = merged_sketches[b];
      }
      
      // Merge sketches
      for (int j = sample_idxs[a]; j < merged_sketches[a]; ++j) {
        CudaSketch cudaSketch1 = cudaSketches[(a * num_sketches) + j];
        CudaSketch cudaSketch2 = cudaSketches[(b * num_sketches) + j];

        vec_t_cu* sketch1_bucket_a = (vec_t_cu*)cudaSketch1.d_bucket_a;
        vec_hash_t* sketch1_bucket_c = cudaSketch1.d_bucket_c;

        vec_t_cu* sketch2_bucket_a = (vec_t_cu*)cudaSketch2.d_bucket_a;
        vec_hash_t* sketch2_bucket_c = cudaSketch2.d_bucket_c;

        if(sketch2_bucket_a[num_elems - 1] == 0 && sketch2_bucket_c[num_elems - 1] == 0) {
          continue;
        }
        for (int k = 0; k < num_elems; k++) {
          sketch1_bucket_a[k] ^= sketch2_bucket_a[k];
          sketch1_bucket_c[k] ^= sketch2_bucket_c[k];
        }
      }
  
    }
  }
}

void CudaKernel::cuda_merge_supernodes(int num_threads, int num_blocks, CudaCCParams* cudaCCParams, CudaSketch* cudaSketches) {
  // Unwarp variables from cudaCCParams
  node_id_t* reps = cudaCCParams[0].reps;

  CudaToMerge* to_merge = cudaCCParams[0].to_merge;

  node_id_t* num_nodes = cudaCCParams[0].num_nodes;

  size_t* sample_idxs = cudaCCParams[0].sample_idxs;
  size_t* merged_sketches = cudaCCParams[0].merged_sketches;

  int num_sketches = cudaCCParams[0].num_sketches;

  size_t num_elems = cudaCCParams[0].num_elems;

  // Call supernodes_to_merge kernel
  merge_supernodes<<<num_blocks, num_threads>>>(reps, to_merge, num_nodes, sample_idxs, merged_sketches, num_sketches, num_elems, cudaSketches);
  cudaDeviceSynchronize();
}