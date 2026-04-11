
#include <cassert>
#include "models/hawkes.hpp"

int main(){
    Hawkes h;
    double prev = h.lambda;
    h.update(0.1,1);
    assert(h.lambda > prev);
}
