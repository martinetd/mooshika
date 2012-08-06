#!/bin/sh

# Usage: ./$0 on server, ./$0 serverip on client

if [[ -z "$1" ]]; then
	./rcat -s -m | pv -W -r > /dev/null
else
	for i in `seq 1 5`; do 
		(for j in `seq 1 20`; do
			dd if=/dev/zero bs=1M count=10 | ./rcat -c "$1" &
		done)&
	done
fi
