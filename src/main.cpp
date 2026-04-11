
#include <iostream>
#include "engine/engine.cpp"

int main(){
    Engine e;
    for(int i=0;i<30;i++) e.stocks.push_back(Stock{});

    for(int t=0;t<200;t++) e.step();

    std::cout<<"done\n";
}
