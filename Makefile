CC = gcc
RC = windres
CFLAGS = -O3 -s -mwindows -Wall -DUNICODE -D_UNICODE
LIBS = -lkernel32 -luser32 -lshell32 -ladvapi32 -lgdi32

TARGET = pinshift.exe
OBJS = core.o res.o

all: $(TARGET)

res.o: res.rc resource.h manifest.xml res/app.ico res/quan.ico res/shuang.ico
	$(RC) -i res.rc -o res.o

core.o: core.c resource.h
	$(CC) $(CFLAGS) -c core.c -o core.o

$(TARGET): $(OBJS)
	$(CC) $(OBJS) $(CFLAGS) $(LIBS) -o $(TARGET)

clean:
	rm -f $(OBJS) $(TARGET)
