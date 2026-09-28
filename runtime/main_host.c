#include "hle.h"
#include <stdlib.h>

extern uint32_t g_export_new_application;
void app_boot(CPU *c);

int main(int argc, char **argv) {
    if (argc < 2) {
        LOG("usage: %s <path to 6r72.app>", argv[0]);
        return 1;
    }
    extern void install_crash_handler(void);
    install_crash_handler();
    set_card_root_from_app(argv[1]);
    mem_init();
    if (!load_image(argv[1])) {
        LOG("cannot load %s", argv[1]);
        return 1;
    }
    hle_bind();
    static CPU cpu;
    kernel_init(&cpu);
    g_export_new_application = g_image_export1;
    app_boot(&cpu);
    return 0;
}
