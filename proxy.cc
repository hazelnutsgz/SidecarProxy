// std
#include <stdio.h>
#include <map>
#include <vector>
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <string>
// network
#include <arpa/inet.h>
#include <sys/socket.h>

// system
#include <signal.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <linux/errqueue.h>

// common
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <memory>
#include <string.h>
#include <sched.h> 

#include <signal.h>

int zc_flag = 0;

/* Catch Signal Handler functio */
void signal_callback_handler(int signum){

        printf("Caught signal SIGPIPE %d\n",signum);
}


const int MAX_EPOLL_SIZE = 10000;
const int PACKET_BUFFER_SIZE = 1000000000;


class BufferPool {
public:
    char* buffer;
    int capacity;
    int current; // position of send(write)
    int end; // position of recv(read)
    int buffer_end;

    BufferPool(int);
    BufferPool();
    ~BufferPool();
};


class Connection {
public:
    struct sockaddr_in listen_address;
	struct sockaddr_in target_address;
    int listen_fd;
    int accepted_fd;
    BufferPool buffer_pool;

    int listen_port;
    char const* upstream_ip;
    int upstream_port;
    int upstream_fd;
    Connection() {};
    Connection(int listen_fd, int accepted_fd, \
            char const* upstream_ip, int upstream_port, int upstream_fd): 
                listen_fd(listen_fd), accepted_fd(accepted_fd), \
                    upstream_ip(upstream_ip), upstream_port(upstream_port), upstream_fd(upstream_fd) {}
};

using ConnectionPtr = std::shared_ptr<Connection>;


class Upstream {
public:
    int upstream_port;
    char const* upstream_ip;
    Upstream(int upstream_port, char const* upstream_ip): upstream_ip(upstream_ip), upstream_port(upstream_port) {};
    Upstream() {};
    ~Upstream() {};
};

using UpstreamPtr = std::shared_ptr<Upstream>;


class Config {
public:
    int listen_port;
    char const* upstream_ip;
    int upstream_port;
    Config(int listen_port, char const* upstream_ip, int upstream_port):listen_port(listen_port), upstream_ip(upstream_ip), upstream_port(upstream_port) {};
};

class Worker {
private:
    int epoll_fd;
    std::unordered_map<int, ConnectionPtr> connection_mapping; //fd: connection
    std::unordered_map<int, int> fd_mapping;
    std::unordered_map<int, UpstreamPtr> listen_mapping; //listen fd : upstream
    std::vector<Config>& configs;
    std::unordered_map<int, bool> zerocopy_mapping; //check whether fd is zerocopy fd.
    std::unordered_map<int, int> zerocopy_runtime;
    // std::unordered_map<int, bool> write_next_time;
public:
    int fail_read_count{0};
    int fail_write_count{0};
    int cpu;
    pthread_t pid;
    Worker(std::vector<Config>& configs, int cpu): configs(configs), cpu(cpu) {};
    ~Worker() {};
    int Serve();
    int buildListener(Config&);
    int onConnection(std::unordered_map<int, UpstreamPtr>::iterator& iter);
    int onDataIn(int fd);
    int onDataOut(int fd);
    int onDataError(int fd);
    int onDataClose(int fd);
    bool setZeroCopy(int fd);
};

class Handler {
private:
    std::vector<Config> configs;
    std::vector<Worker> workers;
    static int number;
public:
    // void Handle();
    int addProxy(int listen_port, char const* upstream_ip, int upstream_port);
    int startWorkers(int worker_number);
    static void* threadProcess(void * arg);
};


bool Worker::setZeroCopy(int fd) {
    bool zero_flag = false;
    int code;

    int optval;
    socklen_t optlen = sizeof(optval);
    if (getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &optval, &optlen) < 0) {
        std::cout << "SO_DOMAIN" << std::endl;
        exit(1);
    }
    if (AF_UNIX == optval) {
        zero_flag = true;
    } else {
        struct sockaddr_storage addr;
        socklen_t addr_len = sizeof(addr);
        if (getpeername(fd, (struct sockaddr*) &addr, &addr_len) < 0) {
            std::cout << "SO_PEER" << std::endl;
            exit(1);
        }
        struct sockaddr_in *s = (struct sockaddr_in *)&addr;
        if (strcmp(inet_ntoa(s->sin_addr), "127.0.0.1")) {
            zero_flag = true;
        }
    }

    if (zero_flag == true) {
      int zerocopy = 1;
      code = ::setsockopt(fd, SOL_SOCKET, SO_ZEROCOPY, &zerocopy, sizeof(zerocopy));
      if (code != 0) {
        std::cout << fd << " " <<  errno << std::endl;
        std::cout << code << std::endl;
        printf("setsockopt failed ...\n");
        exit(0);
      }
    }
    zerocopy_mapping[fd] = zero_flag;
    zerocopy_runtime[fd] = -1;
    std::cout << fd << " " << zero_flag << std::endl;
    return zero_flag;
}




