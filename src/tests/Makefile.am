AM_CFLAGS = -g -Wall -Werror -I$(srcdir)/../../include -I$(srcdir)/..

check_PROGRAMS = read_write bench_rdma nfsv4_client multiple_sge bench_send

read_write_SOURCES = read_write.c
read_write_LDADD = -lrdmacm -libverbs -lpthread
if USE_SHM
read_write_LDADD += ../libmooshika_shm.la
else
read_write_LDADD += ../libmooshika.la
endif

bench_rdma_SOURCES = bench_rdma.c
bench_rdma_LDADD = -lrdmacm -libverbs -lpthread
if USE_SHM
bench_rdma_LDADD += ../libmooshika_shm.la
else
bench_rdma_LDADD += ../libmooshika.la
endif

bench_send_SOURCES = bench_send.c
bench_send_LDADD = -lrdmacm -libverbs -lpthread
if USE_SHM
bench_send_LDADD += ../libmooshika_shm.la
else
bench_send_LDADD += ../libmooshika.la
endif


nfsv4_client_SOURCES = nfsv4_client.c
nfsv4_client_LDADD = -lrdmacm -libverbs -lpthread
nfsv4_client_LDADD += ../libmooshika.la

multiple_sge_SOURCES = multiple_sge.c
multiple_sge_LDADD = -lrdmacm -libverbs -lpthread
multiple_sge_LDADD += ../libmooshika.la

