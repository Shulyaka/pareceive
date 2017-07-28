all: pareceive

pareceive: pareceive.o
	gcc -o pareceive pareceive.o -lpulse -lavformat -lavutil -lavcodec -lswresample

pareceive.o: pareceive.c
	gcc -c pareceive.c -Wall -ggdb -I/usr/include/ffmpeg

clean:
	rm -f *.o pareceive
