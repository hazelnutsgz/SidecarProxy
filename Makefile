all:
	g++ -lpthread -ggdb -o myproxy proxy.cc	
	g++ io_uring_server.cpp -o ./io_uring_server -I./liburing/src/include/ -L./liburing/src/  -Wall -O2 -D_GNU_SOURCE -luring -lstdc++ -lpthread
	g++ -o connection_setup_client connection_setup_client.cc
	g++ -o connection_setup_server connection_setup_server.cc
clean:
	rm -rf myproxy