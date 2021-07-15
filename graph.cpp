#include <map>
#include <iostream>
#include <buffer_tree.h>

#include "include/graph.h"
#include "include/util.h"
#include "include/graph_worker.h"

Graph::Graph(uint64_t num_nodes): num_nodes(num_nodes) {
#ifdef VERIFY_SAMPLES_F
  cout << "Verifying samples..." << endl;
#endif
  representatives = new set<Node>();
  supernodes = new Supernode*[num_nodes];
  parent = new Node[num_nodes];
  time_t seed = time(nullptr);
  for (Node i=0;i<num_nodes;++i) {
    representatives->insert(i);
    supernodes[i] = new Supernode(num_nodes,seed);
    parent[i] = i;
  }
  num_updates = 0; // REMOVE this later
  std::string buffer_loc_prefix = configure_system(); // read the configuration file to configure the system
#ifdef USE_FBT_F
  // Create buffer tree and start the graphWorkers
  bf = new BufferTree(buffer_loc_prefix, (1<<20), 16, num_nodes, GraphWorker::get_num_groups(), true);
  GraphWorker::start_workers(this, bf);
#else
  unsigned long node_size = 24*pow((log2(num_nodes)), 3);
  node_size /= sizeof(Node);
  wq = new WorkQueue(node_size, num_nodes, 2*GraphWorker::get_num_groups());
  GraphWorker::start_workers(this, wq);
#endif
}

Graph::~Graph() {
  for (unsigned i=0;i<num_nodes;++i)
    delete supernodes[i];
  delete[] supernodes;
  delete[] parent;
  delete representatives;
  GraphWorker::stop_workers(); // join the worker threads
#ifdef USE_FBT_F
  delete bf;
#else
  delete wq;
#endif
}

void Graph::update(GraphUpdate upd) {
  if (update_locked) throw UpdateLockedException();
  Edge &edge = upd.first;

#ifdef USE_FBT_F
  bf->insert(edge);
  std::swap(edge.first, edge.second);
  bf->insert(edge);
#else
  wq->insert(edge);
  std::swap(edge.first, edge.second);
  wq->insert(edge);
#endif
}


void Graph::batch_update(uint64_t src, const std::vector<uint64_t>& edges) {
  if (update_locked) throw UpdateLockedException();
  std::vector<vec_t> updates;
  updates.reserve(edges.size());
  for (const auto& edge : edges) {
    if (src < edge) {
      updates.push_back(static_cast<vec_t>(
          nondirectional_non_self_edge_pairing_fn(src, edge)));
    } else {
      updates.push_back(static_cast<vec_t>(
          nondirectional_non_self_edge_pairing_fn(edge, src)));
    }
    num_updates += 1; // REMOVE this later
  }
  supernodes[src]->batch_update(updates);
}

vector<set<Node>> Graph::connected_components() {
#ifdef USE_FBT_F
  bf->force_flush(); // flush everything in buffertree to make final updates
#else
  wq->force_flush();
#endif
  GraphWorker::pause_workers(); // wait for the workers to finish applying the updates
  // after this point all updates have been processed from the buffer tree

  printf("Total number of updates to sketches before CC %lu\n", num_updates.load()); // REMOVE this later
  update_locked = true; // disallow updating the graph after we run the alg
  bool modified;
#ifdef VERIFY_SAMPLES_F
  GraphVerifier verifier {cum_in};
#endif

  do {
    modified = false;
    vector<Node> removed;
    for (Node i: (*representatives)) {
      if (parent[i] != i) continue;
      boost::optional<Edge> edge = supernodes[i]->sample();
#ifdef VERIFY_SAMPLES_F
      if (edge.is_initialized())
        verifier.verify_edge(edge.value());
      else
        verifier.verify_cc(i);
#endif
      if (!edge.is_initialized()) continue;

      Node n;
      // DSU compression
      if (get_parent(edge->first) == i) {
        n = get_parent(edge->second);
        removed.push_back(n);
        parent[n] = i;
      }
      else {
        get_parent(edge->second);
        n = get_parent(edge->first);
        removed.push_back(n);
        parent[n] = i;
      }
      supernodes[i]->merge(*supernodes[n]);
    }
    if (!removed.empty()) modified = true;
    for (Node i : removed) representatives->erase(i);
  } while (modified);
  map<Node, set<Node>> temp;
  for (Node i=0;i<num_nodes;++i)
    temp[get_parent(i)].insert(i);
  vector<set<Node>> retval;
  retval.reserve(temp.size());
  for (const auto& it : temp) retval.push_back(it.second);
#ifdef VERIFY_SAMPLES_F
  verifier.verify_soln(retval);
#endif

  return retval;
}

vector<set<Node>> Graph::parallel_connected_components() {
#ifdef USE_FBT_F
  bf->force_flush(); // flush everything in buffertree to make final updates
#else
  wq->force_flush();
#endif
  GraphWorker::pause_workers(); // wait for the workers to finish applying the updates
  // after this point all updates have been processed from the buffer tree

  printf("Total number of updates to sketches before CC %lu\n", num_updates.load()); // REMOVE this later
  update_locked = true; // disallow updating the graph after we run the alg
  bool modified;
#ifdef VERIFY_SAMPLES_F
  throw UnableToVerifyException();
#endif
  Node query[num_nodes];
  Node size[num_nodes];
  fill(size, size + num_nodes, 1);
  do {
    modified = false;
    #pragma omp parallel for default(none) shared(representatives, query)
    for (auto it = (*representatives).begin(); it != (*representatives).end(); ++it) { // this must be a canonical for loop for OpenMP to work!
      auto edge = supernodes[*it]->sample();
      if (!edge.is_initialized()) continue;
      query[*it] = edge->first ^ edge->second ^ *it;
    }

    vector<Node> to_remove;
    for (Node i : (*representatives)) {
      Node a = get_parent(i);
      Node b = get_parent(query[i]);
      if (a == b) continue;
      // make sure a is the one to be merged into
      if (size[a] < size[b]) std::swap(a,b);
      to_remove.push_back(b);
      parent[b] = a;
      supernodes[a]->merge(*supernodes[b]);
    }
    if (!to_remove.empty()) modified = true;
    for (Node i : to_remove) representatives->erase(i);
  } while (modified);

  map<Node, set<Node>> temp;
  for (Node i=0;i<num_nodes;++i)
    temp[get_parent(i)].insert(i);
  vector<set<Node>> retval;
  retval.reserve(temp.size());
  for (const auto& it : temp) retval.push_back(it.second);

  return retval;
}

void Graph::post_cc_resume() {
  GraphWorker::unpause_workers();
  update_locked = false;
}

Node Graph::get_parent(Node node) {
  if (parent[node] == node) return node;
  return parent[node] = get_parent(parent[node]);
}
