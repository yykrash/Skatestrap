#include <stdio.h>
#include <stdarg.h>
__attribute__((sysv_abi)) int f(int n, ...) {
  __builtin_va_list ap; __builtin_va_start(ap, n);
  int s = 0; for (int i = 0; i < n; i++) s += __builtin_va_arg(ap, int);
  double d = __builtin_va_arg(ap, double);
  __builtin_va_end(ap); return s + (int)d;
}
int main() { printf("%d\n", f(3, 1, 2, 3, 10.0)); printf("sizeof va_list %zu\n", sizeof(__builtin_va_list)); }
