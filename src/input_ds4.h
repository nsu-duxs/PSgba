#ifndef INPUT_DS4_H
#define INPUT_DS4_H

#include <stdbool.h>
#include "emulator_manager.h"

bool input_init(void);
void input_poll_all(struct PS4GBA_Context* ctx);
void input_shutdown(void);

#endif /* INPUT_DS4_H */
