lib_LTLIBRARIES = libercat.la
libercat_la_SOURCES = trans_rdma.c

bin_PROGRAMS = rcat
rcat_SOURCES = rcat.c
rcat_LDADD = libercat.la -lrdmacm -libverbs -lpthread
