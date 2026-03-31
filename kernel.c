/* 
 * Kernel code in C
 */

#include "stdio.h"

void kernel_main() {

    setcursor(0, 8);
    puts("==============================\n", 2);
    puts("Hello world from C Kernel!!!\n", 3);
    puts("Kernel loaded successfully ...\n", 4);
    puts("==============================\n", 5);

}