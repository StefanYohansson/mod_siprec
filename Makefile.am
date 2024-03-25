include $(top_srcdir)/build/modmake.rulesam

MODNAME = mod_siprec

mod_LTLIBRARIES = mod_siprec.la
mod_siprec_la_SOURCES  = mod_siprec.c
mod_siprec_la_CFLAGS   = $(AM_CFLAGS)
mod_siprec_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_siprec_la_LDFLAGS  = -avoid-version -module -no-undefined -shared