.PHONY: all clean

all: wordlist.exe

wordlist.exe: main.cpp wordlist.hpp
	g++ -std=c++17 -g3 -Wall -Wextra -Wpedantic -o wordlist.exe main.cpp

clean:
	rm -f wordlist.exe
