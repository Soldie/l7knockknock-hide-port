#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>

#include <argp.h>

#include "knock-common.h"
#include "proxy-libevent.h"


#define EXT_PORT_DEFAULT 443
#define SSH_PORT_DEFAULT 22
#define NORMAL_PORT_DEFAULT 8443
#define MAX_RECV_BUF_DEFAULT 2<<20
#define DEFAULT_TIMEOUT_DEFAULT 30
#define KNOCK_TIMEOUT_DEFAULT 2

static struct config config;

const char *argp_program_version = "knock-ssh 0.1";
const char *argp_program_bug_address = "<davy.landman@gmail.com>";
static const char *doc = "knock-ssh -- a protocol knocker to unlock a ssh service hidden behind another port";

// non optional params
static const char *args_doc = "KNOCK_KNOCK_STRING";

// optional params
// {NAME, KEY, ARG, FLAGS, DOC}.
static struct argp_option options[] =
{
    {"verbose", 'v', 0, 0, "Produce verbose output", -1},
    {"listenPort", 'p', "port", 0, "Port to listen on for new connections, default: EXT_PORT_DEFAULT", 1},
    {"normalPort", 'n', "port", 0, "Port to forward HTTPS traffic to, default: NORMAL_PORT_DEFAULT", 0},
    {"sshPort", 's', "port", 0, "Port to forward SSH traffic to, default: SSH_PORT_DEFAULT", 0},
    {"bufferSize", 'b', "size", 0, "Maximum proxy buffer size (in bytes), default: MAX_RECV_BUF_DEFAULT", 2},
    {"proxyTimeout", 'o', "seconds", 0, "Seconds before timeout is assumed and connection with HTTPS or SSH is closed, default: DEFAULT_TIMEOUT_DEFAULT", 0},
    {"knockTimeout", 'k', "seconds", 0, "Seconds after which we assume no knock-knock will occur, default: KNOCK_TIMEOUT_DEFAULT", 0},
    {0,0,0,0,0,0}
};

static void fill_defaults() {
    config.external_port = EXT_PORT_DEFAULT;
    config.normal_port = NORMAL_PORT_DEFAULT;
    config.ssh_port = SSH_PORT_DEFAULT;
    config.max_recv_buffer = MAX_RECV_BUF_DEFAULT;
    config.default_timeout.tv_sec = DEFAULT_TIMEOUT_DEFAULT;
    config.knock_timeout.tv_sec = KNOCK_TIMEOUT_DEFAULT;
    config.verbose = false;
    config.knock_value = NULL;
    config.knock_size = 0;
}

#define PARSE_NUMBER(type, result, MIN, MAX, source, error, state) {\
    errno=0; \
    char* ___endPos; \
    unsigned long long ___res = strtoull(source, &___endPos, 10); \
    if ((errno != 0 && errno != ERANGE) || *___endPos != '\0') { \
        fprintf(stderr, "%s: %s\n", error, source); \
        argp_usage(state); \
    } \
    if (___res < (unsigned long long)(MIN) || ___res > (unsigned long long)(MAX)) {\
        fprintf(stderr, "%s: %llu (not in range %llu..%llu)\n", error, ___res, (unsigned long long)(MIN), (unsigned long long)(MAX)); \
        argp_usage(state); \
    }\
    result = (type)___res;\
}

static error_t parse_opt(int key, char *arg, struct argp_state *state) {
    switch(key) {
        case 'v':
            config.verbose = true;
            break;
        case 'p':
            PARSE_NUMBER(uint32_t, config.external_port, 1, 65536, arg, "Invalid port number", state)
            break;
        case 'n':
            PARSE_NUMBER(uint32_t, config.normal_port, 1, 65536, arg, "Invalid port number", state)
            break;
        case 's':
            PARSE_NUMBER(uint32_t, config.ssh_port, 1, 65536, arg, "Invalid port number", state)
            break;
        case 'b':
            PARSE_NUMBER(size_t, config.max_recv_buffer, 1, 1<<31, arg, "Invalid buffer size", state)
            break;
        case 'o':
            PARSE_NUMBER(uint32_t, config.default_timeout.tv_sec, 1, 600, arg, "Invalid amount of seconds", state)
            break;
        case 'k':
            PARSE_NUMBER(uint32_t, config.knock_timeout.tv_sec, 1, 5, arg, "Invalid amount of seconds", state)
            break;
        case ARGP_KEY_ARG:
            if (config.knock_size > 0) {
                argp_usage(state);
                return 1;
            }
            config.knock_value = arg;
            config.knock_size = strlen(arg);
            break;
        case ARGP_KEY_END:
            if (config.knock_size == 0) {
                argp_usage (state);
                return 1;
            }
            break;
        default:
            return ARGP_ERR_UNKNOWN;
    }
    return 0;
}


int main(int argc, char **argv) {
    fill_defaults();

    struct argp argp = {options, parse_opt, args_doc, doc, NULL, NULL, NULL};
    argp_parse (&argp, argc, argv, 0, 0, NULL);

    return start(&config);
}
