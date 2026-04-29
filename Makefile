.PHONY: all debug clean

all:
	bash ./make-mouse

debug:
	bash ./make-mouse --debug

clean:
	rm -rf build .debug
