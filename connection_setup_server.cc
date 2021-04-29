#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <unistd.h>
#include <iostream>

using namespace std;

#define RECEIVE_BUFFER_SIZE 10
#define PORT 8000

int main(void) {
    int s, s2, len;
    char receive_buffer[10];
    memset(receive_buffer, '0', RECEIVE_BUFFER_SIZE);

    if ((s = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket() failed\n");
        exit(1);
    }

    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(PORT);
    saddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(s, (struct sockaddr *)&saddr, sizeof(struct sockaddr)) == -1) {
        perror("bind() failed\n");
        exit(1);
    }

    if (listen(s, 5) == -1) {
        perror("listen() failed\n");
        exit(1);
    }

    while(true) {
        printf("Waiting for a connection...\n");
        struct sockaddr_in peer_saddr;
        socklen_t peer_len = sizeof(struct sockaddr);
        memset(&peer_saddr, 0, sizeof(peer_saddr));
        if ((s2 = accept(s, (struct sockaddr *)&peer_saddr, &peer_len)) == -1) {
            perror("accept() failed\n");
            exit(1);
        }

        int n;
        n = recv(s2, receive_buffer, 1, 0);
        std::cout << "Received size is " << n << std::endl;
        send(s2, receive_buffer, 1, 0);
        close(s2);
        }

  return 0;
}

