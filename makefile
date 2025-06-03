jmytc : jmytc.o hash.o lzw.o
	cc -Og -Wall -o jmytc jmytc.o hash.o lzw.o -lcurl

jmytc.o : jmytc.c
	cc -c jmytc.c

hash.o : hash.c
	cc -c hash.c

lzw.o : lzw.c
	cc -c lzw.c

