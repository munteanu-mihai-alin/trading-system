#include <thread>
#include <chrono>
#include <iostream>
#include <sstream>

#include "core/types.hpp"
#include "models/micro.hpp"
#include "models/ou.hpp"
#include "models/hawkes.hpp"
#include "execution/score.hpp"
#include "infra/spsc_queue.hpp"
#include "log/buffered_logger.hpp"
#include "core/portfolio.hpp"

SPSCQueue<MarketEvent, 1<<14> md_queue;

void md_thread() {
    while (true) {
        MarketEvent ev{0,100,100.02,1000,900,100.01,10,0};
        md_queue.push(ev);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void strategy_thread() {
    Portfolio pf;
    pf.stocks = {{"AAPL"},{"MSFT"},{"NVDA"}};

    BufferedLogger log;

    while (true) {
        MarketEvent ev;
        while (md_queue.pop(ev)) {

            for (auto& s : pf.stocks) {

                s.mid += 0.001 * ((rand()%3)-1);

                update_ou(s.ou, s.mid);
                s.hawkes.update(0.001, rand()%2);

                double mu = (s.ou.mean - s.mid) * (1.0 - s.ou.a);
                double latency = s.latency.mean_latency();

                double best = s.mid;
                double best_score = -1e9;

                for (int i=0;i<32;i++) {
                    double L = s.mid - 0.3 + i*0.02;
                    double sc = compute_score(s.mid, L, mu, s.hawkes.lambda, s.queue, latency);

                    if (sc > best_score) {
                        best_score = sc;
                        best = L;
                    }
                }

                s.score = best_score;
                s.best_limit = best;

                std::ostringstream ss;
                ss << s.symbol << " score=" << s.score;
                log.log(ss.str());
            }

            pf.rank();
        }
    }
}

int main() {
    std::thread t1(md_thread);
    std::thread t2(strategy_thread);

    t1.join();
    t2.join();

    return 0;
}
