#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <signal.h>


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
    int socket;
    struct proxy* other;
    bool timed_out;
    bool closed;

    int buffer[2];
    size_t buffer_filled;

    ProxyCall out_op;
    ProxyCall in_op;

    time_t last_recieved;
    struct proxy* next;
    struct proxy* previous;
};

static int _epoll_queue = -1;

enum { READ = 0, WRITE = 1 };

static struct proxy* timeout_queue_head = NULL;
static struct proxy* timeout_queue_tail = NULL;

static time_t current_time;

static void touch(struct proxy* this) {
    //LOG_D("B-Touch: %p (prev: %p, next: %p) (head: %p, tail: %p)\n", (void*)this, (void*)this->previous, (void*)this->next, (void*)timeout_queue_head, (void*)timeout_queue_tail);
    this->last_recieved = current_time;
    this->timed_out = false;
    if (timeout_queue_head == this) {
        return;
    }

    struct proxy* old_head = timeout_queue_head;
    struct proxy* old_prev = this->previous;
    struct proxy* old_next = this->next;

    timeout_queue_head = this;
    this->previous = NULL;
    this->next = old_head;
    if (old_head) {
        old_head->previous = this;
    }

    if (old_prev) {
        old_prev->next = old_next;
    }

    if (old_next) {
        old_next->previous = old_prev;
    }
    else {
        // we were at the tail of the list
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

static bool add_to_queue(int socket, void* data) {
    struct epoll_event ev;
#ifdef DEBUG
    memset(&ev, 0, sizeof(struct epoll_event));
#endif
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
    ev.data.ptr = data;
    if (epoll_ctl(_epoll_queue, EPOLL_CTL_ADD, socket, &ev) < 0) {
        perror("cannot connect epoll to just created socket");
        return false;
    }
    return true;
}

static void close_and_free_proxy(struct proxy* proxy) {
    if (!proxy->closed) {
        LOG_D("closing: %p %d\n", (void*)proxy, proxy->socket);

        epoll_ctl(_epoll_queue, EPOLL_CTL_DEL, proxy->socket, NULL);
        close(proxy->socket);
        proxy->closed = true;

        if (proxy->other) {
            close_and_free_proxy(proxy->other);
        }

        close(proxy->buffer[READ]);
        close(proxy->buffer[WRITE]);

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
                LOG_D("Closing proxy %p due to timeout from both sides", (void*)this);
                // if the other side already timed-out, close ourself
                close_and_free_proxy(this);
                return;
            }
        }
        else {
            LOG_D("Closing proxy %p due to timeout from single side without backend", (void*)this);
            // no-back side connetion esthablished, so just get out of the queue
            close_and_free_proxy(this);
        }
    }
}

#define MAX_SPLICE_CHUNK (64*1024)

static void do_proxy(struct proxy* proxy) {
    bool should_close_proxy = false;
    LOG_V("Started normal proxy: %p\n", (void*)proxy);
    while (true) {
        /*** reasons the loop stops:
         * - can't read at the moment & buffer was empty
         * - can't write at the moment (don't want to needlesly fill the buffer, we always try to read again when we get a write event)
         */


        // read everything we can fit into the pipe buffer
        ssize_t bytes_read = splice(proxy->socket, NULL, proxy->buffer[WRITE], NULL, MAX_SPLICE_CHUNK, SPLICE_F_MOVE | SPLICE_F_NONBLOCK);
        if (bytes_read == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                bytes_read = 0; // expected end of non_blocking splice
            }
            else {
                LOG_D("ASYNC got connection error: %p %d\n", (void*)proxy, proxy->socket);
#ifdef DEBUG
                perror("Connection error (splicing from socket to pipe)");
#endif
                should_close_proxy = true;
            }
        }
        else if (bytes_read == 0) {
            LOG_D("ASYNC got EOS: %p %d\n", (void*)proxy, proxy->socket);
            should_close_proxy = true;
        }
        proxy->buffer_filled += bytes_read;

        if (proxy->buffer_filled == 0) {
            break;
        }

        // splice stuff from pipe to target socket
        ssize_t bytes_written = splice(proxy->buffer[READ], NULL, proxy->other->socket, NULL, MIN(proxy->buffer_filled, MAX_SPLICE_CHUNK), SPLICE_F_MOVE | SPLICE_F_NONBLOCK);
        if (bytes_written == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; // target not ready to receive more bytes
            }
            else {
                LOG_D("ASYNC got connection error: %p %d\n", (void*)proxy, proxy->socket);
#ifdef DEBUG
                perror("Connection error (splicing from pipe to socket)");
#endif
                should_close_proxy = true;
                proxy->buffer_filled = 0; // signal that the data should be dropped
                break;
            }
        }
        else if (bytes_written == 0) {
            LOG_D("ASYNC got EOS: %p %d\n", (void*)proxy, proxy->socket);
            should_close_proxy = true;
            proxy->buffer_filled = 0; // signal that the data should be dropped
            break;
        }
        proxy->buffer_filled -= bytes_written;
    }

    if (should_close_proxy && proxy->buffer_filled == 0) {
        LOG_D("During proxy we determined we should close it: %p %d\n", (void*)proxy, proxy->socket);
        close_and_free_proxy(proxy);
    }
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
    LOG_D("Back connection setup finished: %p\n", (void*)back);
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

