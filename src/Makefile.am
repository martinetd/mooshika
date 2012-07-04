AM_CFLAGS = -g -Wall -Werror -I$(srcdir)/../include

lib_LTLIBRARIES = libercat.la libercat_shm.la
libercat_la_SOURCES = trans_rdma.c
libercat_la_LDFLAGS = -version-info 0:1:0
libercat_shm_la_SOURCES = trans_shm.c
libercat_shm_la_LDFLAGS = -version-info 0:1:0

bin_PROGRAMS = rcat
rcat_SOURCES = rcat.c
rcat_LDADD = -lrdmacm -libverbs -lpthread
if USE_SHM
rcat_LDADD += libercat_shm.la
else
rcat_LDADD += libercat.la
endif

pkgconfigdir=$(libdir)/pkgconfig
pkgconfig_DATA = libercat.pc


