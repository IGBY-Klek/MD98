# MD98 Makefile (MinGW)
#
# Usage:
#   mingw32-make           build md98.exe
#   mingw32-make clean     clean up
#
# Note: use an old Win32/ANSI toolchain; avoid pulling in modern CRT
# dependencies. The binary only links against the system-provided
# kernel32/user32/gdi32/comdlg32, which is fine for Windows 98.

CC   = gcc
RC   = windres
LIBS = -lcomdlg32 -lgdi32 -luser32 -lkernel32
OBJS = main.o markdown.o render.o lang.o md98_res.o

../bin/md98.exe: $(OBJS)
	-if not exist ..\bin mkdir ..\bin
	$(CC) -mwindows -o ../bin/md98.exe $(OBJS) $(LIBS) -s

main.o: main.c resource.h markdown.h render.h lang.h
	$(CC) -Os -Wall -c main.c

markdown.o: markdown.c markdown.h
	$(CC) -Os -Wall -c markdown.c

render.o: render.c render.h markdown.h
	$(CC) -Os -Wall -c render.c

lang.o: lang.c lang.h
	$(CC) -Os -Wall -c lang.c

md98_res.o: md98.rc resource.h
	$(RC) -O coff md98.rc md98_res.o

clean:
	del /Q *.o ..\bin\md98.exe 2>NUL
