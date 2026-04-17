#include <stdlib.h>
#include <string.h>

int main() {
    while (1) {
        char *p = malloc(50 * 1024 * 1024); // 50 MB

        if (p) {
            memset(p, 1, 50 * 1024 * 1024); // 🔥 FORCE memory usage
        }
    }
}
