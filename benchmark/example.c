#include <stdio.h>
#include <stdio.h>
#include "../my_vm.h"

int main() {
    int *ptr;
    int x = 3;
    int y;

    myRead(ptr, &y, sizeof(int));

    printf("Before write, y=%d\n", y);

    ptr = myMalloc(sizeof(int));
    myWrite(ptr, &x, sizeof(int));
    myRead(ptr, &y, sizeof(int));
    printf("After write, y=%d\n", y);

    myFree(ptr, sizeof(int));
}
