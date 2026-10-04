set shell := ["sh", "-eu", "-c"]

CC := env_var_or_default("CC", "cc")
STRICT := "-std=c89 -pedantic-errors -Wall -Wextra -Werror -O2"
INCS := "-Iinclude -Isrc -I../libdlx89/include -I../libdlq89/include -I../libdatalog89/include"
SRC := "src/dldb89.c src/dldb89_eval.c src/dldb89_program.c src/dldb89_dlq.c"
SIBLINGS := "../libdlx89/build/libdlx89.a ../libdlq89/build/libdlq89.a ../libdatalog89/build/libdatalog89.a ../libunicode89/build/libunicode89.a"

default: build

# Build every sibling static archive this library links against.
deps:
	cd ../libunicode89 && just build
	cd ../libdatalog89 && just build
	cd ../libdlx89 && just build
	cd ../libdlq89 && just build

# Compile the library and archive build/libdldb89.a.
build: deps
	mkdir -p build
	rm -f build/*.o
	@for f in {{SRC}}; do \
	    name=$(basename "$f" .c); \
	    {{CC}} {{STRICT}} {{INCS}} -c "$f" -o "build/$name.o" || exit 1; \
	done
	rm -f build/libdldb89.a
	ar rcs build/libdldb89.a build/dldb89.o build/dldb89_eval.o build/dldb89_program.o build/dldb89_dlq.o

# Smoke test: end-to-end load, evaluate, query, dump, reopen.
smoke: build
	{{CC}} {{STRICT}} {{INCS}} -o build/smoke test/test_smoke.c build/libdldb89.a {{SIBLINGS}} -lsqlite3
	./build/smoke

reject: build
	{{CC}} {{STRICT}} {{INCS}} -o build/reject test/test_reject.c build/libdldb89.a {{SIBLINGS}} -lsqlite3
	./build/reject

test: smoke reject

clean:
	rm -rf build
