.PHONY: default clean

default: monitor

monitor: monitor.c Makefile
	cc -std=c99 -Wall -g -O2 monitor.c -o monitor

midwrite: midwrite.c Makefile
	cc -std=c99 -Wall -g -O2 midwrite.c -o midwrite

clean:
	rm -fv monitor midwrite *~
