libraries = -lrt -ljson-c -lcurl
objects = main.o
executable = procscrape

procscape : main.o
	gcc -Wl,-rpath,./lib -g -o $(executable) $(objects) $(libraries)

main.o : main.c
	gcc -g -c -o main.o main.c

clean :
	rm $(executable) $(objects)

