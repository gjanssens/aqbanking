

INCLUDES = -I$(top_srcdir)/src/libs -I$(top_builddir)/src/libs $(gwenhywfar_includes) -I$(srcdir)/../.. $(AQEBICS_CFLAGS)

AM_CFLAGS=-DBUILDING_AQBANKING @visibility_cflags@

noinst_LTLIBRARIES=libmsg.la

libmsg_la_SOURCES=\
 keys.c \
 msg.c \
 xml.c \
 zip.c \
 eu.c

noinst_HEADERS=\
 eu.h \
 eu_p.h \
 keys.h \
 msg.h \
 msg_p.h \
 xml.h \
 xml_p.h \
 zip.h


sources:
	for d in $(SUBDIRS); do \
	  $(MAKE) -C $$d sources; \
	done
	for f in $(libmsg_la_SOURCES); do \
	  echo $(subdir)/$$f >>$(top_srcdir)/i18nsources; \
	done


cppcheck:
	for f in $(testlib_SOURCES); do \
	  cppcheck --force $$f ; \
	done
	for d in $(SUBDIRS); do \
	  $(MAKE) -C $$d cppcheck; \
	done


