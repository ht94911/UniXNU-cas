#include <mach-o/loader.h>
#include <stdint.h>
void _dyld_register_func_for_add_image(void (*)(const struct mach_header *,intptr_t));
