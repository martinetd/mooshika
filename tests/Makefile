CC=gcc
CFLAGS=-g -Wall -Werror -I/cea/home/gpocre/martinetd/.usr/include/ -I..
COPTFLAGS=-O2
LDFLAGS=-L/cea/home/gpocre/martinetd/.usr/lib -libverbs -lrdmacm
SOURCES=../trans_rdma.c
OBJECTS=$(SOURCES:.c=.o)
MAIN_SOURCE=server.c client.c server_wait.c client_wait.c
MAIN=$(MAIN_SOURCE:.c=)


all: $(MAIN)

$(MAIN) : $(OBJECTS) $(MAIN_SOURCE)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJECTS) $@.c -o $@

.o:
	$(CC) -c $(CFLAGS) $< -o $@

clean:
	rm -f $(OBJECTS) $(MAIN)
