SUBDIRS = src \
          rpm

include_HEADERS = include/trans_rdma.h

rpm: rpm/libercat.spec rpmdirs rpm/libercat.spec rpm/SOURCES/libercat-0.1.tar.gz
	cp -f $(srcdir)/rpm/libercat.spec rpm || true
	cd rpm; rpmbuild -bb libercat.spec

srpm: rpm/libercat.spec rpmdirs rpm/libercat.spec rpm/SOURCES/libercat-0.1.tar.gz
	cp -f $(srcdir)/rpm/libercat.spec rpm || true
	cd rpm; rpmbuild -ba libercat.spec

rpm/SOURCES/libercat-0.1.tar.gz:
	tar -czf $@ --exclude $@ -C $(srcdir) .

rpm/SPECS rpm/SOURCES rpm/RPMS rpm/BUILD rpm/SRPMS:
	mkdir -p $@

rpmdirs: rpm/SPECS rpm/SOURCES rpm/RPMS rpm/BUILD rpm/SRPMS

clean-local:
	rm -rf doxygen

doc:
	doxygen ./doxygen.conf
	cd doxygen/latex; make pdf
