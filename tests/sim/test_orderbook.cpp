
#include <cassert>
#include "sim/orderbook.hpp"

int main(){
    OrderBook ob;

    OBOrder b{1,100,100,true};
    OBOrder a{2,100,100,false};

    ob.add(b);
    ob.add(a);

    double traded = ob.match_level(100);
    assert(traded == 100);
}
