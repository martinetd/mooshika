AM_CFLAGS = -g -D_REENTRANT @WARNINGS_CFLAGS@ -I$(srcdir)/../../include

noinst_PROGRAMS = 
if ENABLE_RMITM
noinst_PROGRAMS += pktdump
endif

pktdump_SOURCES = pktdump.c
pktdump_LDADD = -lpcap


