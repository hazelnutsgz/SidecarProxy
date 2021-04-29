#include <liburing.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <errno.h>

#include <stdlib.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <pthread.h>
#include <vector>
#include <arpa/inet.h>
#include <unordered_map>
#include <iostream>

#define MAX_CONNECTIONS 1024
#define BACKLOG 512
#define MAX_MESSAGE_LEN 2048
#define IORING_FEAT_FAST_POLL (1U << 5)

void add_accept(struct io_uring *ring, int fd, struct sockaddr *client_addr, socklen_t *client_len, unsigned flags);
void add_socket_read(struct io_uring* ring, int fd, size_t size, unsigned flags);
void add_socket_write(struct io_uring* ring, int fd, size_t size, unsigned flags, std::unordered_map<int,int>& mapp);

enum {
    ACCEPT,
    POLL_LISTEN,
    POLL_NEW_CONNECTION,
    READ,
    WRITE,
};

typedef struct conn_info
{
    unsigned fd;
    unsigned type;
} conn_info;

conn_info conns[MAX_CONNECTIONS];
char bufs[MAX_CONNECTIONS][MAX_MESSAGE_LEN];

int set_nonblocking(int fd) {
    int flags;
	flags = fcntl(fd, F_GETFL, 0);    
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int connect_to_upstream(char* s, int port) {
    struct sockaddr_in target_address;
    target_address.sin_family = AF_INET;
    target_address.sin_port = htons(port);
    if (inet_pton(AF_INET, s, &target_address.sin_addr) <= 0) { return -1; }

    const int flag = 1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(fd,(struct sockaddr*) &target_address, sizeof(struct sockaddr_in)) < 0) { return -1; }
    return fd;
}

void* thread_fn(void* arg)
{

    // some variables we need
    int port = *((int*)arg);
    struct sockaddr_in serv_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);


    // setup socket
    int sock_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    const int val = 1;
    setsockopt(sock_listen_fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
    setsockopt(sock_listen_fd, SOL_SOCKET, SO_REUSEPORT, &val, sizeof(val));

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    serv_addr.sin_addr.s_addr = INADDR_ANY;


    // bind and listen
    if (bind(sock_listen_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        perror("Error binding socket..\n");
        exit(1);
    }
    if (listen(sock_listen_fd, BACKLOG) < 0)
    {
        perror("Error listening..\n");
        exit(1);
    }
    printf("io_uring echo server listening for connections on port: %d\n", port);


    // initialize io_uring
    struct io_uring_params params;
    struct io_uring ring;
    memset(&params, 0, sizeof(params));
    // params.flags |= IORING_SETUP_SQPOLL;
    // params.flags |= IORING_SETUP_IOPOLL;
    // params.sq_thread_idle = 2000;

    if (io_uring_queue_init_params(4096, &ring, &params) < 0)
    {
        perror("io_uring_init_failed...\n");
        exit(1);
    }

    if (!(params.features & IORING_FEAT_FAST_POLL))
    {
        printf("IORING_FEAT_FAST_POLL not available in the kernel, quiting...\n");
        exit(0);
    }
    // io_uring_register_files(&ring, &sock_listen_fd, 1);

    // add first accept sqe to monitor for new incoming connections
    add_accept(&ring, sock_listen_fd, (struct sockaddr *)&client_addr, &client_len, 0);

    std::unordered_map<int, int> mapping;
    // start event loop
    while (1)
    {
        struct io_uring_cqe *cqe;
        int ret;

        // tell kernel we have put a sqe on the submission ring
        io_uring_submit(&ring);

        // wait for new cqe to become available
        ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret != 0)
        {
            perror("Error io_uring_wait_cqe\n");
            exit(1);
        }

        // check how many cqe's are on the cqe ring at this moment
        struct io_uring_cqe *cqes[BACKLOG];
        int cqe_count = io_uring_peek_batch_cqe(&ring, cqes, sizeof(cqes) / sizeof(cqes[0]));

        // go through all the cqe's
        for (int i = 0; i < cqe_count; ++i)
        {
            struct io_uring_cqe *cqe = cqes[i];
            struct conn_info *user_data = (struct conn_info *)io_uring_cqe_get_data(cqe);
            int type = user_data->type;

            if (type == ACCEPT)
            {
                std::cout << "accept:" << cqe->res << std::endl;
                int sock_conn_fd = cqe->res;
                io_uring_cqe_seen(&ring, cqe);
                // set_nonblocking(sock_conn_fd);
                // new connected client; read data from socket and re-add accept to monitor for new connections
                add_socket_read(&ring, sock_conn_fd, MAX_MESSAGE_LEN, 0);
                // io_uring_register_files(&ring, &sock_conn_fd, 1);
                add_accept(&ring, sock_listen_fd, (struct sockaddr *)&client_addr, &client_len, 0);
                int client_fd = connect_to_upstream("10.198.60.44", 7999);
                //std::cout << "accept2:" << cqe->res << std::endl;
                mapping[client_fd] = sock_conn_fd;
                mapping[sock_conn_fd] = client_fd;
                // set_nonblocking(client_fd);

            }
            else if (type == READ)
            {
                int bytes_read = cqe->res;
                // std::cout << user_data->fd << ":has read:" << bytes_read << std::endl;
                if (bytes_read <= 0)
                {
                    // no bytes available on socket, client must be disconnected
                    io_uring_cqe_seen(&ring, cqe);
                    shutdown(user_data->fd, SHUT_RDWR);
                }
                else
                {
                    // bytes have been read into bufs, now add write to socket sqe
                    io_uring_cqe_seen(&ring, cqe);
                    add_socket_write(&ring, mapping[user_data->fd], bytes_read, 0, mapping);
                }
            }
            else if (type == WRITE)
            {
                //std::cout << user_data->fd << ":has write:" << cqe->res << std::endl;
                // write to socket completed, re-add socket read
                io_uring_cqe_seen(&ring, cqe);
                add_socket_read(&ring, user_data->fd, MAX_MESSAGE_LEN, 0);
            }
        }
    }
}

