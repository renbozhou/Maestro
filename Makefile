# $@ - left side of rule.
# $^ - right side of rule.
# $< - first prerequisite (usually the source file)

CC = gcc
CFLAGS = -O2 -Wall
LDFLAGS = -lpthread
OBJS = base64.o \
       deflate.o \
       thpool.o \
       linkedlist.o \
       io.o \
       util.o \
       http_msg.o \
       http_parser.o \
       http_svc.o \
       http_conn.o \
       maestro.o
EXES = maestro

all: ${EXES}

${EXES}: $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	${CC} -o $@ -c $< $(CFLAGS)

clean:
	$(RM) *.o $(EXES)
