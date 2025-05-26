#include <stdio.h>
#include <stdlib.h>

#define ARRAY_SIZE 1000

void perform_computation(int *arr, int size) {
    int count = 0;
    for (int i = 0; i < size; ++i) {
        if ((i % 10) == 0) {
            // printf("%d\n", arr[i]);
            count += 1;
        } else if ((i % 5) == 0) {
            arr[i] = arr[i - 1] * 2;
        } else {
            arr[i] = arr[i - 1] + arr[i - 2];
        }
    }
}

int main() {
    int *arr = (int*)malloc(ARRAY_SIZE * sizeof(int));

    // Initialize the array with some values
    arr[0] = 1;
    arr[1] = 1;
    for (int i = 2; i < ARRAY_SIZE; ++i) {
        arr[i] = 0;
    }

    for (int run = 0; run < 10; ++run) {
        perform_computation(arr, ARRAY_SIZE);
    }

    free(arr);
    return 0;
}
