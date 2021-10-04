ifeq ($(DEBUG),Y)
	CFLAGS+=-ggdb -O0 -Wall -DDEBUG
	LDFLAGS+=-ggdb
else
	CFLAGS+=-Wall
endif

LDFLAGS+=-lpulse -lavformat -lavutil -lavcodec -lswresample

.PHONY: clean install all tests

all: pareceive

pareceive: pareceive.o
	${CC} -o pareceive pareceive.o ${LDFLAGS}

pareceive.o: pareceive.c
	${CC} -c pareceive.c -I/usr/include/ffmpeg ${CFLAGS}

clean:
	rm -f *.o pareceive

install: pareceive
	cp pareceive /usr/local/bin/

tests: pareceive
	@for i in tests/*.sdf; do echo -e "\ncat $$i | LANG=C time ./pareceive -"; test "$$(cat $$i | LANG=C time ./pareceive - 2> >(tee >(cat 1>&2)) | grep "\(Playing\|Using\)" | tr '\n' ' ' | sed -e 's/ $$//'; echo " $${PIPESTATUS[1]}")" == "$$(cat $$i.txt) 0" || exit 1; done
	@ echo -e "\n cat tests/random.sdf tests/zero.sdf tests/classical_4_a1.sdf | LANG=C time ./pareceive -"; test "$$(cat tests/random.sdf tests/zero.sdf tests/classical_4_a1.sdf | LANG=C time ./pareceive - 2> >(tee >(cat 1>&2)) | grep "\(Playing\|Using\)" | tr '\n' ' '; echo "$${PIPESTATUS[1]}")" == "Playing PCM Playing silence Playing IEC61937: Audio: ac3, 48000 Hz, mono, fltp, 64 kb/s Using sample spec 's16le 2ch 48000Hz', channel map 'front-left,front-right'. Using sample spec 'float32le 1ch 48000Hz', channel map 'front-center'. 0" || exit 1
