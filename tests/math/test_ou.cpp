
#include <cassert>
#include "models/ou.hpp"

int main(){
    OU o;
    double prev = o.x;
    o.step(0.1);
    assert(o.x != prev);
}
