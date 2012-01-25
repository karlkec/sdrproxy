CC=gcc
CFLAGS=-Wall -m32 -ggdb -pthread
OBJDIR=../obj

LOCOBJS = buffer.o client.o databfr.o main.o server.o socket.o time.o util.o

OBJS = $(OBJDIR)/buffer.o $(OBJDIR)/client.o $(OBJDIR)/databfr.o $(OBJDIR)/main.o $(OBJDIR)/server.o $(OBJDIR)/socket.o $(OBJDIR)/time.o $(OBJDIR)/util.o

all: ../sdrproxy

linux: ../sdrproxy

../sdrproxy: $(LOCOBJS) $(OBJS) sdrproxy.h Makefile
	find . -maxdepth 1 -name '*.o' && mv *.o $(OBJDIR)
	$(CC) $(CFLAGS) $(OBJS) -o ../sdrproxy

$(OBJDIR)/buffer.o: buffer.c sdrproxy.h Makefile
	$(CC) $(CFLAGS) -c -o buffer.o buffer.c
$(OBJDIR)/client.o: client.c sdrproxy.h Makefile
	$(CC) $(CFLAGS) -c -o client.o client.c
$(OBJDIR)/databfr.o: databfr.c sdrproxy.h Makefile
	$(CC) $(CFLAGS) -c -o databfr.o databfr.c
$(OBJDIR)/main.o: main.c sdrproxy.h Makefile
	$(CC) $(CFLAGS) -c -o main.o main.c
$(OBJDIR)/server.o: server.c sdrproxy.h Makefile
	$(CC) $(CFLAGS) -c -o server.o server.c
$(OBJDIR)/socket.o: socket.c sdrproxy.h Makefile
	$(CC) $(CFLAGS) -c -o socket.o socket.c
$(OBJDIR)/time.o: time.c sdrproxy.h Makefile
	$(CC) $(CFLAGS) -c -o time.o time.c
$(OBJDIR)/util.o: util.c sdrproxy.h Makefile
	$(CC) $(CFLAGS) -c -o util.o util.c

clean:
	rm -f ../sdrproxy $(OBJS)
