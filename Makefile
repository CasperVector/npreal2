LINUX := $(shell uname -r | sed 's/\..*//')
DRIVER = /var/npreal2/module
SCRIPTS = mxmkdrv mxaddsvr mxdelsvr mxloadsvr
OBJS = npreal2d redund
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

clean:
	rm -f *.o $(OBJS)

install:
	mkdir -p $(DESTDIR)/etc $(DESTDIR)/usr/bin $(DESTDIR)$(DRIVER)
	install -m 0644 npreal2d.cf $(DESTDIR)/etc
	install -m 0755 npreal2d redund $(SCRIPTS) $(DESTDIR)/usr/bin
	install -m 0644 npreal2.h np_ver.h $(DESTDIR)$(DRIVER)
	install -m 0644 npreal2-linux$(LINUX).c $(DESTDIR)$(DRIVER)/npreal2.c
	install -m 0644 /dev/null $(DESTDIR)$(DRIVER)/Makefile
	./cppflags.sh $(LINUX) >> $(DESTDIR)$(DRIVER)/Makefile
	cat Makefile.drv >> $(DESTDIR)$(DRIVER)/Makefile

