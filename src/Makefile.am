SUBDIRS = . tests #build . before tests

AM_CFLAGS = -g -D_REENTRANT -Wall -Wimplicit -Wformat -Wmissing-braces -Wno-pointer-sign -Werror -I$(srcdir)/../include

lib_LTLIBRARIES = libmooshika.la libmooshika_shm.la
libmooshika_la_SOURCES = trans_rdma.c
libmooshika_la_LDFLAGS = -version-info 1:0:0
libmooshika_shm_la_SOURCES = trans_shm.c
libmooshika_shm_la_LDFLAGS = -version-info 1:0:0

bin_PROGRAMS = rcat rmitm
rcat_SOURCES = rcat.c
rcat_LDADD = -lrdmacm -libverbs -lpthread
if USE_SHM
rcat_LDADD += libmooshika_shm.la
else
rcat_LDADD += libmooshika.la
endif

rmitm_SOURCES = rmitm.c
rmitm_LDADD = -lrdmacm -libverbs -lpthread -lpcap
if USE_SHM
rmitm_LDADD += libmooshika_shm.la
else
rmitm_LDADD += libmooshika.la
endif

pkgconfigdir=$(libdir)/pkgconfig
pkgconfig_DATA = libmooshika.pc


