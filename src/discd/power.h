#pragma once
#include <stdio.h>

void power_init(void);
void power_tick(void);
int power_set_limit(int limit);
void power_status(FILE *out);
