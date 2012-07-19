AC_INIT([libmsk], [0.1], [dominique.martinet.ocre@cea.fr])
AM_INIT_AUTOMAKE([-Wall -Werror])

AC_CONFIG_MACRO_DIR([m4])

AC_GNU_SOURCE
AC_PROG_CC
AC_PROG_LIBTOOL

# Check for InfiniBand Libs and Headers
AC_SEARCH_LIBS([ibv_create_qp], [ibverbs], [], [
	AC_MSG_ERROR([unable to find libibverbs])
])
AC_CHECK_HEADERS([infiniband/verbs.h],[], [AC_MSG_ERROR(missing infiniband headers)])

# Check for RDAM_CM Libs and Headers
AC_SEARCH_LIBS([rdma_create_id], [rdmacm], [], [
	AC_MSG_ERROR([unable to find librdmacm])
])
AC_CHECK_HEADERS([rdma/rdma_verbs.h],[], [AC_MSG_ERROR(missing rdma headers)])

AC_DEFINE( [VERSION_COMMENT], ["libmsk and examples"], [No Comment])

# Git latest commit
AC_MSG_CHECKING( [Git HEAD] )
head_commit=`git rev-parse HEAD 2>/dev/null`

if test "x$head_commit" == "x" ; then
  AC_MSG_RESULT( [no git here] ) 
  AC_DEFINE_UNQUOTED( [_GIT_HEAD_COMMIT], "not compiled within a git repository", [Lastest HEAD at the time configure was run])
  AC_MSG_CHECKING( [Git describe] )
  AC_DEFINE_UNQUOTED( [_GIT_DESCRIBE], "not compiled within a git repository", [Result of git-describe --long])
else
  AC_MSG_RESULT( $head_commit ) 
  AC_DEFINE_UNQUOTED( [_GIT_HEAD_COMMIT], "$head_commit", [Lastest HEAD at the time configure was run])
  AC_MSG_CHECKING( [Git describe] )
  git_describe=`git describe --long`
  AC_MSG_RESULT( $git_describe ) 
  AC_DEFINE_UNQUOTED( [_GIT_DESCRIBE], "$git_describe", [Result of git-describe --long])
fi


# use shm implem
AC_ARG_ENABLE([shm], AS_HELP_STRING([--enable-shm], [use shmem implementation for the rcat binary [default=no]]))
AM_CONDITIONAL(USE_SHM, test "x$enable_shm" = "xyes")

#for exporting to spec file
AC_SUBST(ac_configure_args)

AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([Makefile src/Makefile src/tests/Makefile rpm/Makefile src/libmsk.pc libmsk.spec])
AC_OUTPUT
