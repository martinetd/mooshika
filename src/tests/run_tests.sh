
# $1 = display string, $2-rest = program
runit() {
  local STR=$1
  shift
  eval $@ >& /dev/null
  printf "%s status: %d\n" "$STR" $?
}

IP="$1"

for prog in ./bench_rdma ./multiple_sge ./read_write; do
	echo "Starting server $prog -S \"$IP\""
	runit "Server" $prog -S "$IP" &
	sleep 0.2

	echo "Starting client: $prog -c \"$IP\""
	runit "Client" $prog -c "$IP"
done
