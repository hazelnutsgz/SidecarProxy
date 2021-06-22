all:
	g++ -lpthread -ggdb -o myproxy proxy.cc	
	g++ -lpthread -ggdb -o et ET.cc
##	g++ -lpthread -ggdb -o traffic_client traffic_client.cc	
##	g++ -lpthread -ggdb -o traffic_server traffic_server.cc	
	g++ io_uring_server.cpp -o ./io_uring_server -I./liburing/src/include/ -L./liburing/src/  -Wall -O2 -D_GNU_SOURCE -luring -lstdc++ -lpthread
	g++ io_uring_server_uds.cpp -o ./io_uring_server_uds -I./liburing/src/include/ -L./liburing/src/  -Wall -O2 -D_GNU_SOURCE -luring -lstdc++ -lpthread
	g++ -o connection_setup_client connection_setup_client.cc
	g++ -o connection_setup_server connection_setup_server.cc
clean:
	rm -rf myproxy
