#pragma once
#include <stddef.h>

#define DISCD_SOCK "/run/discd.sock"

int discd_request(const char *cmd, char *reply, size_t reply_len, int timeout_ms);