void add_accept(struct io_uring *ring, int fd, struct sockaddr *client_addr, socklen_t *client_len, unsigned flags)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    io_uring_prep_accept(sqe, fd, client_addr, client_len, 0);
    io_uring_sqe_set_flags(sqe, flags);

    conn_info *conn_i = &conns[fd];
    conn_i->fd = fd;
    conn_i->type = ACCEPT;

    io_uring_sqe_set_data(sqe, conn_i);
}

void add_socket_read(struct io_uring *ring, int fd, size_t size, unsigned flags)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    io_uring_prep_recv(sqe, fd, &bufs[fd], size, 0);
    io_uring_sqe_set_flags(sqe, flags);

    conn_info *conn_i = &conns[fd];
    conn_i->fd = fd;
    conn_i->type = READ;

    io_uring_sqe_set_data(sqe, conn_i);
}

void add_socket_write(struct io_uring *ring, int fd, size_t size, unsigned flags, std::unordered_map<int,int>& mapp)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    io_uring_prep_send(sqe, fd, &bufs[mapp[fd]], size, 0);
    io_uring_sqe_set_flags(sqe, flags);

    conn_info *conn_i = &conns[fd];
    conn_i->fd = fd;
    conn_i->type = WRITE;
    io_uring_sqe_set_data(sqe, conn_i);
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        printf("Please give a port number: ./io_uring_echo_server [port]\n");
        exit(0);
    }

    std::vector<pthread_t> pid_list;
    int portno = strtol(argv[1], NULL, 10);
    int worker_number = strtol(argv[2], NULL, 10);
    for (int i = 0; i < worker_number; ++i) {
        pthread_t tid;
        pthread_create(&tid, NULL, &thread_fn, &portno); // remember to pthread_join
        pid_list.push_back(tid);
    }
    for (int i = 0; i < worker_number; i++) {
        pthread_join(pid_list[i], NULL);  
    }

}
