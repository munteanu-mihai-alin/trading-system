
#pragma once
struct ICostModel {
    virtual double cost(double qty)=0;
    virtual ~ICostModel() = default;
};
