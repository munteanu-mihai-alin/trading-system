
#include <iostream>
#include <vector>
#include <algorithm>
#include <random>

#include "bench/rdtsc.hpp"
#include "bench/bench.hpp"
#include "models/stock.hpp"
#include "models/score.hpp"
#include "io/logger.hpp"

int main(){

    const int N = 30;
    const int TOP_K = 3;
    const int COOLDOWN = 50;

    std::vector<Stock> stocks(N);

    for(int i=0;i<N;i++)
        stocks[i].symbol = "STK" + std::to_string(i);

    std::mt19937 rng(42);
    std::uniform_real_distribution<> noise(-0.05,0.05);

    std::vector<uint64_t> samples;
    Logger log("shadow_results.csv");

    for(int t=0;t<10000;t++){

        uint64_t start = rdtsc_start();

        // update + scoring
        for(auto& s : stocks){

            int event = rng()%2;
            s.hawkes.update(0.001, event);

            s.mid += noise(rng);

            if(s.cooldown > 0) s.cooldown--;

            double bestS = -1e9;

            for(int i=0;i<16;i++){
                double L = s.mid - 0.2 + i*0.02;
                double sc = compute_score(s.mid, L, s.hawkes.lambda, s.queue);

                if(sc > bestS){
                    bestS = sc;
                    s.best_L = L;
                    s.score = sc;
                }
            }

            if(s.cooldown > 0)
                s.score *= 0.1;
        }

        // ranking
        std::sort(stocks.begin(), stocks.end(), [](auto& a, auto& b){
            return a.score > b.score;
        });

        // assign top K active
        for(int i=0;i<N;i++)
            stocks[i].active = (i < TOP_K);

        // simulate trades
        for(auto& s : stocks){

            bool signal = (rng()%5==0); // sparse trading signal

            if(!signal) continue;

            double pnl = noise(rng); // simulated PnL

            if(s.active){
                s.real.update(pnl);
                log.log(t, s.symbol, "real", pnl, s.real.pnl, s.real.trades);

                if(pnl > 0)
                    s.cooldown = COOLDOWN;
            } else {
                s.shadow.update(pnl);
                log.log(t, s.symbol, "shadow", pnl, s.shadow.pnl, s.shadow.trades);
            }
        }

        uint64_t end = rdtsc_end();
        samples.push_back(end - start);
    }

    report(samples);

    std::cout << "Done. Results in shadow_results.csv\n";
}
