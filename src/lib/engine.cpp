
#include <vector>
#include <algorithm>
#include <random>

#include "models/stock.hpp"
#include "models/score.hpp"

struct Engine {

    std::vector<Stock> stocks;
    int TOP_K = 3;

    std::mt19937 rng{42};

    void step(){

        for(auto& s: stocks){

            int event = rng()%2;
            s.hawkes.update(0.001, event);

            double bestS=-1e9;

            for(int i=0;i<16;i++){
                double L = s.mid - 0.2 + i*0.02;
                double sc = compute_score(s.mid, L, s.hawkes.lambda, s.queue);

                if(sc>bestS){
                    bestS=sc;
                    s.best_L=L;
                    s.score=sc;
                }
            }

            if(s.cooldown>0){
                s.score *= 0.1;
                s.cooldown--;
            }
        }

        std::sort(stocks.begin(), stocks.end(), [](auto&a,auto&b){
            return a.score > b.score;
        });

        for(int i=0;i<stocks.size();i++)
            stocks[i].active = (i < TOP_K);

        for(auto& s: stocks){

            bool signal = (rng()%5==0);
            if(!signal) continue;

            double pnl = (rng()%100)/10000.0 - 0.005;

            if(s.active){
                s.real.update(pnl);
                if(pnl>0) s.cooldown=50;
            } else {
                s.shadow.update(pnl);
            }
        }
    }
};
