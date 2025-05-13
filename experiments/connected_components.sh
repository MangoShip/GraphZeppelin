#
# Name: connected_components.sh
#
# Full system experiments for solving the 
#   connected components problem with graph stream
#

if [[ $# -lt 6 ]]; then
  echo "ERROR: Invalid Arguments!"
  echo "USAGE: results_dir workers readers num_it, using_num_batch_table, stream_files[+]"
  echo "results_dir:           Directory where csv file should be placed"
  echo "workers:               Number of graph workers"
  echo "readers:               Number of graph readers"
  echo "num_it:                Number of iterations"
  echo "using_num_batch_table: Using num_batch_table"
  echo "stream_files:          One or more stream files to process"
  exit 1
fi

# Following numbers are specific values of num_batch_per_buffer for each dataset
# These numbers have shown the best performance in Mingun's machine
declare -A num_batch_table=(
  [kron_13_stream_binary]=54
  [kron_15_stream_binary]=216
  [kron_16_stream_binary]=540
  [kron_17_stream_binary]=540
  [ktree_13_2048_stream_binary_shuffled]=54
  [ktree_14_4096_stream_binary_shuffled]=216
  [ktree_15_8192_stream_binary_shuffled]=216
  [ktree_16_16384_stream_binary_shuffled]=540
  [ktree_17_32768_stream_binary_shuffled]=540
  [ca_citeseer_stream_binary]=216
  [google_plus_stream_binary]=216
  [p2p_gnutella_stream_binary]=216
  [rec_amazon_stream_binary]=216
  [web_uk_stream_binary]=216)

result_dir=$1
workers=$2
readers=$3
num_it=$4

# Declare to use the above table or not 
# (Not using the table will use the default value of num_batch_per_buffer)
using_num_batch_table=$5

shift 5

cd build

out_file=runtime_results.csv

# Note Message for GPU performance
echo "Note: To ensure the best performance for GPU, make sure to maximize its clock speed by ./nvidia-smi -lgc [CLOCK_SPEED]"

# Write header to outfile csv
echo "stream_file, num_batch_per_buffer, ingestion_rate (1e6), memory_usage (MiB), query_latency (sec)" > $out_file

# Process input files
for input in $@
do
  echo "============================================================"
  echo "============================================================"

  stream_name=`basename $input`
  for (( it=1; it <= $num_it; ++it ))
  do
    if [ $using_num_batch_table = true ]
    then
      if [[ -v "num_batch_table[$stream_name]" ]] # Check if registered in table
      then 
        echo -n "$stream_name, ${num_batch_table[$stream_name]}, " >> $out_file
        ./cuda_process_stream $input $workers $readers ${num_batch_table[$stream_name]}
      else
        echo $stream_name " does not exist in num_batch_table"
      fi
    else
      echo -n "$stream_name, 540, " >> $out_file
      ./cuda_process_stream $input $workers $readers
    fi
  done
done

cd -
mv build/$out_file $result_dir/cc.csv
