#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>


#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <fcntl.h>

#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>

#include "knock-common.h"
#include "debug.h"
#include "common.h"

#define MAX_EVENTS 42


static struct config* config;

static void* to_free[MAX_EVENTS * 4]; // proxy 2 way plus buffers
static size_t free_index = 0;
#define SCHEDULE_FREE(__p) to_free[free_index++] = __p

struct proxy;

typedef void (*ProxyCall)(struct proxy* this);

struct proxy {
    int epoll_queue;
    int socket;
    struct proxy* other;
    bool timed_out;
    bool closed;

    uint8_t* buffer;
    size_t buffer_size;

    uint8_t* buffer_filled;
    uint8_t* buffer_flushed;

    ProxyCall out_op;
    ProxyCall in_op;

    struct timespec last_recieved;
    struct proxy* next;
    struct proxy* previous;
};

static struct proxy* timeout_queue_head = NULL;
static struct proxy* timeout_queue_tail = NULL;

static struct timespec current_time;

static void touch(struct proxy* this) {
    this->last_recieved = current_time;
    this->timed_out = false;

    struct proxy* old_head = timeout_queue_head;
    struct proxy* old_prev = this->previous;
    struct proxy* old_next = this->next;

    timeout_queue_head = this;
    this->previous = NULL;
    this->next = (old_head != this) ? old_head : NULL; // do not make a loop

    if (old_prev) {
        old_prev->next = old_next;
    }

    if (old_next) {
        old_next->previous = old_prev;
    }
    else {
        // at the end of the tail, so update the tail pointer to the new tail
        timeout_queue_tail = old_prev;
    }
}

static void add_new_timeout_queue(struct proxy* this) {
    this->last_recieved = current_time;
    this->previous = NULL;
    this->next = timeout_queue_head;
    if (timeout_queue_head) {
        timeout_queue_head->previous = this;
    }
    timeout_queue_head = this;
}

static void remove_from_timeout_queue(struct proxy* this) {
    if (this->previous) {
        this->previous->next = this->next;
    }
    if (this->next) {
        this->next->previous = this->previous;
    }
    if (timeout_queue_tail == this) {
        timeout_queue_tail = this->previous;
    }
    if (timeout_queue_head == this) {
        timeout_queue_head = this->next;
    }
}

static void non_block(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK | O_CLOEXEC);
}

static bool add_to_queue(int epoll_queue, int socket, void* data) {
    struct epoll_event ev;
#ifdef DEBUG
    memset(&ev, 0, sizeof(struct epoll_event));
#endif
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP;
    ev.data.ptr = data;
    if (epoll_ctl(epoll_queue, EPOLL_CTL_ADD, socket, &ev) < 0) {
        perror("cannot connect epoll to just created socket");
        return false;
    }
    return true;
}

static void close_and_free_proxy(struct proxy* proxy) {
    if (!proxy->closed) {
        LOG_D("closing: %p %d\n", (void*)proxy, proxy->socket);

        epoll_ctl(proxy->epoll_queue, EPOLL_CTL_DEL, proxy->socket, NULL);
        close(proxy->socket);
        proxy->closed = true;

        if (proxy->other) {
            close_and_free_proxy(proxy->other);
        }

        if (proxy->buffer) {
            SCHEDULE_FREE(proxy->buffer);
        }
        SCHEDULE_FREE(proxy);
        remove_from_timeout_queue(proxy);
    }
}

static void handle_normal_timeout(struct proxy* this) {
    if (!this->timed_out) {
        // only handle new time out events
        this->timed_out = true;
        if (this->other) {
            // we are a fully running connection
            if (this->other->timed_out) {
                // if the other side already timed-out, close ourself
                close_and_free_proxy(this);
                return;
            }
        }
        else {
            // no-back side connetion esthablished, so just get out of the queue
            close_and_free_proxy(this);
        }
    }
}


#define CHECK_ASYNC_RESULTS(__result, __proxy, __msg) { \
    if (__result == -1u && (errno == EAGAIN || errno == EWOULDBLOCK)) { \
        return; \
    } \
    else if (__result == -1u) { \
        LOG_D("ASYNC got connection error: %p %d\n", (void*)__proxy, __proxy->socket); \
        close_and_free_proxy(__proxy); \
        perror("Connection error ("__msg")"); \
        return; \
    } \
    else if (__result == 0) { \
        LOG_D("ASYNC got EOS: %p %d\n", (void*)__proxy, __proxy->socket); \
        close_and_free_proxy(__proxy); \
        return; \
    } \
}


