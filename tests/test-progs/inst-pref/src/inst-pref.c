#define ITERATIONS 100

// 测试2: 固定模式分支 - 测试分支预测和预取
void patterned_branch() {
    volatile int sum = 0;
    for (int i = 0; i < ITERATIONS; i++) {
        if (i % 4 == 0) {  // 可预测的分支模式
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