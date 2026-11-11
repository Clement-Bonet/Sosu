// The C++ file of tests/programs/cppsources: compiled by the C++ compiler, without exceptions nor
// RTTI, and called from Sosu through functions of C.
#include <cstddef>
#include <vector>

extern "C" int twice(int x);  // the C file of the package

namespace {

struct Shape {
    virtual ~Shape() {}
    virtual int area() const = 0;
};

struct Square : Shape {
    int side;
    explicit Square(int s) : side(s) {}
    int area() const override { return side * side; }
};

template <typename T> T sum(const std::vector<T> &items) {
    T total = T();
    for (const T &item : items) total += item;
    return total;
}

// built before main: the C++ library starts it
struct Started {
    int value;
    Started() : value(SHAPES_SCALE) {}
} started;

}  // namespace

extern "C" int shapes_area(int side) {
    Square square(side);
    const Shape &shape = square;  // a call through the table of virtual functions
    return shape.area();
}

extern "C" int shapes_sum(const int *items, size_t count) {
    std::vector<int> all(items, items + count);
    return sum(all) * started.value + twice(1);
}
