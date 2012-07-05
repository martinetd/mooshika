SUBDIRS = src \
          rpm

include_HEADERS = include/trans_rdma.h

rpm: dist libercat.spec
	rpmbuild $(RPM_DB_ARGS) $(extra_arg) --define "_topdir `pwd`/rpm" -tb $(distdir).tar.gz

srpm: dist libercat.spec
	rpmbuild $(RPM_DB_ARGS) $(extra_arg) --define "_topdir `pwd`/rpm" -ts $(distdir).tar.gz

rpm/SPECS rpm/SOURCES rpm/RPMS rpm/BUILD rpm/SRPMS:
	mkdir -p $@

rpmdirs: rpm/SPECS rpm/SOURCES rpm/RPMS rpm/BUILD rpm/SRPMS

clean-local:
	rm -rf doxygen

doc:
	doxygen ./doxygen.conf
	cd doxygen/latex; make pdf

EXTRA_DIST=libercat.spec \
           src/log.h