static void setup_back_connection(struct proxy* proxy, uint32_t port) {
    int back_proxy_socket = create_connection(port);
    if (back_proxy_socket < 0) {
        close_and_free_proxy(proxy);
        return;
    }

    struct proxy *back_proxy = malloc(sizeof(struct proxy));
    back_proxy->closed = false;
    back_proxy->socket = back_proxy_socket;
    back_proxy->other = proxy;
    proxy->other = back_proxy;
    back_proxy->timed_out = false;
    pipe2(back_proxy->buffer, O_CLOEXEC | O_NONBLOCK);
    back_proxy->buffer_filled = 0;
    back_proxy->out_op = back_connection_finished;
    back_proxy->in_op = NULL;

    add_new_timeout_queue(back_proxy);
    if (!add_to_queue(back_proxy_socket, back_proxy)) {
        close_and_free_proxy(proxy);
        return;
    }
    proxy->in_op = NULL; // don't read anything anymore until we are done with back connection

}

static void first_data(struct proxy* proxy) {
    assert(proxy->other == NULL);
    assert(!proxy->closed);

    uint8_t* tmp_buffer = malloc(config->knock_size);
    size_t bytes_read = read(proxy->socket, tmp_buffer, config->knock_size);
    if (bytes_read == -1u && errno != EAGAIN && errno != EWOULDBLOCK) {
        LOG_D("Got connection error before first read: %p %d\n", (void*)proxy, proxy->socket);
        free(tmp_buffer);
        close_and_free_proxy(proxy);
        perror("Connection error: (Reading initial data from remote)");
        return;
    }
    if (bytes_read == 0) {
        LOG_D("Got EOS before first read: %p %d\n", (void*)proxy, proxy->socket);
        free(tmp_buffer);
        close_and_free_proxy(proxy);
        return;
    }

    uint32_t port = config->normal_port;
    if (bytes_read == config->knock_size) {
        if (memcmp(config->knock_value, tmp_buffer, config->knock_size) == 0) {
            port = config->hidden_port;
        }
    }
    if (port == config->normal_port && bytes_read > 0) {
        // copy stuff we read to the pipe
        write(proxy->buffer[WRITE], tmp_buffer, bytes_read);
        proxy->buffer_filled += bytes_read;
    }

#ifdef DEBUG
    char __buf_copy[16];
    memcpy(__buf_copy, tmp_buffer, MIN(bytes_read, 16));
    for (int c = 0; c < MIN(bytes_read, 16); c++) {
        if (!isalnum(__buf_copy[c])) {
            __buf_copy[c]= '_';
        }
    }
    __buf_copy[MIN(bytes_read, 15)] ='\0';
    LOG_D("New connection send: %p \"%s\" to port %d\n", (void*) proxy, __buf_copy, port);
#endif

    free(tmp_buffer);

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
    if (ev->events & EPOLLIN) {
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

static bool initialize(struct sockaddr_in *listen_address, int* listen_socket) {
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

    _epoll_queue = epoll_create1(EPOLL_CLOEXEC);
    if (_epoll_queue < 0) {
        perror("cannot create epoll queue");
        return false;
    }

    return add_to_queue(*listen_socket, NULL);
}

static int _listen_socket = -1;

static void close_down_nicely() {
    if (_epoll_queue != -1) {
        close(_epoll_queue);
    }
    if (_listen_socket != -1) {
        close(_listen_socket);
    }
}

void cleanup_buffers(int UNUSED(signum)) {
    close_down_nicely();
    exit(0);
}

int start(struct config* _config) {
    config = _config;

    signal(SIGTERM, cleanup_buffers);

    struct sockaddr_in sin;
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = 0;
    sin.sin_port = htons(config->external_port);

    if (!initialize(&sin, &_listen_socket)) {
        perror("Cannot initialize epoll queue");
        close_down_nicely();
        return -1;
    }

    struct epoll_event events[MAX_EVENTS];
#ifdef DEBUG
    memset(&events, 0, MAX_EVENTS * sizeof(struct epoll_event));
#endif
    for (;;) {
        int nfds = epoll_wait(_epoll_queue, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            perror("epoll_wait failure");
            close_down_nicely();
            return -1;
        }
        // get the current time stamp
        struct timespec tm;
        clock_gettime(CLOCK_MONOTONIC, &tm);
        current_time = tm.tv_sec;

        LOG_V("Got %d events\n", nfds);
        for (int n = 0; n < nfds; ++n) {
            struct epoll_event* current_event = &(events[n]);
            if (current_event->data.ptr == NULL) {
                if (current_event->events & EPOLLIN) {
                    // one or more new connections
                    while (true) {
                        int conn_sock = accept4(_listen_socket, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
                        if (conn_sock == -1) {
                            if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
                                // done with handling new connections
                                break;
                            } else {
                                perror("cannot accept new connection");
                                break;
                            }
                        }

                        struct proxy* data = malloc(sizeof(struct proxy));
                        data->closed = false;
                        data->socket = conn_sock;
                        data->other = NULL;
                        data->timed_out = false;
                        pipe2(data->buffer, O_CLOEXEC | O_NONBLOCK);
                        data->buffer_filled = 0;
                        data->out_op = NULL;
                        data->in_op = first_data;
                        if (!add_to_queue(conn_sock, data)) {
                            close(conn_sock);
                            close(data->buffer[READ]);
                            close(data->buffer[WRITE]);
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
        time_t default_timeout_threshold = current_time - config->default_timeout.tv_sec;
        struct proxy* current_proxy = timeout_queue_tail;

        // first we go throught the normal timeout cases
        while (current_proxy && current_proxy->last_recieved < default_timeout_threshold) {
            // general timeout
            handle_normal_timeout(current_proxy);
            current_proxy = current_proxy->previous;
        }
        // then we go through the knock timeout cases, which are newer in timeout
        time_t knock_timeout_threshold = current_time - config->knock_timeout.tv_sec;
        while (current_proxy && current_proxy->last_recieved < knock_timeout_threshold) {
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
