CC = gcc
RC = windres
CFLAGS = -O3 -s -mwindows -Wall -DUNICODE -D_UNICODE
LIBS = -lkernel32 -luser32 -lshell32 -ladvapi32 -lgdi32

TARGET = pinshift.exe
OBJS = main.o resource.o

all: $(TARGET)

resource.o: resource.rc resource.h res/quan.ico res/shuang.ico
	$(RC) -i resource.rc -o resource.o

main.o: main.c resource.h
	$(CC) $(CFLAGS) -c main.c -o main.o

$(TARGET): $(OBJS)
	$(CC) $(OBJS) $(CFLAGS) $(LIBS) -o $(TARGET)

clean:
	rm -f $(OBJS) $(TARGET)
