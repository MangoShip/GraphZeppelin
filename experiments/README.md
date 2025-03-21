# GPUSketch Experiments

## How to Run Experiments


## Experiment Description

### Experiment List
1. k-connectivity space savings. Justification histogram.
2. k-connectivity querying algorithm (should we really do this one?)
3. hybrid sketch on minimum cut (space, ingestion rate, query latency)
  - space: w/ relatively small dataset compare and contrast methods while measuring RAM utilization every 5 seconds
  - ingestion rate: measure periodically (every 5 seconds) compare each method
  - query latency: Query each x% of way through the stream, construct a whisper graph
4. GPU vs CPU Experiments. Bottleneck finding (ask Mingun)
5. As sketch size increases how do bottlenecks change (ask again)
6. GPU Utilization and speed of light (ask Mingun)
7. Full system experiments for CC, k-conn, minimum cut on:
  - All kron datasets
  - ktree datasets
  - Real-world graph datasets
8. Comparison systems: Also run the following systems on the above datasets
  - VieCut
  - Landscape (all but ktree. Reuse paper results)
  - CuGraph

