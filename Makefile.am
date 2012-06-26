SUBDIRS = src \
          rpm

include_HEADERS = include/trans_rdma.h

rpm: rpm/libercat.spec rpmdirs rpm/SOURCES/libercat-0.1.tar.gz
	cd rpm; rpmbuild -bb libercat.spec

srpm: rpm/libercat.spec rpmdirs rpm/SOURCES/libercat-0.1.tar.gz
	cd rpm; rpmbuild -ba libercat.spec

rpm/SOURCES/libercat-0.1.tar.gz:
	tar -czf $@ --exclude $@ .

rpm/SPECS rpm/SOURCES rpm/RPMS rpm/BUILD rpm/SRPMS:
	mkdir -p $@

rpmdirs: rpm/SPECS rpm/SOURCES rpm/RPMS rpm/BUILD rpm/SRPMS

doc:
	doxygen ./doxygen.conf
