#include "common/TestFramework.hpp"

#include "engine/RankingEngine.hpp"

using namespace hft;

HFT_TEST(test_ranking_engine_initialize_and_step_paths) {
    RankingEngine engine(3, "tmp_shadow_results.csv");
    engine.initialize(8);
    hft::test::require(engine.portfolio.items.size() == 8, "engine should initialize requested universe size");

    for (int t = 0; t < 5; ++t) {
        engine.step(t);
    }

    hft::test::require(!engine.cycle_samples.empty(), "engine should record cycle samples");
    hft::test::require(engine.validation.size() > 0, "engine should accumulate validation samples");
}
