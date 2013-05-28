SUBDIRS = . tests #build . before tests

AM_CFLAGS = -g -D_REENTRANT -Wall -Wimplicit -Wformat -Wmissing-braces -Wno-pointer-sign -Werror -I$(srcdir)/../include

lib_LTLIBRARIES = libmooshika.la
libmooshika_la_SOURCES = trans_rdma.c
libmooshika_la_LDFLAGS = -version-info 2:0:0
libmooshika_la_LIBADD = -lrdmacm -libverbs -lpthread -lrt

bin_PROGRAMS = rcat rmitm
rcat_SOURCES = rcat.c
rcat_LDADD = -lpthread
rcat_LDADD += libmooshika.la

if ENABLE_RMITM
rmitm_SOURCES = rmitm.c
rmitm_LDADD = -lpthread -lpcap
rmitm_LDADD += libmooshika.la
endif

pkgconfigdir=$(libdir)/pkgconfig
pkgconfig_DATA = libmooshika.pc


