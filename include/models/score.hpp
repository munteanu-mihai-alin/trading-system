
#pragma once
#include <cmath>

inline double compute_score(double mid,double L,double lambda,double q){
    double d=fabs(mid-L);
    double pt=exp(-5*d);
    double fill=1-exp(-lambda/(q+1e-9));
    return pt*fill;
}
