#include "sketch.h"

#include <cstring>
#include <iostream>
#include <vector>
#include <cassert>

Sketch::Sketch(node_id_t n, uint64_t seed, size_t _samples, size_t _cols) : seed(seed) {
  num_samples = _samples == 0 ? samples_gen(n) : _samples;
  cols_per_sample = _cols == 0 ? default_cols_per_sample : _cols;
  num_columns = num_samples * cols_per_sample;
  num_guesses = guess_gen(n);
  num_buckets = num_columns * num_guesses + 1; // plus 1 for deterministic bucket
  buckets = new Bucket[num_buckets];

  // initialize bucket values
  for (size_t i = 0; i < num_buckets; ++i) {
    buckets[i].alpha = 0;
    buckets[i].gamma = 0;
  }
}

Sketch::Sketch(node_id_t n, uint64_t seed, std::istream &binary_in, size_t _samples, size_t _cols)
    : seed(seed) {
  num_samples = _samples == 0 ? samples_gen(n) : _samples;
  cols_per_sample = _cols == 0 ? default_cols_per_sample : _cols;
  num_columns = num_samples * cols_per_sample;
  num_guesses = guess_gen(n);
  num_buckets = num_columns * num_guesses + 1; // plus 1 for deterministic bucket
  buckets = new Bucket[num_buckets];

  // Read the serialized Sketch contents
  binary_in.read((char *)buckets, num_buckets * sizeof(Bucket));
}

Sketch::Sketch(const Sketch &s) : seed(s.seed) {
  num_samples = s.num_samples;
  cols_per_sample = s.cols_per_sample;
  num_columns = s.num_columns;
  num_guesses = s.num_guesses;
  num_buckets = s.num_buckets;
  buckets = new Bucket[num_buckets];

  std::memcpy(buckets, s.buckets, num_buckets * sizeof(Bucket));
}

Sketch::~Sketch() { delete[] buckets; }

#ifdef L0_SAMPLING
void Sketch::update(const vec_t update_idx) {
  vec_hash_t checksum = Bucket_Boruvka::get_index_hash(update_idx, checksum_seed());

  // Update depth 0 bucket
  Bucket_Boruvka::updates(buckets[num_buckets - 1], update_idx, checksum);

  // Update higher depth buckets
  for (unsigned i = 0; i < num_columns; ++i) {
    col_hash_t depth = Bucket_Boruvka::get_index_depth(update_idx, column_seed(i), num_guesses);
    likely_if(depth < num_guesses) {
      for (col_hash_t j = 0; j <= depth; ++j) {
        size_t bucket_id = i * num_guesses + j;
        Bucket_Boruvka::update(buckets[bucket_id], update_idx, checksum);
      }
    }
  }
}
#else  // Use support finding algorithm instead. Faster but no guarantee of uniform sample.
void Sketch::update(const vec_t update_idx) {
  vec_hash_t checksum = Bucket_Boruvka::get_index_hash(update_idx, checksum_seed());

  // Update depth 0 bucket
  Bucket_Boruvka::update(buckets[num_buckets - 1], update_idx, checksum);

  // Update higher depth buckets
  for (unsigned i = 0; i < num_columns; ++i) {
    col_hash_t depth = Bucket_Boruvka::get_index_depth(update_idx, column_seed(i), num_guesses);
    size_t bucket_id = i * num_guesses + depth;
    likely_if(depth < num_guesses) {
      Bucket_Boruvka::update(buckets[bucket_id], update_idx, checksum);
    }
  }
}
#endif

void Sketch::zero_contents() {
  for (size_t i = 0; i < num_buckets; i++) {
    buckets[i].alpha = 0;
    buckets[i].gamma = 0;
  }
}

std::pair<vec_t, SampleSketchRet> Sketch::sample() {
  if (sample_idx >= num_samples) {
    throw OutOfQueriesException();
  }

  size_t idx = sample_idx++;
  size_t first_column = idx * cols_per_sample;

  if (buckets[num_buckets - 1].alpha == 0 && buckets[num_buckets - 1].gamma == 0)
    return {0, ZERO};  // the "first" bucket is deterministic so if all zero then no edges to return

  if (Bucket_Boruvka::is_good(buckets[num_buckets - 1], checksum_seed()))
    return {buckets[num_buckets - 1].alpha, GOOD};

  for (unsigned i = 0; i < cols_per_sample; ++i) {
    for (unsigned j = 0; j < num_guesses; ++j) {
      unsigned bucket_id = (i + first_column) * num_guesses + j;
      if (Bucket_Boruvka::is_good(buckets[bucket_id], checksum_seed()))
        return {buckets[bucket_id].alpha, GOOD};
    }
  }
  return {0, FAIL};
}

std::pair<std::unordered_set<vec_t>, SampleSketchRet> Sketch::exhaustive_sample() {
  // TODO!
  exit(EXIT_FAILURE);
}

void Sketch::merge(Sketch &other) {
  if (other.buckets[num_buckets-1].alpha == 0 && other.buckets[num_buckets-1].gamma == 0) {
    // other sketch is empty so just return
    return;
  }

  // perform the merge
  for (size_t i = 0; i < num_buckets; ++i) {
    buckets[i].alpha ^= other.buckets[i].alpha;
    buckets[i].gamma ^= other.buckets[i].gamma;
  }
}

void Sketch::merge_raw_bucket_buffer(vec_t *buckets, size_t start_sample, size_t num_samples) {
  // TODO!
  exit(EXIT_FAILURE);
}

void Sketch::serialize(std::ostream &binary_out) const {
  binary_out.write((char*) buckets, num_buckets * sizeof(Bucket));
}

bool operator==(const Sketch &sketch1, const Sketch &sketch2) {
  if (sketch1.num_buckets != sketch2.num_buckets || sketch1.seed != sketch2.seed) {
    std::cout << "sketch1 = " << sketch1 << std::endl;
    std::cout << "sketch2 = " << sketch2 << std::endl;
    return false;
  }

  for (size_t i = 0; i < sketch1.num_buckets; ++i) {
    if (sketch1.buckets[i].alpha != sketch2.buckets[i].alpha ||
        sketch1.buckets[i].gamma != sketch2.buckets[i].gamma) {
      std::cout << i << std::endl;
      std::cout << "sketch1 = " << sketch1 << std::endl;
      std::cout << "sketch2 = " << sketch2 << std::endl;
      return false;
    }
  }

  return true;
}

std::ostream &operator<<(std::ostream &os, const Sketch &sketch) {
  Bucket bkt = sketch.buckets[sketch.num_buckets - 1];
  bool good = Bucket_Boruvka::is_good(bkt, sketch.checksum_seed());
  vec_t a = bkt.alpha;
  vec_hash_t c = bkt.gamma;

  os << " a:" << a << " c:" << c << (good ? " good" : " bad") << std::endl;

  for (unsigned i = 0; i < sketch.num_columns; ++i) {
    for (unsigned j = 0; j < sketch.num_guesses; ++j) {
      unsigned bucket_id = i * sketch.num_guesses + j;
      Bucket bkt = sketch.buckets[bucket_id];
      vec_t a = bkt.alpha;
      vec_hash_t c = bkt.gamma;
      bool good = Bucket_Boruvka::is_good(bkt, sketch.checksum_seed());

      os << " a:" << a << " c:" << c << (good ? " good" : " bad") << std::endl;
    }
    os << std::endl;
  }
  return os;
}
