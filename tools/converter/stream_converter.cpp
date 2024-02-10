#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>

#include "binary_file_stream.h"

int main(int argc, char** argv) {

  if (argc != 4) {
    std::cout << "ERROR: Incorrect number of arguments!" << std::endl;
    std::cout << "Arguments: graph_file num_nodes num_edges" << std::endl;
    exit(EXIT_FAILURE);
  }

	std::string file_name = argv[1];
	std::ifstream graph_file(file_name);
	std::string line;

	std::string stream_name = file_name.substr(0, file_name.length() - 4) + "_stream_binary";
	BinaryFileStream fout(stream_name , false);

	size_t num_nodes = std::atoi(argv[2]); 
	size_t num_edges = std::atoi(argv[3]);

	std::cout << "Input Graph File: " << file_name << "\n";
  	std::cout << "Reading Input Graph File...\n";

	std::cout << "  Num Nodes: " << num_nodes << "\n";
	std::cout << "  Num Edges: " << num_edges << "\n";

	fout.write_header(num_nodes, num_edges);

	GraphStreamUpdate update;
	update.type = INSERT;

	std::cout << "Writing Binary Stream File...\n";
	
	if(graph_file.is_open()) {
		size_t num_read_nodes = 0;
		size_t num_read_edges = 0;

		std::map<node_id_t, node_id_t> node_ids;
		
		while(std::getline(graph_file, line)) {
			std::istringstream iss(line);
			std::string token;

			node_id_t node1, node2;

			std::getline(iss, token, ' ');
			node1 = std::stoi(token);

			std::getline(iss, token, ' ');
			node2 = std::stoi(token);

			update.edge = {node1, node2};
      		fout.write_updates(&update, 1);

			num_read_edges++;

			if (node_ids.find(node1) == node_ids.end()) {
				node_ids[node1] = 1;
				num_read_nodes++;
			}

			if (node_ids.find(node2) == node_ids.end()) {
				node_ids[node2] = 1;
				num_read_nodes++;
			}

			if(num_read_edges % 100000 == 0) {
				std::cout << "  Progress - Edges Read: " << num_read_edges << "\n";
			}
		}

		std::cout << "  Num Read Nodes: " << num_read_nodes << "\n";
		std::cout << "  Num Read Edges: " << num_read_edges << "\n";

		if ((num_nodes != num_read_nodes) || (num_edges != num_read_edges)) {
			std::cout << "ERROR: Number of read nodes or edges not matching to the original graph!\n";
		}
	}

	graph_file.close();

}
