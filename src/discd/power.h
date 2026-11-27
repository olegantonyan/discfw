#pragma once
#include <stdbool.h>
#include <stddef.h>

struct kvbuf;

void power_init(void);
void power_reload(void);
void power_charge_full(bool on);
bool power_tick(void);
void power_status(struct kvbuf *b, bool full);
