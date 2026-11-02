#include <charconv>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <random>
#include <iostream>
#include <limits>

int main() {
  double vals[] = {0.0, -0.0, 1.0, -1.0, 0.0/0.0, 1.0/0.0, -1.0/0.0,
    1e-4, 1e-5, 9.99999999999999e14, 1e15, 1e-308, 1.7976931348623157e308};
  for (double d : vals) {
    char buf[64]; std::snprintf(buf, sizeof buf, "%.15g", d);
    char tbuf[64]; auto r = std::to_chars(tbuf, tbuf+sizeof tbuf, d, std::chars_format::general, 15);
    if (r.ec != std::errc()) { std::cout << "ERR to_chars " << d << "\n"; continue; }
    *r.ptr = 0;
    bool same = (std::strcmp(buf, tbuf) == 0);
    std::cout << (same?"OK ":"DIFF ") << d << "  snprintf=" << buf << "  to_chars=" << tbuf << "\n";
  }
  std::mt19937_64 rng(12345);
  int diffs = 0;
  for (int i=0;i<200000;i++) {
    double d;
    switch (i % 5) {
      case 0: d = (double)rng() / (double)rng(); break;
      case 1: d = (double)(rng() % 1000000) - 500000; break;
      case 2: d = std::ldexp((double)(rng() % 1000000), (int)(rng()%60)-30); break;
      case 3: d = (rng() & 1) ? (double)rng() : -(double)rng(); break;
      default: d = (double)(rng() % 1000) / 1000.0; break;
    }
    char buf[64]; std::snprintf(buf, sizeof buf, "%.15g", d);
    char tbuf[64]; auto r = std::to_chars(tbuf, tbuf+sizeof tbuf, d, std::chars_format::general, 15);
    if (r.ec != std::errc()) { std::cout << "ERR to_chars " << d << "\n"; diffs++; continue; }
    *r.ptr = 0;
    if (std::strcmp(buf, tbuf) != 0) {
      if (diffs < 20) std::cout << "DIFF " << d << "  snprintf=" << buf << "  to_chars=" << tbuf << "\n";
      diffs++;
    }
  }
  std::cout << "fuzz diffs: " << diffs << "\n";
  return 0;
}