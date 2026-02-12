PROG=thr
SRCS=main.c
CFLAGS+= -I${.CURDIR}/../../lib/libiapp/ -Wall -Werror
LDFLAGS+= -L${.OBJDIR}/../../lib/libiapp/
LDADD=-lpthread -liapp
MK_MAN=no
DEBUG_FLAGS=-g

.include <bsd.prog.mk>
