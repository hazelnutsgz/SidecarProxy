#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
//CPP header
#include <iostream>
#include <chrono>
#include <unordered_map>
#include <sys/time.h>
#include <sys/resource.h>

const int SEND_BUFFER_SIZE = 1024;

int main(int argc, char* argv[])
{
    int s, t, len;
    struct sockaddr_un remote;
    char* send_buffer;

    struct rusage ru;

    int port = atoi(argv[1]);
    char const* ip_address = argv[2];


    send_buffer = (char* ) malloc(SEND_BUFFER_SIZE);
    if (!send_buffer) {
        perror("socket() failed\n");

        exit(1);
    }
    memset(send_buffer, 'c', SEND_BUFFER_SIZE);
    if ((s = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket() failed\n");
        exit(1);
    }
    
    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip_address, &servaddr.sin_addr) < 0) {
        perror("inet_pton");    
        exit(-1); 
    }


    
    auto before_all = std::chrono::high_resolution_clock::now();
    if (connect(s, (struct sockaddr *)&servaddr, sizeof(servaddr)) == -1) {
        perror("connect() failed\n");
        exit(1);
    }
    
    auto before_send = std::chrono::high_resolution_clock::now();
    int n = 0;
    if ((n = send(s, send_buffer, 1, 0)) < 0) {
        perror("send() failed\n");
        exit(1);
    }
    recv(s, send_buffer, 1, 0);
    std::cout << "receive size: " << n << std::endl;
    
    auto after_send = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> t_diff_ms = std::chrono::duration_cast<std::chrono::duration<double>>(after_send - before_send);
    std::cout << "Total time is " << t_diff_ms.count() << std::endl;

    close(s);

    return 0;
}
