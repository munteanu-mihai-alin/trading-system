
#include <cassert>
#include <cmath>

int main(){
    double x = 1 - std::exp(-2.0);
    double y = 1 - std::exp(-2.0);
    assert(std::fabs(x - y) < 1e-12);
}
