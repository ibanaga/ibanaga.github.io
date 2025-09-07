int global1 = 10;
int global2 = 20;

int sum(int a, int b) {
        return a + b;
}

int global_sum(void) {
	return global1 + global2;
}
