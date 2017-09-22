#ifndef KNOCK_COMMON_H
#define KNOCK_COMMON_H
#include <stdbool.h>
struct config {
    uint32_t external_port;
    uint32_t normal_port;
    uint32_t ssh_port;
    uint64_t max_recv_buffer;
    struct timeval default_timeout;
    struct timeval knock_timeout;
    bool verbose;
    char* knock_value;
    size_t knock_size;
};

#ifdef __GNUC__
#  define UNUSED(x) UNUSED_ ## x __attribute__((__unused__))
#else
#  define UNUSED(x) UNUSED_ ## x
#endif

#endif
