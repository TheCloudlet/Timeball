/* Two passes over an array of 256 bytes: four 64-byte lines.
   A one-line cache misses again on the second pass. A cache that holds
   the array hits. The loads are volatile so the compiler keeps them. */

static volatile unsigned char bytes[256];

int main(void) {
  unsigned sum = 0;
  for (int pass = 0; pass < 2; ++pass) {
    for (int i = 0; i < 256; ++i) {
      sum += bytes[i];
    }
  }
  return sum == 0 ? 0 : 1;
}
