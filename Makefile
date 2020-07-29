ifeq ($(DEBUG),Y)
	CFLAGS+=-ggdb -O0 -Wall -DDEBUG
	LDFLAGS+=-ggdb
else
	CFLAGS+=-Wall
endif

LDFLAGS+=-lpulse -lavformat -lavutil -lavcodec -lswresample

.PHONY: clean install all

all: pareceive

pareceive: pareceive.o
	${CC} -o pareceive pareceive.o ${LDFLAGS}

pareceive.o: pareceive.c
	${CC} -c pareceive.c -I/usr/include/ffmpeg ${CFLAGS}

clean:
	rm -f *.o pareceive

install: pareceive
	cp pareceive /usr/local/bin/
