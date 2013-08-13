TEMPLATE = app
CONFIG += console
CONFIG -= app_bundle
CONFIG -= qt

OTHER_FILES += \
    README.md \
    test.lua

HEADERS += \
    luredis.h \
    hiredis/util.h \
    hiredis/sds.h \
    hiredis/net.h \
    hiredis/hiredis.h \
    hiredis/fmacros.h \
    hiredis/dict.h \
    hiredis/async.h \
    hiredis/adlist.h \
    lvm_config.h \
    hiredis/ae.h

SOURCES += \
    luredis.c \
    hiredis/util.c \
    hiredis/sds.c \
    hiredis/net.c \
    hiredis/hiredis.c \
    hiredis/dict.c \
    hiredis/async.c \
    hiredis/ae_epoll.c \
    hiredis/adlist.c \
    main.c \
    hiredis/ae_select.c \
    hiredis/ae_kqueue.c \
    hiredis/ae_evport.c \
    hiredis/ae.c

INCLUDEPATH += $$quote(/usr/local/include/)
LIBS += -L/usr/local/lib/ -lluajit-5.1
