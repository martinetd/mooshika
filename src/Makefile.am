AM_CFLAGS = -g -Wall -Werror

lib_LTLIBRARIES = libercat.la libercat_shm.la
libercat_la_SOURCES = trans_rdma.c
libercat_shm_la_SOURCES = trans_shm.c

bin_PROGRAMS = rcat
rcat_SOURCES = rcat.c
rcat_LDADD = libercat.la -lrdmacm -libverbs -lpthread
