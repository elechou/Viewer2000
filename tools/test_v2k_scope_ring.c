#include <stdint.h>

#include "../contracts/v2k_scope.h"

static int expect(uint16_t actual, uint16_t expected)
{
    return (actual == expected) ? 0 : 1;
}

int main(void)
{
    const uint16_t capacity = 35u;
    int failed = 0;

    failed += expect(v2k_ring_pos(0u, capacity), 0u);
    failed += expect(v2k_ring_pos(34u, capacity), 34u);
    failed += expect(v2k_ring_pos(35u, capacity), 0u);
    failed += expect(v2k_ring_pos(69u, capacity), 34u);
    failed += expect(v2k_ring_next(69u, capacity), 0u);
    failed += expect(v2k_ring_dist(3u, 68u, capacity), 5u);
    failed += expect(v2k_ring_dist(35u, 0u, capacity), capacity);
    failed += expect(v2k_ring_back(2u, 5u, capacity), 67u);
    failed += expect(v2k_ring_add(67u, 5u, capacity), 2u);

    return failed;
}
