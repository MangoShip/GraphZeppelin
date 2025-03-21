if [[ $# -lt 4 ]]; then
  echo "ERROR: Invalid Arguments!"
  echo "USAGE: results_dir workers readers stream_files[+]"
  echo "results_dir:  Directory where csv file should be placed"
  echo "workers:      Number of graph workers"
  echo "readers:      Number of graph readers"
  echo "stream_files: One or more stream files to process"
  exit 1
fi


result_dir=$1
workers=$2
readers=$3
shift 3

cd build

out_file=runtime_results.csv

# Write header to outfile csv
echo "edge_store, stream_file, ingestion_rate (1e6), memory_usage (MiB), query_latency (sec)" > $out_file

# Process input files
for input in $@
do
  echo "============================================================"
  echo "============================================================"

  stream_name=`basename $input`
  echo -n "yes, $stream_name, " >> $out_file
  ./min_cut $input $workers $readers no
done

cd -
mv build/$out_file $result_dir/minimum_cut.csv
