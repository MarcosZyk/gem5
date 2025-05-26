#define ITERATIONS 100

void patterned_branch() {
    volatile int sum = 0;
    for (int i = 0; i < ITERATIONS; i++) {
        if (i % 4 == 0) {
            sum += i;
        } else {
            sum -= i;
        }
    }
}

int main() {

    patterned_branch();

    return 0;
}