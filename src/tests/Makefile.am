AM_CFLAGS = -g -Wall -Werror -I$(srcdir)/../../include -I$(srcdir)/..

bin_PROGRAMS = read_write

read_write_SOURCES = read_write.c
read_write_LDADD = -lrdmacm -libverbs -lpthread
if USE_SHM
read_write_LDADD += libercat_shm.la
else
read_write_LDADD += libercat.la
endif


