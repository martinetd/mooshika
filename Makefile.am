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
	rm -rf doc/doxygen

doc:
	doxygen ./doc/doxygen.conf
	cd doc/doxygen/latex; make pdf
	cd doc; asciidoc DESIGN; a2x -fpdf DESIGN
	cd doc/man; asciidoc rcat.1.txt; a2x -fmanpage rcat.1.txt; a2x -fpdf rcat.1.txt

EXTRA_DIST=libmooshika.spec \
           src/utils.h \
           src/rmitm.h \
           src/atomics.h \
           LICENSE
