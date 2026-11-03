/* Host stub: quiet unless HOST_LOG=1, so the test prints only its verdicts. */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#define HOST_LOG(level, tag, fmt, ...) \
    do { if (getenv("HOST_LOG")) fprintf(stderr, level " %s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGE(tag, fmt, ...) HOST_LOG("E", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) HOST_LOG("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) HOST_LOG("I", tag, fmt, ##__VA_ARGS__)
