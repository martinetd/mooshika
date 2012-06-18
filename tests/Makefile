CC=gcc
CFLAGS=-g -Wall -Werror -I/cea/home/gpocre/martinetd/.usr/include/ -I..
COPTFLAGS=-O2
LDFLAGS=-L/cea/home/gpocre/martinetd/.usr/lib -libverbs -lrdmacm
SOURCES=../trans_rdma.c
OBJECTS=$(SOURCES:.c=.o)
MAIN_SOURCE=server.c client.c server_wait.c client_wait.c
MAIN=$(MAIN_SOURCE:.c=)


all: $(MAIN)

server: server.c
	$(CC) -o $@ $(CFLAGS) $(LDFLAGS) $(OBJECTS) $?

client: client.c
	$(CC) -o $@ $(CFLAGS) $(LDFLAGS) $(OBJECTS) $?

server_wait: server_wait.c
	$(CC) -o $@ $(CFLAGS) $(LDFLAGS) $(OBJECTS) $?

client_wait: client_wait.c
	$(CC) -o $@ $(CFLAGS) $(LDFLAGS) $(OBJECTS) $?

.o:
	$(CC) -c $(CFLAGS) $< -o $@

clean:
	rm -f *.o $(MAIN)
