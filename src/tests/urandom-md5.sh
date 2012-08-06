#!/bin/sh

# Usage: ./$0 on server, ./$0 serverip on client

if [[ -z "$1" ]]; then
	./rcat -s | md5sum -
else
	dd if=/dev/urandom bs=10M count=1 > /tmp/urandom
	(for i in `seq 1 10`; do cat /tmp/urandom; done)| tee >(md5sum - 1>&2) | ./rcat -c "$1"
fi

