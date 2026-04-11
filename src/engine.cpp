
#include <vector>
#include <algorithm>
#include <random>

#include "models/stock.hpp"
#include "fill/fill.hpp"
#include "sim/orderbook.hpp"
#include "sim/sim.hpp"

struct Engine {
    std::vector<Stock> stocks;
    FillModel fill;
    Simulator sim;
    OrderBook ob;

    void step(){
        for(auto& s: stocks){
            int event = rand()%2;
            s.hawkes.update(0.001, event);

            // choose price level (simple grid)
            double bestS = -1e9;
            double bestL = s.mid;

            for(int i=0;i<8;i++){
                double L = s.mid - 0.1 + i*0.025;
                double dist = std::abs(s.mid - L);

                // simulate traded volume at this level
                double traded = sim.step(ob, L);

                // update queue tracking
                s.my.on_traded(traded);

                double p = fill.compute(traded, s.my.queue_ahead + 1e-9, dist);
                double sc = p * s.hawkes.lambda;

                if(sc > bestS){
                    bestS = sc;
                    bestL = L;
                }
            }

            s.best_L = bestL;
            s.score = bestS;

            // assume we join the queue at best level with some ahead volume
            s.my.price = bestL;
            s.my.queue_ahead = (rand()%500) + 50; // initial ahead
        }

        std::sort(stocks.begin(), stocks.end(), [](auto&a,auto&b){
            return a.score > b.score;
        });
    }
};
