
#include <iostream>
#include "engine/engine.cpp"

int main(){

    Engine eng;

    for(int i=0;i<30;i++){
        Stock s;
        s.symbol = "STK" + std::to_string(i);
        eng.stocks.push_back(s);
    }

    for(int t=0;t<1000;t++){
        eng.step();
    }

    std::cout << "Done\n";
}
