all: $(patsubst %.sdf, %.sdf.txt, $(wildcard *.sdf))

clean:
	rm *.sdf.txt

.PHONY: clean all

%.sdf.txt : %.sdf
	cat $< | LANG=C ../pareceive - 2>&1 | grep "\(Playing\|Using\)" | tr '\n' ' ' | sed -e 's/ $$//' > $<.txt
