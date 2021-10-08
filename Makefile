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
	@for i in tests/*.sdf; do echo -e "\ncat $$i | time ./pareceive -"; test "$$(cat $$i | LANG=C time ./pareceive - 2> >(tee >(cat 1>&2)) | grep "\(Playing\|Using\)" | tr '\n' ' ' | sed -e 's/ $$//'; echo " $${PIPESTATUS[1]}")" == "$$(cat $$i.txt) 0" || exit 1; done
	@echo -e "\ncat tests/random.sdf tests/zero.sdf tests/classical_4_a1.sdf | LANG=C time ./pareceive -"; OUTPUT="$$(cat tests/random.sdf tests/zero.sdf tests/classical_4_a1.sdf | LANG=C time ./pareceive - 2> >(tee >(cat 1>&2)); echo "Exit code $$?")"; test "$$(echo "$$OUTPUT" | grep "Playing" | tr '\n' ' ')" == "Playing PCM Playing silence Playing IEC61937: Audio: ac3, 48000 Hz, mono, fltp, 64 kb/s " || exit 1; test "$$(echo "$$OUTPUT" | grep "Using" | tr '\n' ' ')" == "Using sample spec 's16le 2ch 48000Hz', channel map 'front-left,front-right'. Using sample spec 'float32le 1ch 48000Hz', channel map 'front-center'. " || exit 1; test "$$(echo "$$OUTPUT" | grep "Exit code")" == "Exit code 0" || exit 1
	@echo -e "\ndd if=/dev/urandom bs=1024 count=\$$((1024*96*4)) | time ./pareceive -"; read LATENCY STATUS <<< $$(dd if=/dev/urandom bs=1024 count=$$((1024*96*4)) | LANG=C time ./pareceive - 2> >(tee >(cat 1>&2)) | grep "Output stream latency" | sed -e 's/Output stream latency \([-0-9]*\) usec/\1/' | tr '\n' ' '; echo "$${PIPESTATUS[1]}"); test "$$STATUS" == "0" || exit $${STATUS:-1}; test "$$LATENCY" -lt 41667 || echo "Warning: PCM latency is too high ($$((($$LATENCY+500)/1000)) ms)"
	@echo -e "\n(for i in \`seq 1 20\`; do cat tests/classical_17_441_a7_alt.sdf; done) | time ./pareceive -"; read LATENCY STATUS <<< $$((for i in `seq 1 20`; do cat tests/classical_17_441_a7_alt.sdf; done) | LANG=C time ./pareceive - 2> >(tee >(cat 1>&2)) | grep "Output stream latency" | sed -e 's/Output stream latency \([-0-9]*\) usec/\1/' | tr '\n' ' '; echo "$${PIPESTATUS[1]}"); test "$$STATUS" == "0" || exit $${STATUS:-1}; test "$$LATENCY" -lt 41667 || echo "Warning: IEC61937 latency is too high ($$((($$LATENCY+500)/1000)) ms)"
	@echo -e "\npacat tests/random.sdf & time ./pareceive $$(LANG=C pactl list sources|grep "\(Name\|Monitor of Sink\)" | grep "Monitor of Sink: $$(LANG=C pactl info | sed -En 's/Default Sink: (.*)/\1/p')" -B1 | grep "Name: " | sed -e 's/.*Name: //') & sleep 1; wait %1; kill %2; sleep 1"; OUTPUT="$$((PROC=$$(PROC=$$(LANG=C pacat tests/random.sdf -v 2>&1 | grep "Connected to device" --line-buffered | sed -e "s/Connected to device \([^ ]*\).*/\1/" -u | while read SINK; do (LANG=C time ./pareceive $$(LANG=C pactl list sources|grep "\(Name\|Monitor of Sink\)" | grep "Monitor of Sink: $$SINK" -B1 | grep "Name: " | sed -e 's/.*Name: //'); echo "Exit code $$?") >&2 & echo $$!; done ); ps -ef | grep pareceive | grep $$PROC | grep -v grep | grep time | awk '{print $$2;}'); ps -ef | grep pareceive | grep $$PROC | grep -v grep | grep -v time | awk '{print $$2;}' | xargs kill) 2> >(tee >(cat 1>&2)))"; test "$$(echo "$$OUTPUT" | grep "Exit code")" == "Exit code 0" || exit 1; test "$$(echo "$$OUTPUT" | grep "\(Playing\|Using\)" | tr '\n' ' ')" == "Using sample spec 's16le 2ch 48000Hz', channel map 'front-left,front-right'. Playing PCM Using sample spec 's16le 2ch 48000Hz', channel map 'front-left,front-right'. " -o "$$(echo "$$OUTPUT" | grep "\(Playing\|Using\)" | tr '\n' ' ')" == "Using sample spec 's16le 2ch 44100Hz', channel map 'front-left,front-right'. Playing PCM Using sample spec 's16le 2ch 44100Hz', channel map 'front-left,front-right'. " || exit 1; echo "$$OUTPUT" | grep "stream latency" | sed -e 's/.*stream latency \([-0-9]*\) usec/\1/'| while read LATENCY; do test "$$LATENCY" -lt 41667 || echo "Warning: Latency is too high ($$((($$LATENCY+500)/1000)) ms)"; done
