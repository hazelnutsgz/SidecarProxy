all:
	g++ -lpthread -ggdb -o myproxy proxy.cc	
	g++ io_uring_server.cpp -o ./io_uring_server -I./liburing/src/include/ -L./liburing/src/  -Wall -O2 -D_GNU_SOURCE -luring -lstdc++ -lpthread
clean:
	rm -rf myproxy