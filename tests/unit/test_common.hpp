
#pragma once
#include <iostream>
#include <cmath>

inline void assert_true(bool cond, const char* msg) {
    if(!cond) {
        std::cerr << "FAIL: " << msg << std::endl;
        exit(1);
    }
}

inline void assert_close(double a,double b,double eps,const char* msg) {
    if(std::fabs(a-b) > eps) {
        std::cerr << "FAIL: " << msg << " got="<<a<<" expected="<<b<<std::endl;
        exit(1);
    }
}
