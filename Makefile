all: build/recv.o

mkbuild:
	mkdir -p build/

build/%.o: src/%.c | mkbuild
	$(CC) -luring $? -o $@ $(CFLAGS)

clean:
	rm -rf build/
