#include "ps4_jailbreak.h"
#include "libjbc/libjbc.h"
#include <stdio.h>

static bool s_jailbroken = false;

bool ps4_jailbreak_system(void) {
    if (s_jailbroken) {
        return true;
    }

    jbc_cred cred;
    if (jbc_get_cred(&cred) == 0) {
        if (jbc_jailbreak_cred(&cred) == 0) {
            if (jbc_set_cred(&cred) == 0) {
                s_jailbroken = true;
            }
        }
    }

    /* Também tenta mapear pontos de montagem no sandbox para acesso irrestrito */
    for (int u = 0; u < 8; ++u) {
        char sys_usb[32];
        char mnt_usb[16];
        snprintf(sys_usb, sizeof(sys_usb), "/mnt/usb%d", u);
        snprintf(mnt_usb, sizeof(mnt_usb), "usb%d", u);
        jbc_mount_in_sandbox(sys_usb, mnt_usb);
    }

    return s_jailbroken;
}

bool ps4_is_jailbroken(void) {
    return s_jailbroken;
}

