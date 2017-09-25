#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <fcntl.h>

#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>

#include "knock-common.h"

#define MAX_EVENTS 42

static struct config* config;

struct proxy {
    int epoll_queue;
    int socket;
    struct proxy* other;
    bool other_timed_out;
    bool closed;
    
    void* buffer;
    size_t buffer_size;

    void* buffer_filled;
    void* buffer_flushed;
};

static void non_block(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int add_to_queue(int epoll_queue, int socket, void* data) {
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
    ev.data.ptr = data;
	if (epoll_ctl(epoll_queue, EPOLL_CTL_ADD, socket, &ev) < 0) {
        perror("cannot connect epoll to just created socket");
        return -1;
    }
    return 0;
}

static void close_and_delete(struct proxy* this) {
    if (!this->closed) {
        epoll_ctl(this->epoll_queue, EPOLL_CTL_DEL, this->socket, NULL);
        close(this->socket);
        this->closed = true;
    }
}

static void close_and_free_proxy(struct proxy* info) {
    close_and_delete(info);
    if (info->other) {
        close_and_delete(info->other);
        info->other->other = NULL; // break the backwards pointer
        // TODO: if the other side is in the queue of stuff still to process, we can't free stuff, yet if not, we do have to cleanup stuff
    }
    if (info->buffer) {
        free(info->buffer);
    }
    free(info);
}

static void handle_timeout(struct proxy* info) {
    if (!info) {
        return;
    }
    if (info->other) {
        // fully established connection
        info->other->other_timed_out = true;
        if (!info->other_timed_out) {
            return; // the other didn't time out yet so lets wait for that
        }
    }
    close_and_free_proxy(info);
}

#define CHECK_ASYNC_RESULTS(__result, __proxy) { \ \
    if (__result == -1u && errno == EAGAIN) { \ // nothing left to read, so try again later
        return; \
    } \
    else if (__result == -1u) { \
        close_and_free_proxy(__proxy); \
        perror("Connection error?"); \
        return; \
    } \
    else if (__result == 0) { \ // connection closed
        close_and_free_proxy(__proxy); \
        return; \
    } \
} \


static void do_proxy(struct proxy* proxy) {
    bool full_buffer = false;
    bool full_flush = false;
    do {
        if (proxy->buffer_filled == proxy->buffer_flushed) {
            // buffer has been send to other side, so we can refill it
            size_t bytes_read = read(proxy->socket, proxy->buffer, proxy->buffer_size);
            CHECK_ASYNC_RESULTS(bytes_read, proxy);

            full_buffer = bytes_read == proxy->buffer_size;
            proxy->buffer_flushed = proxy->buffer;
            proxy->buffer_filled = proxy->buffer + bytes_read;
        }

        size_t to_write = proxy->buffer_filled - proxy->buffer_flushed;
        size_t bytes_written = write(proxy->other->socket, proxy->buffer_flushed, to_write);
        CHECK_ASYNC_RESULTS(bytes_written, proxy);

        proxy->buffer_filled += bytes_written;
        full_flush = proxy->buffer_filled == proxy->buffer_flushed;
    /*
     * It could be that the read or the write side has more to produce/consume, and we won't get a new event for that, so while either one was fully flushed, we try again
     */
    } while (full_flush || full_buffer); 
    
}

static int create_connection(int port) {
    struct sockaddr_in sin;

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(0x7f000001); /* 127.0.0.1 */
    sin.sin_port = htons(port); 

    int new_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (new_socket < 0) {
        return -1;
    }
    non_block(new_socket);
    int res = connect(new_socket, (struct sockaddr *)(&sin), sizeof(struct sockaddr_in));
    if (res < 0 && errno != EINPROGRESS) {
        perror("Error opening connection to back-end");
        close(new_socket);
        return -1;
    }
    return new_socket;
}

