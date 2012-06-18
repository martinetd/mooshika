CC=gcc
CFLAGS=-g -Wall -Werror -I/cea/home/gpocre/martinetd/.usr/include/
COPTFLAGS=-O2
LDFLAGS=-L/cea/home/gpocre/martinetd/.usr/lib -libverbs -lrdmacm
SOURCES=trans_rdma.c
OBJECTS=$(SOURCES:.c=.o)
MAIN_SOURCE=rcat.c
MAIN=rcat


all: $(OBJECTS) main

opt: $(OBJECTS)
	$(CC) $(CFLAGS) $(COPTFLAGS) -o $(MAIN) $(LDFLAGS) $(OBJECTS) $(MAIN_SOURCE)

main:
	$(CC) -o $(MAIN) $(CFLAGS) $(LDFLAGS) $(OBJECTS) $(MAIN_SOURCE)

.o:
	$(CC) -c $(CFLAGS) $< -o $@

clean:
	rm -f *.o $(MAIN)