int Worker::onConnection(std::unordered_map<int, UpstreamPtr>::iterator& iter) {
    UpstreamPtr upstream = iter->second;
    int listen_fd = iter->first;
    
    //accept
    struct sockaddr_in _addr;
	int socklen = sizeof(sockaddr_in);
	int accept_fd = accept(listen_fd, (struct sockaddr *)&_addr, (socklen_t*) & socklen);

    if (accept_fd < 0) {
        std::cout << "DDDDD" << std::endl;
    }
    setZeroCopy(accept_fd);
    //Connection Upstream
    struct sockaddr_in target_address;
    target_address.sin_family = AF_INET;
    target_address.sin_port = htons(upstream->upstream_port);
    if (inet_pton(AF_INET, upstream->upstream_ip, &target_address.sin_addr) <= 0) { return -1; }

    const int flag = 1;
    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    // if(setsockopt(client_fd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) < 0) { return -1; }
    if(connect(client_fd,(struct sockaddr*) &target_address, sizeof(struct sockaddr_in)) < 0) { return -1; }
    setZeroCopy(client_fd);

    //New Connection;
    auto connection_downstream = std::make_shared<Connection>(listen_fd, accept_fd, upstream->upstream_ip, upstream->upstream_port, client_fd);
    connection_mapping[accept_fd] = connection_downstream;

    auto connection_upstream = std::make_shared<Connection>(listen_fd, accept_fd, upstream->upstream_ip, upstream->upstream_port, client_fd);
	connection_mapping[client_fd] = connection_upstream;

    fd_mapping[accept_fd] = client_fd;
    fd_mapping[client_fd] = accept_fd;

	int flags;
	flags = fcntl(accept_fd, F_GETFL, 0);    
	fcntl(accept_fd, F_SETFL, flags | O_NONBLOCK);
	flags = fcntl(client_fd, F_GETFL, 0);    
	fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

	// event
	struct epoll_event ev;
	ev.data.fd = accept_fd;
	ev.events = EPOLLIN | EPOLLERR | EPOLLRDHUP;
	epoll_ctl(epoll_fd, EPOLL_CTL_ADD, accept_fd, &ev);
	ev.data.fd = client_fd;
	ev.events = EPOLLIN | EPOLLERR | EPOLLRDHUP;
	epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);
	
	printf("Thread ID: %d OPEN: %d <--> %d\n", pid, accept_fd, client_fd);
	return 0;
}

int Worker::buildListener(Config& config) {

    struct sockaddr_in listen_address;
    listen_address.sin_family = AF_INET;
    listen_address.sin_port = htons(config.listen_port);

    listen_address.sin_addr.s_addr = htonl(INADDR_ANY);

	// listen
	const int flag = 1;
	int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &flag, sizeof(flag)) < 0) { return -1; }
    if (bind(listen_fd,(const struct sockaddr*)&(listen_address), sizeof(struct sockaddr_in)) < 0) { return -1; }
    if (listen(listen_fd, SOMAXCONN) < 0) { return -1; }

    listen_mapping[listen_fd] = std::make_shared<Upstream>(config.upstream_port, config.upstream_ip);;
	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.fd  = listen_fd;
	epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);

    return 0;
}

int Worker::onDataClose(int fd) { 
    std::cout << "Errn: " << fd << std::endl;
    
    // int opposite  = fd_mapping[fd]; 
    // fd_mapping.erase(fd);
    // fd_mapping.erase(opposite); 
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
    return -1;
}

