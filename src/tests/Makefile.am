AM_CFLAGS = -g @WARNINGS_CFLAGS@ -I$(srcdir)/../../include -I$(srcdir)/..

noinst_PROGRAMS = read_write bench_rdma nfsv4_client multiple_sge
read_write_SOURCES = read_write.c
read_write_LDADD = -lrdmacm -libverbs -lpthread
read_write_LDADD += ../libmooshika.la

bench_rdma_SOURCES = bench_rdma.c
bench_rdma_LDADD = -lrdmacm -libverbs -lpthread
bench_rdma_LDADD += ../libmooshika.la

nfsv4_client_SOURCES = nfsv4_client.c
nfsv4_client_LDADD = -lrdmacm -libverbs -lpthread
nfsv4_client_LDADD += ../libmooshika.la

multiple_sge_SOURCES = multiple_sge.c
multiple_sge_LDADD = -lrdmacm -libverbs -lpthread
multiple_sge_LDADD += ../libmooshika.la
