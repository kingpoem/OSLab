#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include "../my_vm.h"

#define SIZE 10
#define ARRAY_SIZE 6000

void mat_mult(void *mat1, void *mat2, int size, void *answer)
{
	int i, k, j, num1, num2, total;
	unsigned int addr_mat1, addr_mat2, addr_ans;

	if (!mat1 || !mat2 || !answer || size <= 0)
		return;

	for (i = 0; i < size; i++) {
		for (j = 0; j < size; j++) {
			total = 0;
			/* answer[i][j] += mat1[i][k] * mat2[k][j] */
			for (k = 0; k < size; k++) {
				addr_mat1 = (unsigned int) mat1 +
						(i * size * sizeof(int)) +
						(k * sizeof(int));

				addr_mat2 = (unsigned int) mat2 +
						(k * size * sizeof(int)) +
						(j * sizeof(int));

				myRead((void *) addr_mat1, &num1, sizeof(int));
				myRead((void *) addr_mat2, &num2, sizeof(int));
				total += num1 * num2;
			}
			addr_ans = (unsigned int) answer +
					(i * size * sizeof(int)) + (j * sizeof(int));
			myWrite((void *) addr_ans, &total, sizeof(int));
		}
	}
}


int main() {

    printf("Allocating three arrays of %d bytes\n", ARRAY_SIZE);

    void *a = myMalloc(ARRAY_SIZE);
    int old_a = (int)a;
    void *b = myMalloc(ARRAY_SIZE);
    void *c = myMalloc(ARRAY_SIZE);
    int x = 1;
    int y, z;
    int i =0, j=0;
    int address_a = 0, address_b = 0;
    int address_c = 0;

    printf("Addresses of the allocations: %x, %x, %x\n", (int)a, (int)b, (int)c);

    printf("Storing integers to generate a SIZExSIZE matrix\n");
    for (i = 0; i < SIZE; i++) {
        for (j = 0; j < SIZE; j++) {
            address_a = (unsigned int)a + ((i * SIZE * sizeof(int))) + (j * sizeof(int));
            address_b = (unsigned int)b + ((i * SIZE * sizeof(int))) + (j * sizeof(int));
            myWrite((void *)address_a, &x, sizeof(int));
            myWrite((void *)address_b, &x, sizeof(int));
        }
    } 

    printf("Fetching matrix elements stored in the arrays\n");

    for (i = 0; i < SIZE; i++) {
        for (j = 0; j < SIZE; j++) {
            address_a = (unsigned int)a + ((i * SIZE * sizeof(int))) + (j * sizeof(int));
            address_b = (unsigned int)b + ((i * SIZE * sizeof(int))) + (j * sizeof(int));
            myRead((void *)address_a, &y, sizeof(int));
            myRead( (void *)address_b, &z, sizeof(int));
            printf("%d ", y);
        }
        printf("\n");
    } 

    printf("Performing matrix multiplication with itself!\n");
    mat_mult(a, b, SIZE, c);


    for (i = 0; i < SIZE; i++) {
        for (j = 0; j < SIZE; j++) {
            address_c = (unsigned int)c + ((i * SIZE * sizeof(int))) + (j * sizeof(int));
            myRead((void *)address_c, &y, sizeof(int));
            printf("%d ", y);
        }
        printf("\n");
    }
    printf("Freeing the allocations!\n");
    myFree(a, ARRAY_SIZE);
    myFree(b, ARRAY_SIZE);
    myFree(c, ARRAY_SIZE);

    printf("Checking if allocations were freed!\n");
    a = myMalloc(ARRAY_SIZE);
    if ((int)a == old_a)
        printf("free function works\n");
    else
        printf("free function does not work\n");

    return 0;
}
