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
    uint8_t* welcome_bytes;
    size_t welcome_size;
};

static void non_block(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int add_to_queue(int epoll_queue, int socket, bool receive_out, void* data) {
    struct epoll_event ev;
    if (receive_out) {
        ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
    }
    else {
        ev.events = EPOLLIN | EPOLLET;
    }
    ev.data.ptr = data;
	if (epoll_ctl(epoll_queue, EPOLL_CTL_ADD, socket, &ev) < 0) {
        perror("cannot connect epoll to just created socket");
        return -1;
    }
    return 0;
}

static void close_and_delete(int epoll_queue, int socket) {
	epoll_ctl(epoll_queue, EPOLL_CTL_DEL, socket, NULL);
    close(socket);
}

static void close_and_free_proxy(struct proxy* info) {
    close_and_delete(info->epoll_queue, info->socket);
    if (info->other) {
        close_and_delete(info->epoll_queue, info->other->socket);
        if (info->other->welcome_size != 0) {
            free(info->other->welcome_bytes);
        }
    }
    if (info->welcome_size != 0) {
        free(info->welcome_bytes);
    }
    free(info);
}

static int handle_timeout(struct proxy* info) {
    if (info->other) {
        // fully established connection
        info->other->other_timed_out = true;
        if (!info->other_timed_out) {
            return 0; // the other didn't time out yet so lets wait for that
        }
    }
    close_and_free_proxy(info);
    return 0;
}

static void general_error(struct proxy* info) {
    close_and_free_proxy(info);
}


static uint8_t* _buffer;
static void do_proxy(struct proxy* proxy) {
    for (;;) {
        size_t bytes_read = read(proxy->socket, _buffer, config->max_recv_buffer);
        if (bytes_read == -1u) {
            if (errno != EAGAIN) {
                general_error(proxy);
            }
            return;
        }
        else if (bytes_read == 0) {
            // connection closed
            general_error(proxy);
            return;
        }
        else {
            size_t written = 0;
            while (written < bytes_read) {
                size_t current_written = write(proxy->other->socket, _buffer + written, bytes_read - written);
                if (current_written == 0 || current_written == -1u) {
                    general_error(proxy);
                    return;
                }
                written += current_written;
            }
        }
    }
}

static void handle_new_data(struct proxy* proxy) {
    size_t bytes_read = read(proxy->socket, _buffer, config->max_recv_buffer);
    if (bytes_read == -1u) {
        if (errno != EAGAIN) {
            general_error(proxy);
            return;
        }
        return;
    }
    else if (bytes_read == 0) {
        // connection closed
        general_error(proxy);
        return;
    }

    size_t start_pos = 0;
	uint32_t port = config->normal_port;
    if (bytes_read >= config->knock_size) {
        if (memcmp(config->knock_value, _buffer, config->knock_size) == 0) {
			port = config->ssh_port;
            start_pos = config->knock_size;
            bytes_read -= config->knock_size;
        }
    }

    struct sockaddr_in sin;

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(0x7f000001); /* 127.0.0.1 */
    sin.sin_port = htons(port); 

    int back_proxy_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (back_proxy_socket < 0) {
        general_error(proxy);
        return;
    }
    non_block(back_proxy_socket);
    int res = connect(back_proxy_socket, (struct sockaddr *)(&sin), sizeof(struct sockaddr_in));
    if (res < 0 && errno != EINPROGRESS) {
        perror("Error connection to backend");
        general_error(proxy);
        close(back_proxy_socket);
        return;
    }

    struct proxy *back_proxy = malloc(sizeof(struct proxy));
    back_proxy->epoll_queue = proxy->epoll_queue;
    back_proxy->socket = back_proxy_socket;
    back_proxy->other = proxy;
    proxy->other = back_proxy;
    back_proxy->other_timed_out = false;
    back_proxy->welcome_size = (bytes_read - start_pos) * sizeof(uint8_t);
    back_proxy->welcome_bytes = malloc(back_proxy->welcome_size);
    memcpy(_buffer + start_pos, back_proxy->welcome_bytes, back_proxy->welcome_size);

    if (add_to_queue(proxy->epoll_queue, back_proxy_socket, true, back_proxy) != 0) {
        general_error(proxy);
        return;
    }
}

static void handle_back_connection_finished(struct proxy* proxy) {
    size_t to_write = proxy->welcome_size;
    if (to_write == -1u) {
        perror("Already initialized");
        return;
    }
    size_t written = 0;
    while (written < to_write) {
        size_t current_written = write(proxy->socket, proxy->welcome_bytes + written, to_write - written);
        if (current_written <= 0) {
            general_error(proxy);
            return;
        }
        written += current_written;
    }
    proxy->welcome_size = 0;
    free(proxy->welcome_bytes);

    // now switch to only getting events for new bytes
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = proxy;
    epoll_ctl(proxy->epoll_queue, EPOLL_CTL_MOD, proxy->socket, &ev);
}

static void process_other_events(struct epoll_event *ev) {
    struct proxy* info = (struct proxy*)ev->data.ptr;
    if (ev->events & (EPOLLERR|EPOLLHUP)) {
        handle_timeout(info);
    }
    else if (ev->events & EPOLLIN) {
        if (info->other) {
            if (info->welcome_size != 0 && ev->events & EPOLLOUT) {
                // server sends data before it got any
                handle_back_connection_finished(info);
            }
            do_proxy(info);
        }
        else {
             handle_new_data(info);
        }
    }
    else if (ev->events & EPOLLOUT) {
        if (info->welcome_bytes) {
            handle_back_connection_finished(info);
        }
        else {
            perror("Not sure what to do with this event");
        }
    }
    else {
        perror("Unsupported event?");
    }
}

static int initialize(struct sockaddr_in *listen_address, int* epoll_queue, int* listen_socket) {
    *listen_socket = socket(AF_INET, SOCK_STREAM, 0);

    int one = 1;
	if (setsockopt(*listen_socket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(int)) < 0) {
		perror("cannot set SO_REUSEADDR");
        return -1;
    }
	if (bind(*listen_socket,(struct sockaddr *)listen_address, sizeof(struct sockaddr_in)) < 0) {
		perror("cannot bind");
        return -1;
    }
	if (listen(*listen_socket, 20) < 0) {
		perror("cannot start listening");
        return -1;
    }
	non_block(*listen_socket);

    *epoll_queue = epoll_create1(FD_CLOEXEC);
    if (*epoll_queue < 0) {
        perror("cannot create epoll queue");
        return -1;
    }

    return add_to_queue(*epoll_queue, *listen_socket, false, NULL);
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

                    struct proxy* data = malloc(sizeof(struct proxy));
                    data->epoll_queue = epoll_queue;
                    data->socket = conn_sock;
                    data->other = NULL;
                    data->other_timed_out = false;
                    data->welcome_size = 0;
                    if (add_to_queue(epoll_queue, conn_sock, false, data) != 0) {
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
