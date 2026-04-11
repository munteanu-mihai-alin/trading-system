
#include <iostream>
#include "engine/engine.cpp"

int main(){
    Engine e;

    for(int i=0;i<1000;i++) e.step();

    std::cout<<"predicted="<<e.metrics.predicted_hits
             <<" realized="<<e.metrics.realized_hits<<"\n";
}
