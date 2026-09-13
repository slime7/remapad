#include "ns2_serial.h"

void ns2_serial_build(const char prefix[3], const char digits10[10], char out[15])
{
    int sum = 0;
    for (int i = 0; i < 10; i++) {
        const int d = digits10[i] - '0';
        sum += (i % 2 == 0) ? d : 3 * d;
    }
    const char check = (char)('0' + ((10 - sum % 10) % 10));
    out[0] = prefix[0];
    out[1] = prefix[1];
    out[2] = prefix[2];
    for (int i = 0; i < 10; i++) {
        out[3 + i] = digits10[i];
    }
    out[13] = check;
    out[14] = '\0';
}
