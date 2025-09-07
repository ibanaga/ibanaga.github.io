int global1 = 10;
int global2 = 20;
char global_array[512];

int sum(int a, int b) {
        return a + b;
}

int main() {
        return sum(global1, global2);
}
