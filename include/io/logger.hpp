
#pragma once
#include <fstream>

struct Logger {
    std::ofstream out;

    Logger(const std::string& file){
        out.open(file);
        out << "step,symbol,mode,pnl,total_pnl,trades\n";
    }

    void log(int step, const std::string& sym, const std::string& mode,
             double pnl, double total, int trades){
        out << step << "," << sym << "," << mode << ","
            << pnl << "," << total << "," << trades << "\n";
    }
};
