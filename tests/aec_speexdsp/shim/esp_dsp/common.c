#include <stdbool.h>
bool dsp_is_power_of_two(int x) { return x > 0 && (x & (x-1)) == 0; }
int dsp_power_of_two(int x) {
    int power = 0;
    while (x > 1) { x >>= 1; ++power; }
    return power;
}
