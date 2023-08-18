#pragma once

#include <guttering_configuration.h>

// forward declaration
class Graph;

enum GutterSystem {
  GUTTERTREE,
  STANDALONE,
  CACHETREE
};

// Graph parameters
class GraphConfiguration {
private:
  // which guttering system to use for buffering updates
  GutterSystem _gutter_sys = STANDALONE;

  // Where to place on-disk datastructures
  std::string _disk_dir = ".";

  // Backup supernodes in memory or on disk when performing queries
  bool _backup_in_mem = true;

  // The number of graph workers
  size_t _num_graph_workers = 1;

  // Option to create more sketches than for standard connected components
  // Ex factor of 1.5, 1.5 times the sketches
  //    factor of 1, normal quantity of sketches
  double _sketches_factor = 1;

  // Size of update batches as relative to the size of a Supernode
  double _batch_factor = 1;

  // Configuration for the guttering system
  GutteringConfiguration _gutter_conf;

  friend class Graph;

public:
  GraphConfiguration() {};

  // setters
  GraphConfiguration& gutter_sys(GutterSystem gutter_sys);

  GraphConfiguration& disk_dir(std::string disk_dir);

  GraphConfiguration& backup_in_mem(bool backup_in_mem);

  GraphConfiguration& num_graph_workers(size_t num_groups);

  GraphConfiguration& sketches_factor(double factor);

  GraphConfiguration& batch_factor(double factor);

  GutteringConfiguration& gutter_conf();

  friend std::ostream& operator<< (std::ostream &out, const GraphConfiguration &conf);

  // no use of equal operator
  GraphConfiguration& operator=(const GraphConfiguration &) = delete;

  // moving and copying allowed
  GraphConfiguration(const GraphConfiguration &oth) = default;
  GraphConfiguration (GraphConfiguration &&) = default;

};
