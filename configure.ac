AC_INIT([libercat], [1.0], [dominique.martinet.ocre@cea.fr])
AM_INIT_AUTOMAKE([-Wall -Werror])

AC_PROG_CC
AC_PROG_LIBTOOL

AC_SEARCH_LIBS([ibv_create_qp], [ibverbs], [], [
	AC_MSG_ERROR([unable to find libibverbs])
])

AC_SEARCH_LIBS([rdma_create_id], [rdmacm], [], [
	AC_MSG_ERROR([unable to find librdmacm])
])


AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([Makefile src/Makefile rpm/Makefile])
AC_OUTPUT