static void do_proxy(struct proxy* proxy) {
    bool full_buffer = false;
    bool full_flush = false;
    LOG_V("Started normal proxy: %p\n", (void*)proxy);
    do {
        if (proxy->buffer_filled == proxy->buffer_flushed) {
            // buffer has been send to other side, so we can refill it
            size_t bytes_read = read(proxy->socket, proxy->buffer, proxy->buffer_size);
            CHECK_ASYNC_RESULTS(bytes_read, proxy, "Reading data from socket");

            full_buffer = bytes_read == proxy->buffer_size;
            proxy->buffer_flushed = proxy->buffer;
            proxy->buffer_filled = proxy->buffer + bytes_read;
            LOG_V("Read new bytes: %p %d\n", (void*)proxy, bytes_read);
        }

        size_t to_write = proxy->buffer_filled - proxy->buffer_flushed;
        size_t bytes_written = write(proxy->other->socket, proxy->buffer_flushed, to_write);
        CHECK_ASYNC_RESULTS(bytes_written, proxy, "Writing data to other side");

        proxy->buffer_flushed += bytes_written;
        full_flush = proxy->buffer_filled == proxy->buffer_flushed;
        LOG_V("Write new bytes: %p %d\n", (void*)proxy, bytes_written);
    /*
     * It could be that the read or the write side has more to produce/consume, and we won't get a new event for that, so while either one was fully flushed, we try again
     */
    } while (full_flush || full_buffer);
}

static void do_proxy_reverse(struct proxy* proxy) {
    // out side is ready for writing, so flush buffers to that direction
    do_proxy(proxy->other);
}

static void back_connection_finished(struct proxy* back) {
    struct proxy* front = back->other;

    LOG_D("Back connection setup: %p\n", (void*)back);
    back->out_op = front->out_op = do_proxy_reverse;
    back->in_op = front->in_op = do_proxy;

    do_proxy_reverse(back);
    do_proxy(back);
}

static int create_connection(int port) {
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(0x7f000001); /* 127.0.0.1 */
    sin.sin_port = htons(port);

    int new_socket = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (new_socket < 0) {
        return -1;
    }
    int res = connect(new_socket, (struct sockaddr *)(&sin), sizeof(struct sockaddr_in));
    if (res < 0 && errno != EINPROGRESS) {
        perror("Error opening connection to back-end");
        close(new_socket);
        return -1;
    }
    return new_socket;
}

static void setup_back_connection(struct proxy* proxy, int port) {
    int back_proxy_socket = create_connection(port);
    if (back_proxy_socket < 0) {
        close_and_free_proxy(proxy);
        return;
    }

    struct proxy *back_proxy = malloc(sizeof(struct proxy));
    back_proxy->closed = false;
    back_proxy->epoll_queue = proxy->epoll_queue;
    back_proxy->socket = back_proxy_socket;
    back_proxy->other = proxy;
    proxy->other = back_proxy;
    back_proxy->timed_out = false;
    back_proxy->buffer = malloc(config->max_recv_buffer);
    back_proxy->buffer_size = config->max_recv_buffer;
    back_proxy->buffer_flushed = back_proxy->buffer_filled = NULL;
    back_proxy->out_op = back_connection_finished;
    back_proxy->in_op = NULL;


    if (!add_to_queue(proxy->epoll_queue, back_proxy_socket, back_proxy)) {
        close_and_free_proxy(proxy);
        return;
    }
    add_new_timeout_queue(back_proxy);
    proxy->in_op = NULL; // don't read anything anymore until we are done with back connection

}

static void first_data(struct proxy* proxy) {
    assert(proxy->other == NULL);
    assert(!proxy->closed);
    assert(proxy->buffer_flushed == NULL && proxy->buffer_filled == NULL);

    size_t bytes_read = read(proxy->socket, proxy->buffer, proxy->buffer_size);
    CHECK_ASYNC_RESULTS(bytes_read, proxy, "Reading initial data from remote");

    proxy->buffer_flushed = proxy->buffer;
    proxy->buffer_filled = proxy->buffer + bytes_read;
    int port = config->normal_port;
    if (bytes_read >= config->knock_size) {
        if (memcmp(config->knock_value, proxy->buffer, config->knock_size) == 0) {
            port = config->ssh_port;
            proxy->buffer_flushed += config->knock_size; // skip the first knock-bytes
        }
    }
#ifdef DEBUG
    char __buf_copy[16];
    memcpy(__buf_copy, proxy->buffer, MIN(bytes_read, 16));
    for (int c = 0; c < 16; c++) {
        if (!isalnum(__buf_copy[c])) {
            __buf_copy[c]= '_';
        }
    }
    __buf_copy[MIN(bytes_read, 15)] ='\0';
    LOG_D("New connection send: %p %s %p (and %p vs %p (recv:%d)) to port %d\n", (void*) proxy, __buf_copy, (void*) proxy->buffer, (void*) proxy->buffer_flushed,(void*) proxy->buffer_filled, bytes_read, port);
#endif
    setup_back_connection(proxy, port);
}

static void handle_knock_timeout(struct proxy* this) {
    if (!this->timed_out) {
        this->timed_out = true;
        setup_back_connection(this, config->normal_port);
    }
}

