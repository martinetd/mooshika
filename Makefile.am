ACLOCAL_AMFLAGS = -I m4

SUBDIRS = src

include_HEADERS = include/mooshika.h

rpm: dist libmooshika.spec
	rpmbuild $(RPM_DB_ARGS) $(extra_arg) -tb $(distdir).tar.gz

srpm: dist libmooshika.spec
	rpmbuild $(RPM_DB_ARGS) $(extra_arg) -ts $(distdir).tar.gz

rpms: rpm srpm

rpm/SPECS rpm/SOURCES rpm/RPMS rpm/BUILD rpm/SRPMS:
	mkdir -p $@

rpmdirs: rpm/SPECS rpm/SOURCES rpm/RPMS rpm/BUILD rpm/SRPMS

clean-local:
	rm -rf doxygen

doc:
	doxygen ./doxygen.conf
	cd doxygen/latex; make pdf

EXTRA_DIST=libmooshika.spec \
           src/utils.h \
           src/rmitm.h \
           LICENSE
