#pragma once
#include <stdio.h>
#include <stdlib.h>

static int check_failed;

#define CHECK(cond)                                                                    \
	do {                                                                           \
		if (!(cond)) {                                                         \
			fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
			check_failed++;                                                \
		}                                                                      \
	} while (0)

#define CHECK_DONE()                                                   \
	do {                                                           \
		if (check_failed) {                                    \
			fprintf(stderr, "%s: %d failed\n", __FILE__, check_failed); \
			return 1;                                      \
		}                                                      \
		printf("%s: ok\n", __FILE__);                          \
		return 0;                                              \
	} while (0)