int Worker::onDataIn(int in_fd) {
    // if (zerocopy_runtime[in_fd] > 0) {
    //     std::cout << in_fd << "The memory is in used...On Data In" << std::endl;
    //     return -1;
    // }
    ConnectionPtr connection = connection_mapping[in_fd];
	// recv

    char* start = connection->buffer_pool.end+connection->buffer_pool.buffer;
    int size = connection->buffer_pool.capacity-connection->buffer_pool.end;
    // bool eagain_flag = false;
    if (connection->buffer_pool.end >= connection->buffer_pool.capacity) {
        std::cout << "buffer overflow, blocking this socket read" << std::endl;
        // fail_read_count += 1;
        
        // start = connection->buffer_pool.buffer_start;
        // size = connection->buffer_pool.current-connection->buffer_pool.buffer_start;
        return 0;
        // exit(1);
    }
    // std::cout << "Try to Read 1 " << connection->buffer_pool.buffer_end-connection->buffer_pool.end << " bytes, from " << in_fd << std::endl;
    int ret = recv(in_fd, start, size, 0);
    // std::cout << "Try to Read 2 " << connection->buffer_pool.buffer_end-connection->buffer_pool.end << " bytes, from " << in_fd << std::endl;
    if (ret <= 0) {
        if (errno == EAGAIN) {
            // std::cout << "We successfully drained this read buffer" << std::endl;
            return 0;
        } else {
            std::cout << "Errno:" << errno << " " << in_fd << std::endl;
            onDataClose(in_fd);
            return -1;
        }
    }
    connection->buffer_pool.end += ret;
    // std::cout << "Read " << ret << " bytes, from " << in_fd << std::endl;
    int out_fd = fd_mapping[in_fd];
    // std::cout << "Try to write to " << out_fd << " " << zerocopy_runtime[out_fd] << std::endl;
    onDataOut(out_fd);

    // }
}

int Worker::onDataOut(int out_fd) {
    if (zerocopy_runtime[out_fd] > 0) {
        // std::cout << out_fd << "The memory is in used...On Data Out" << std::endl;
        fail_write_count += 1;
        return 1;
    }

    bool finished = false;
    int ret;
    bool zerocopy_flag = zerocopy_mapping[out_fd];
    ConnectionPtr connection = connection_mapping[fd_mapping[out_fd]];
    // std::cout << "should write..." << connection->buffer_pool.end - connection->buffer_pool.current << std::endl;
	while(connection->buffer_pool.current < connection->buffer_pool.end) {
        bool should_send = zc_flag && ((connection->buffer_pool.end-connection->buffer_pool.current) > 1000) && zerocopy_flag;
		ret = send(out_fd, connection->buffer_pool.current+connection->buffer_pool.buffer, connection->buffer_pool.end-connection->buffer_pool.current, should_send ? MSG_ZEROCOPY: 0);
        // std::cout << zc_flag << zerocopy_flag << ":" << ret << ":" << should_send << ":" << connection->buffer_pool.end-connection->buffer_pool.current << std::endl;
		if (ret <= 0) {
			if (errno == EAGAIN) {
                // std::cout << "OnDataOut: EAGAIN" << errno << std::endl; 
				break;
			} else {
                std::cout << "OnDataOut: errono is " << errno << std::endl; 
				return -1;
			}
		} else {
            if (should_send) {
                // std::cout << "zero send" << out_fd << " " << ret << " " <<connection->buffer_pool.current << ":" << connection->buffer_pool.end << std::endl;
                zerocopy_runtime[out_fd] = ret;
                return -1;
            } else {
                // Normal
                // std::cout << "Truely write " << ret << std::endl; 
                connection->buffer_pool.current += ret;
                if (connection->buffer_pool.current == connection->buffer_pool.end) {
                    connection->buffer_pool.current = 0;
                    connection->buffer_pool.end = 0;
                    struct epoll_event ev;
                    ev.data.fd = out_fd;
                    ev.events = EPOLLIN;
                    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, out_fd, &ev);
                    // write_next_time[out_fd] = false;
                } else {
                    struct epoll_event ev;
                    ev.data.fd = out_fd;
                    ev.events = EPOLLIN|EPOLLOUT;
                    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, out_fd, &ev);
                }
            } 
		}
	}

    
    return 0;
}

