extern int global1;
extern int global2;
char global_array[512];

extern int sum(int a, int b);
extern int global_sum();

int main() {
    int res1 = sum(global1, global2);
	res1 += global_sum();
	return res1;
}
