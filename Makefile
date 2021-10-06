ifeq ($(COVERAGE),Y)
	CFLAGS+=--coverage
	LDFLAGS+=--coverage
	DEBUG=Y
endif

ifeq ($(DEBUG),Y)
	CFLAGS+=-ggdb -O0 -Wall -DDEBUG
	LDFLAGS+=-ggdb
else
	CFLAGS+=-Wall
endif

LDFLAGS+=-lpulse -lavformat -lavutil -lavcodec -lswresample

.PHONY: clean install all tests

SHELL = /bin/bash

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
	@echo -e "\ncat tests/random.sdf tests/zero.sdf tests/classical_4_a1.sdf | LANG=C time ./pareceive -"; OUTPUT="$$(cat tests/random.sdf tests/zero.sdf tests/classical_4_a1.sdf | LANG=C time ./pareceive - 2> >(tee >(cat 1>&2)); echo "Exit code $$?")"; test "$$(echo "$$OUTPUT" | grep "Playing" | tr '\n' ' ')" == "Playing PCM Playing silence Playing IEC61937: Audio: ac3, 48000 Hz, mono, fltp, 64 kb/s " || exit 1; test "$$(echo "$$OUTPUT" | grep "Using" | tr '\n' ' ')" == "Using sample spec 's16le 2ch 48000Hz', channel map 'front-left,front-right'. Using sample spec 'float32le 1ch 48000Hz', channel map 'front-center'. " || exit 1; test "$$(echo "$$OUTPUT" | grep "Exit code")" == "Exit code 0" || exit 1
	@echo -e "\ndd if=/dev/urandom bs=1024 count=\$$((1024*96*4)) | LANG=C time ./pareceive -"; read LATENCY STATUS <<< $$(dd if=/dev/urandom bs=1024 count=$$((1024*96*4)) | LANG=C time ./pareceive - 2> >(tee >(cat 1>&2)) | grep "Stream latency" | sed -e 's/Stream latency \([-0-9]*\) usec/\1/' | tr '\n' ' '; echo "$${PIPESTATUS[1]}"); test "$$STATUS" == "0" || exit $${STATUS:-1}; test "$$LATENCY" -lt 42000 || echo "Warning: PCM latency is too high ($$(($$LATENCY/1000)) ms)"
	@echo -e "\n(for i in \`seq 1 20\`; do cat tests/classical_17_441_a7_alt.sdf; done) | LANG=C time ./pareceive -"; read LATENCY STATUS <<< $$((for i in `seq 1 20`; do cat tests/classical_17_441_a7_alt.sdf; done) | LANG=C time ./pareceive - 2> >(tee >(cat 1>&2)) | grep "Stream latency" | sed -e 's/Stream latency \([-0-9]*\) usec/\1/' | tr '\n' ' '; echo "$${PIPESTATUS[1]}"); test "$$STATUS" == "0" || exit $${STATUS:-1}; test "$$LATENCY" -lt 42000 || echo "Warning: IEC61937 latency is too high ($$(($$LATENCY/1000)) ms)"
