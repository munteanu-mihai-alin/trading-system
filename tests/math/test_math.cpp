
#include "../../include/execution/score.hpp"
#include "../unit/test_common.hpp"

/*
Test mathematical properties:

1. Increasing distance → lower p_touch → lower score
2. Increasing lambda → higher fill probability → higher score
*/

void test_distance_monotonic() {
    double s1 = compute_score(100,99.9,0.01,50,1000,1);
    double s2 = compute_score(100,99.0,0.01,50,1000,1);
    assert_true(s1 > s2, "closer limit should have higher score");
}

void test_lambda_effect() {
    double s1 = compute_score(100,99.9,0.01,10,1000,1);
    double s2 = compute_score(100,99.9,0.01,100,1000,1);
    assert_true(s2 > s1, "higher lambda should increase score");
}

int main(){
    test_distance_monotonic();
    test_lambda_effect();
    std::cout<<"Math tests passed\n";
    return 0;
}
