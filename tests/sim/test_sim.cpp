
#include "../../include/execution/score.hpp"
#include "../unit/test_common.hpp"

/*
Simulate different conditions:
- high latency reduces score
- high spread scenario
*/

void test_latency_effect() {
    double s1 = compute_score(100,99.9,0.01,50,1000,1);
    double s2 = compute_score(100,99.9,0.01,50,1000,50);
    assert_true(s1 > s2, "higher latency should reduce score");
}

int main(){
    test_latency_effect();
    std::cout<<"Simulation tests passed\n";
    return 0;
}
