PROG=thr
SRCS=main.c
CFLAGS+= -I${.CURDIR}/../../lib/libiapp/ -I${.CURDIR}/../../lib -Wall -Werror
LDFLAGS+= -L${.OBJDIR}/../../lib/libiapp/ -L${.OBJDIR}/../../lib/libidebug/
LDADD=-lpthread -liapp -lidebug
MK_MAN=no
DEBUG_FLAGS=-g

.include <bsd.prog.mk>