static void process_other_events(struct epoll_event *ev) {
    struct proxy* proxy = (struct proxy*)ev->data.ptr;
    if (!proxy || proxy->closed) {
        return;
    }

    if (ev->events & EPOLLRDHUP) {
        close_and_free_proxy(proxy);
        return;
    }
    if (ev->events & (EPOLLERR|EPOLLHUP)) {
        // something happened, will need to actually talk to the socket to see if we care enough
        if (proxy->in_op) {
            proxy->in_op(proxy);
        }
        else if (proxy->out_op) {
            proxy->out_op(proxy);
        }
        return ;
    }
    if (ev->events & (EPOLLIN | EPOLLOUT)) {
        touch(proxy);
    }
    if (ev->events & EPOLLIN && proxy->in_op) {
        proxy->in_op(proxy);
        if (proxy->closed) { // don't continue in case
            return;
        }
    }
    if ((ev->events & EPOLLOUT) && proxy->out_op) {
        proxy->out_op(proxy);
    }
}

static bool initialize(struct sockaddr_in *listen_address, int* epoll_queue, int* listen_socket) {
    *listen_socket = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (*listen_socket < 0) {
        perror("cannot open socket");
        return false;
    }
    int one = 1;
    if (setsockopt(*listen_socket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(int)) < 0) {
        perror("cannot set SO_REUSEADDR");
        return false;
    }
    if (bind(*listen_socket, (struct sockaddr *)listen_address, sizeof(struct sockaddr_in)) < 0) {
        perror("cannot bind");
        return false;
    }
    if (listen(*listen_socket, 20) < 0) {
        perror("cannot start listening");
        return false;
    }

    *epoll_queue = epoll_create1(EPOLL_CLOEXEC);
    if (*epoll_queue < 0) {
        perror("cannot create epoll queue");
        return false;
    }

    return add_to_queue(*epoll_queue, *listen_socket, NULL);
}

int start(struct config* _config) {
    config = _config;
    setvbuf(stdout, NULL, _IONBF, 0);

    struct sockaddr_in sin;
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = 0;
    sin.sin_port = htons(config->external_port);

    int epoll_queue;
    int listen_socket;
    if (!initialize(&sin, &epoll_queue, &listen_socket)) {
        perror("Cannot initialize epoll queue");
        return -1;
    }

    struct epoll_event events[MAX_EVENTS];
#ifdef DEBUG
    memset(&events, 0, MAX_EVENTS * sizeof(struct epoll_event));
#endif
    for (;;) {
        int nfds = epoll_wait(epoll_queue, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            perror("epoll_wait failure");
            return -1;
        }
        // get the current time stamp
        clock_gettime(CLOCK_MONOTONIC, &current_time);

        LOG_V("Got %d events\n", nfds);
        for (int n = 0; n < nfds; ++n) {
            struct epoll_event* current_event = &(events[n]);
            if (current_event->data.ptr == NULL) {
                if (current_event->events & EPOLLIN) {
                    // one or more new connections
                    while (true) {
                        int conn_sock = accept(listen_socket, NULL, NULL);
                        if (conn_sock == -1) {
                            if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
                                // done with handling new connections
                                break;
                            } else {
                                perror("cannot accept new connection");
                                break;
                            }
                        }
                        non_block(conn_sock);

                        struct proxy* data = malloc(sizeof(struct proxy));
                        data->closed = false;
                        data->epoll_queue = epoll_queue;
                        data->socket = conn_sock;
                        data->other = NULL;
                        data->timed_out = false;
                        data->buffer = malloc(config->max_recv_buffer);
                        data->buffer_size = config->max_recv_buffer;
                        data->buffer_flushed = data->buffer_filled = NULL;
                        data->out_op = NULL;
                        data->in_op = first_data;
                        if (!add_to_queue(epoll_queue, conn_sock, data)) {
                            close(conn_sock);
                            free(data->buffer);
                            free(data);
                        }
                        else {
                            add_new_timeout_queue(data);
                        }
                    }
                }
            } else {
                process_other_events(current_event);
            }
        }
        // handle timeouts
        time_t default_timeout_threshold = current_time.tv_sec - config->default_timeout.tv_sec;
        struct proxy* current_proxy = timeout_queue_tail;

        // first we go throught the normal timeout cases
        while (current_proxy && current_proxy->last_recieved.tv_sec < default_timeout_threshold) {
            // general timeout
            handle_normal_timeout(current_proxy);
            current_proxy = current_proxy->previous;
        }
        // then we go through the knock timeout cases, which are newer in timeout
        time_t knock_timeout_threshold = current_time.tv_sec - config->knock_timeout.tv_sec;
        while (current_proxy && current_proxy->last_recieved.tv_sec < knock_timeout_threshold) {
            if (!current_proxy->other) {
                // knock timeout applies
                handle_knock_timeout(current_proxy);
            }
            current_proxy = current_proxy->previous;
        }


        // handle pending free's
        for (size_t i = 0 ; i < free_index; i++) {
            free(to_free[i]);
        }
        free_index = 0;
    }
}
