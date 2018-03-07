#ifndef DEBUG_13157_H
#define DEBUG_13157_H

#define debug_print(fmt, ...) do { fprintf(stderr, fmt, __VA_ARGS__); } while (0)

#ifdef DEBUG
    #define LOG_D(fmt, ...) debug_print(" D: "fmt, __VA_ARGS__)
#else
    #define LOG_D(fmt, ...) 
#endif

#ifdef VERY_VERBOSE 
    #define LOG_V(fmt, ...) debug_print(" V: "fmt, __VA_ARGS__)
#else
    #define LOG_V(fmt, ...) 
#endif

#endif
