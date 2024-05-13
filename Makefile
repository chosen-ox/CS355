# Compiler and compile options
CC = gcc
CFLAGS = -Wall -fpic -c -I$(SRCDIR)
LFLAGS = -shared
LIBNAME = libuserthread
SRCDIR = .
TESTDIR = ./fcfs-sjf

# Always build the shared library, but allow specifying a test program to build and run
all: libuserthread.so

# Compile the thread library into an object files
userthread.o: $(SRCDIR)/userthread.c
	$(CC) $(CFLAGS) $< -o $@

# Link the object file into a shared library
libuserthread.so: userthread.o
	$(CC) $(LFLAGS) -o $(LIBNAME).so userthread.o

# Rule to compile any .c file in the xchen4 directory and link it with the shared library
%: $(TESTDIR)/%.c libuserthread.so
	$(CC) -o $@ $< -L. -luserthread -Wl,-rpath,. -I$(SRCDIR)

# Clean the build directory
clean:
	find . -type f ! -name '*.c' ! -name '*.so' ! -name '*.h' ! -name 'Makefile' ! -name 'README.txt' ! -path './$(TESTDIR)/*' -delete