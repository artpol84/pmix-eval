# Copyright (c) 2013      Mellanox Technologies, Inc.
#                         All rights reserved.
# Copyright (c) 2013-2014 Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2014      Intel, Inc.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$

dist_oshmemdata_DATA =  \
	shmemcc-wrapper-data.txt \
	shmemfort-wrapper-data.txt

if PROJECT_OSHMEM
# Only install / uninstall if we're building oshmem
install-exec-hook:
	test -z "$(bindir)" || $(mkdir_p) "$(DESTDIR)$(bindir)"
	(cd $(DESTDIR)$(bindir); rm -f shmemrun$(EXEEXT); $(LN_S) mpirun shmemrun)
	(cd $(DESTDIR)$(bindir); rm -f oshrun$(EXEEXT); $(LN_S) mpirun oshrun)
	(cd $(DESTDIR)$(bindir); rm -f shmemcc$(EXEEXT); $(LN_S) mpicc shmemcc)
	(cd $(DESTDIR)$(bindir); rm -f oshcc$(EXEEXT); $(LN_S) mpicc oshcc)
	(cd $(DESTDIR)$(bindir); rm -f shmemfort$(EXEEXT); $(LN_S) mpifort shmemfort)
	(cd $(DESTDIR)$(bindir); rm -f oshfort$(EXEEXT); $(LN_S) mpifort oshfort)
	(cd $(DESTDIR)$(bindir); rm -f shmemjavac$(EXEEXT); $(LN_S) mpijavac shmemjavac)
	(cd $(DESTDIR)$(bindir); rm -f oshjavac$(EXEEXT); $(LN_S) mpijavac oshjavac)

install-data-hook:
	(cd $(DESTDIR)$(pkgdatadir); rm -f oshcc-wrapper-data.txt; $(LN_S) shmemcc-wrapper-data.txt oshcc-wrapper-data.txt)
	(cd $(DESTDIR)$(pkgdatadir); rm -f oshfort-wrapper-data.txt; $(LN_S) shmemfort-wrapper-data.txt oshfort-wrapper-data.txt)

uninstall-local:
	rm -f $(DESTDIR)$(bindir)/shmemrun$(EXEEXT) \
	$(DESTDIR)$(bindir)/oshrun$(EXEEXT) \
	$(DESTDIR)$(bindir)/shmemcc$(EXEEXT) \
	$(DESTDIR)$(bindir)/oshcc$(EXEEXT) \
	$(DESTDIR)$(bindir)/shmemfort$(EXEEXT) \
	$(DESTDIR)$(bindir)/oshfort$(EXEEXT) \
	$(DESTDIR)$(bindir)/shmemjavac$(EXEEXT) \
	$(DESTDIR)$(bindir)/oshjavac$(EXEEXT) \
	$(DESTDIR)$(pkgdatadir)/shmemcc-wrapper-data.txt \
	$(DESTDIR)$(pkgdatadir)/oshcc-wrapper-data.txt \
	$(DESTDIR)$(pkgdatadir)/shmemfort-wrapper-data.txt \
	$(DESTDIR)$(pkgdatadir)/oshfort-wrapper-data.txt

endif # PROJECT_OSHMEM
