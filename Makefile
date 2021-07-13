KERNEL := /usr/src/kernels/$(shell uname -r)
DRIVER = /var/npreal2/module
ASYNC_PATT = \
	'ASYNCB_INITIALIZED|ASYNCB_CLOSING|ASYNCB_NORMAL_ACTIVE|ASYNCB_CHECK_CD'
SCRIPTS = mxmkdrv mxaddsvr mxdelsvr mxloadsvr
OBJS = npreal2d redund async_flags.h
CFLAGS = -g -O2 -Wall

ifdef POLLING
CPPFLAGS += -DOFFLINE_POLLING
endif

ifdef SSL
CPPFLAGS += -DSSL_ON -DOPENSSL_NO_KRB5
LIBS += -lssl
endif

all: $(OBJS)
npreal2d: npreal2d.o common.o $(LIBS)
redund: redund.o common.o -lpthread $(LIBS)

async_flags.h:
	if ! grep -E $(ASYNC_PATT) < $(KERNEL)/include/uapi/linux/tty_flags.h \
		> $@; then rm -f $@; false; fi

clean:
	rm -f *.o $(OBJS)

install:
	mkdir -p $(DESTDIR)/etc $(DESTDIR)/usr/bin $(DESTDIR)$(DRIVER)
	install -m 0644 npreal2d.cf $(DESTDIR)/etc
	install -m 0755 npreal2d redund $(SCRIPTS) $(DESTDIR)/usr/bin
	install -m 0644 Makefile.drv $(DESTDIR)$(DRIVER)/Makefile
	install -m 0644 npreal2.c npreal2.h np_ver.h \
		async_flags.h $(DESTDIR)$(DRIVER)

