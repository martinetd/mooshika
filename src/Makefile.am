SUBDIRS = . tests #build . before tests

AM_CFLAGS = -g -Wall -Werror -I$(srcdir)/../include

lib_LTLIBRARIES = libmsk.la libmsk_shm.la
libmsk_la_SOURCES = trans_rdma.c
libmsk_la_LDFLAGS = -version-info 0:1:0
libmsk_shm_la_SOURCES = trans_shm.c
libmsk_shm_la_LDFLAGS = -version-info 0:1:0

bin_PROGRAMS = rcat rmitm
rcat_SOURCES = rcat.c
rcat_LDADD = -lrdmacm -libverbs -lpthread
if USE_SHM
rcat_LDADD += libmsk_shm.la
else
rcat_LDADD += libmsk.la
endif

rmitm_SOURCES = rmitm.c
rmitm_LDADD = -lrdmacm -libverbs -lpthread
if USE_SHM
rmitm_LDADD += libmsk_shm.la
else
rmitm_LDADD += libmsk.la
endif

pkgconfigdir=$(libdir)/pkgconfig
pkgconfig_DATA = libmsk.pc


