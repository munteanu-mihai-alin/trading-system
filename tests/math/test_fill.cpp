
#include <cassert>
#include "fill/fill.hpp"

int main(){
    FillModel f;
    double p = f.compute(100,200,0.01);
    assert(p>=0 && p<=1);
}
