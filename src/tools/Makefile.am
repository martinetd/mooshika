AM_CFLAGS = -g -D_REENTRANT -Wall -Wimplicit -Wformat -Wmissing-braces -Wno-pointer-sign -Werror -I$(srcdir)/../../include

noinst_PROGRAMS = 
if ENABLE_RMITM
noinst_PROGRAMS += pktdump
endif

pktdump_SOURCES = pktdump.c
pktdump_LDADD = -lpcap


