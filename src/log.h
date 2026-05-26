#ifndef BG_LOG_H
#define BG_LOG_H

#include <stdio.h>

#define LOG_INFO(fmt, ...)  fprintf(stderr, "[bg] " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  fprintf(stderr, "[bg:warn] " fmt "\n", ##__VA_ARGS__)
#define LOG_ERR(fmt, ...)   fprintf(stderr, "[bg:err] " fmt "\n", ##__VA_ARGS__)

#endif