int Worker::onDataError(int fd) {
    // if (fd_mapping.find(fd) == fd_mapping.end()) {
    //     std::cout << fd << " has been closed" << std::endl; 
    //     return -1;
    // }
    struct msghdr cmsg={0};
    char control[100];
    struct cmsghdr *cm;
    struct sock_extended_err *serr;
    cmsg.msg_control = control;
    cmsg.msg_controllen = sizeof(control);
    int ret = recvmsg(fd, &cmsg, MSG_ERRQUEUE);
    if(ret == -1){
        std::cout << "recvmsg error" << std::endl;
        onDataClose(fd);
        return 0;
    }
    cm = CMSG_FIRSTHDR(&cmsg);
    // printf("cmsg_level = %p cmsg_type =%p\n",cm->cmsg_level,cm->cmsg_type);
    // if (cm->cmsg_level != SOL_IP && cm->cmsg_type != IP_RECVERR) {
    //     std::cout << "error" << std::endl;
    //     onDataClose(fd);
    //     return 0;
    // }

    serr = (sock_extended_err* )CMSG_DATA(cm);
    if(serr->ee_errno != 0||
            serr->ee_origin != SO_EE_ORIGIN_ZEROCOPY){
        std::cout << "serr..\n" << std::endl;
        onDataClose(fd);
        return 0;
    }
    // printf("complete: %u .. %u\n",serr->ee_info, serr->ee_data);
    // printf("i can free the buff which send in seq %u .. %u\n",serr->ee_info,serr->ee_data);

    ConnectionPtr connection = connection_mapping[fd_mapping[fd]];

    // std::cout << "Free memory for " << fd << " " << zerocopy_runtime[fd] << " " << connection->buffer_pool.end << std::endl;
    connection->buffer_pool.current += zerocopy_runtime[fd];
    zerocopy_runtime[fd] = -1;
    if (connection->buffer_pool.current == connection->buffer_pool.end) {
        connection->buffer_pool.current = 0;
	    connection->buffer_pool.end = 0;
    } else {
        onDataOut(fd);
    }
}



int Worker::Serve() {
    epoll_fd = epoll_create(MAX_EPOLL_SIZE); // epoll_create(int size); size is no longer used

    for (auto& config: configs) {
        buildListener(config);
    }

    struct epoll_event events[MAX_EPOLL_SIZE];
	int count = 0;
	
    //serving
	while(true) {
		count = epoll_wait(epoll_fd, events, MAX_EPOLL_SIZE, -1);
		if (count < 0) {
			if (errno == EINTR) {
				continue;
			} else {
				printf("epoll error\n");
				return -1;
			}
		}
		for (int i = 0; i < count; i ++) {
            // std::cout << events[i].data.fd << std::endl;
            
			auto it = listen_mapping.find(events[i].data.fd);
		    if (it != listen_mapping.end()) {
				onConnection(it);
			} else {
                if(events[i].events & EPOLLERR){
                    onDataError(events[i].data.fd);
                }
                if (events[i].events & EPOLLRDHUP ) {
                    std::cout << "closed" << std::endl;
					onDataClose(events[i].data.fd);
				}
				if (events[i].events & EPOLLOUT) {
					onDataOut(events[i].data.fd);
				}
				if (events[i].events & EPOLLIN) {
					onDataIn(events[i].data.fd);
				}
			}
		}
	}
}

BufferPool::BufferPool() {
    capacity = PACKET_BUFFER_SIZE;
    buffer = new char[capacity];
    current = 0;
    end = 0;
    buffer_end = capacity-1;
}

BufferPool::BufferPool(int capacity) : capacity(capacity) {
    buffer = new char[capacity];
    current = 0;
    end = 0;
    buffer_end = capacity-1;
}

BufferPool::~BufferPool() {
    delete []buffer;
    buffer = nullptr;
}

// void Handler::Handle() {
//     while(true) {
        
//     }
// }

int Handler::addProxy(int listen_port, char const* upstream_ip, int upstream_port) {
    Config config(listen_port, upstream_ip, upstream_port);
    configs.push_back(config);
    return 0;
}

void* Handler::threadProcess(void * arg) {

    Worker* worker = (Worker *) arg;

    worker->Serve();
    return NULL;
}
int Handler::startWorkers(int worker_number) {
    std::vector<Worker*> workers;
    for (int i = 0; i < worker_number; ++i) {
        Worker* worker = new Worker(configs, i);
        pthread_create(&(worker->pid), NULL, &threadProcess, worker); // remember to pthread_join
        workers.push_back(worker);
    }
    for (int i = 0; i < worker_number; i++) {
        pthread_join(workers[i]->pid, NULL);  
    }
    std::cout << "LLLL" << std::endl;
}

int main(int argc, char const *argv[])
{
    signal(SIGPIPE, signal_callback_handler);
    Handler handler;
    int listen_port = atoi(argv[1]);
    char const* upstream_ip = argv[2];
    int upstream_port =atoi(argv[3]);
    int workers = atoi(argv[4]);
    zc_flag = atoi(argv[5]);
	// handler.addProxy(8080, "127.0.0.1", 80);
    std::cout << listen_port << upstream_ip << upstream_port << std::endl;
	handler.addProxy(listen_port, upstream_ip, upstream_port);
    // handler.addUDSProxy("/data00/guozhen/memcached.sock", upstream_ip, upstream_port);
    handler.startWorkers(workers);
	// handler.Handle();
    return 0;
}