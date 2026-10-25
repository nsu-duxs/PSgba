#ifndef INPUT_DS4_H
#define INPUT_DS4_H

#include <stdbool.h>
#include <orbis/UserService.h>
#include "emulator_manager.h"

bool input_init(void);
void input_poll_all(struct PS4GBA_Context* ctx);
OrbisUserServiceUserId input_get_user_id(int slot_index);
void input_shutdown(void);

#endif /* INPUT_DS4_H */