static void handle_new_data(struct proxy* proxy) {
    size_t bytes_read = read(proxy->socket, proxy->buffer, proxy->buffer_size);
    CHECK_ASYNC_RESULTS(bytes_read, proxy);

    proxy->buffer_flushed = proxy->buffer;
    proxy->buffer_filled = proxy->buffer + bytes_read;
	int port = config->normal_port;
    if (bytes_read >= config->knock_size) {
        if (memcmp(config->knock_value, proxy->buffer, config->knock_size) == 0) {
			port = config->ssh_port;
            proxy->buffer_flushed += config->knock_size; // skip the first knock-bytes
        }
    }
    int back_proxy_socket = create_connection(port);
    if (back_proxy_socket < 0) {
        close_and_free_proxy(proxy);
        return;
    }

    struct proxy *back_proxy = malloc(sizeof(struct proxy));
    back_proxy->epoll_queue = proxy->epoll_queue;
    back_proxy->socket = back_proxy_socket;
    back_proxy->other = proxy;
    proxy->other = back_proxy;
    back_proxy->other_timed_out = false;
    back_proxy->buffer = malloc(config->max_recv_buffer);
    back_proxy->buffer_size = config->max_recv_buffer;
    back_proxy->buffer_flushed = back_proxy->buffer_filled = NULL;

    if (add_to_queue(proxy->epoll_queue, back_proxy_socket, back_proxy) != 0) {
        close_and_free_proxy(proxy);
        return;
    }
}

static void process_other_events(struct epoll_event *ev) {
    struct proxy* info = (struct proxy*)ev->data.ptr;
    if (ev->events & (EPOLLERR|EPOLLHUP)) {
        handle_timeout(info);
        return ;
    }
    if (ev->events & EPOLLIN) {
        if (info->other) {
            do_proxy(info);
        }
        else {
             handle_new_data(info);
        }
    }
    if (ev->events & EPOLLOUT && info->other) {
        // this side of the proxy is ready for writing
        // so if the other side has something left to send, try it
        do_proxy(info->other);
    }
}

static int initialize(struct sockaddr_in *listen_address, int* epoll_queue, int* listen_socket) {
    *listen_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (*listen_socket < 0) {
		perror("cannot open socket");
        return -1;
    }
    int one = 1;
	if (setsockopt(*listen_socket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(int)) < 0) {
		perror("cannot set SO_REUSEADDR");
        return -1;
    }
	if (bind(*listen_socket, (struct sockaddr *)listen_address, sizeof(struct sockaddr_in)) < 0) {
		perror("cannot bind");
        return -1;
    }
	if (listen(*listen_socket, 20) < 0) {
		perror("cannot start listening");
        return -1;
    }
	non_block(*listen_socket);

    *epoll_queue = epoll_create1(EPOLL_CLOEXEC);
    if (*epoll_queue < 0) {
        perror("cannot create epoll queue");
        return -1;
    }

    return add_to_queue(*epoll_queue, *listen_socket, NULL);
}


int start(struct config* _config) {
    config = _config;
    _buffer = malloc(config->max_recv_buffer * sizeof(uint8_t));
    setvbuf(stdout, NULL, _IONBF, 0);

    struct sockaddr_in sin;
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = 0;
    sin.sin_port = htons(config->external_port);

    int epoll_queue;
    int listen_socket;
    int error = initialize(&sin, &epoll_queue, &listen_socket);
    if (error != 0) {
        return error;
    }

    struct epoll_event events[MAX_EVENTS];
    for (;;) {
        int nfds = epoll_wait(epoll_queue, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            perror("epoll_wait failure");
            return -1;
        }
        for (int n = 0; n < nfds; ++n) {
            struct epoll_event* current_event = &(events[n]);
            if (current_event->data.ptr == NULL) {
                if (current_event->events & EPOLLIN) {
                    // new connection
                    int conn_sock = accept(listen_socket, NULL, NULL);
                    if (conn_sock == -1) {
                        perror("cannot accept new connection");
                        return -1;
                    }
                    non_block(conn_sock);

                    struct proxy* data = malloc(sizeof(struct proxy));
                    data->epoll_queue = epoll_queue;
                    data->socket = conn_sock;
                    data->other = NULL;
                    data->other_timed_out = false;
                    data->welcome_size = 0;
                    if (add_to_queue(epoll_queue, conn_sock, data) != 0) {
                        close(conn_sock);
                        free(data);
                    }
                }
            } else {
                process_other_events(current_event);
            }
        }
    }
}
