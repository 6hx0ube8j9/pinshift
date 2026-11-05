CC      ?= gcc
WINDRES ?= windres
CFLAGS  := -O3 -s -mwindows -Wall -DUNICODE -D_UNICODE
LIBS    := -lkernel32 -luser32 -lshell32 -ladvapi32 -lgdi32

TARGET  := pinshift.exe
SRCS    := core.c
RC      := res.rc
OBJS    := core.o res.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

core.o: resource.h
res.o: $(RC) resource.h manifest.xml
	$(WINDRES) -i $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
