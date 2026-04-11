
#include "../../include/execution/score.hpp"
#include "../unit/test_common.hpp"

void test_basic_score_positive() {
    double s = compute_score(100,99.9,0.01,50,1000,1);
    assert_true(s != 0, "score should not be zero");
}

int main(){
    test_basic_score_positive();
    std::cout<<"Unit tests passed\n";
    return 0;
}
