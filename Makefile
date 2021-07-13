DRIVER = /var/npreal2/module
SCRIPTS = mxmkdrv mxaddsvr mxdelsvr mxloadsvr
OBJS = npreal2d redund

ifdef POLLING
CPPFLAGS += -DOFFLINE_POLLING
endif

ifdef SSL
CPPFLAGS += -DSSL_ON -DOPENSSL_NO_KRB5
LIBS += -lssl
endif

all: $(OBJS)
npreal2d: npreal2d.o $(LIBS)
redund: redund_main.o redund.o -lpthread $(LIBS)

clean:
	rm -f *.o $(OBJS)

install:
	mkdir -p $(DESTDIR)/etc $(DESTDIR)/usr/bin $(DESTDIR)$(DRIVER)
	install -m 0644 npreal2d.cf $(DESTDIR)/etc
	install -m 0755 npreal2d redund $(SCRIPTS) $(DESTDIR)/usr/bin
	install -m 0644 Makefile.drv $(DESTDIR)$(DRIVER)/Makefile
	install -m 0644 npreal2.c npreal2.h np_ver.h $(DESTDIR)$(DRIVER)

