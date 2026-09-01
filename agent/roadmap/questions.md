# Roadmap questions — user verbatim, 2026-05-30

Preserved as the user wrote them. Discussion + per-question answers live in [`answers.md`](answers.md); the tackle order lives in [`plan.md`](plan.md).

---

**1.** If we have already placed orders from another session(live), we need the system to pick those up, but if for any reason the price is above our target, we need to set the sell order around that price. This should also apply for the case when we would have a sudden increase in price above our target. Detail how it happens right now.

**2.** What if we place the sell around the same time with the buy. Worth investigating.

**3.** For ranking buys, maybe we have some stock that has a good score, much better than others. We should adjust cooldown to be shorter for this one, or allow multiple orders from this one too. (2/3 or 3/3 for one stock) but should investigate.

**4.** Explain exactly what happens step by step for a buy and step by step for a sell.

**5.** Average fill time for buy and for sell.

**6.** App. I need a mobile app that should allow to view overall results (continuous run lets say on live/paper, but as we switch between that may be a problem):
- Should have an intuitive menu that should let you see paper / live overall performance: when orders were placed, when they were filled, average wait time to fill for buy/ sell limit orders.
- Should be able to see average memory usage, p99 and other latencies for placing buy, sell, and other important substeps (I think we already see something like that in steps.csv or other csvs).
- It should allow to see an item list of runs (our backtests) that should show the same things as the one above.
- It should have health/status check.
- It should use something running on hetzner to get data it currently does not or something.
- be able to communicate with claude code to investigate if some kind of problem appear and make me a short summary on phone.
- it should push notifications for when system is down/ too much mem usage/ too much disk usage.
- I think all these infos should be also present in report. And the app should basically per each run show me the report that is generated.

**7.** Daemon/systemd proc:
**(This section was drafted BEFORE the alert-only kill switch + user kill switch landed - keep that in mind when reading.)**
- killswitch: like for killing the process
- killswitch for trading: force selling in loss so we save the portfolio. (must decide when such a thing would happen, most probably from the app at 6)
- restart
- it should monitor mem / disk usage, also when there are bugs or something

**8.** Other daemon/process to launch backtests. This one should monitor the binaries used in backtests. The one at 7 should monitor this one also.

**9.** I should be able to launch a test from the app and be able to specify:
- config params (should have a default value like we used)
- period the test runs
- symbol universe (I think I should be able to pick)
- it should be able to connect to databento to see how many remaining credits there are.
- I should also have a chat that interacts with you when a test fails for some unknown reason. (but that is maybe the future use for this) I should be able to pick from codex, claude code and cursor. These platforms should be able to connect to hetzner and check even if I'm on the phone.

**10.** Please make the html report with all ratios near each other and maybe top of the item list.

**11.** For trading live/paper, a mechanism to be able to entry the market with some already ranking made. So like I start the bot at 11:02:35. We should already have the ranking for the symbols, right? or just wait until we have the ranking after we start the system? To be discussed with me.

**12.** Related to APP and daemon, you should also suggest improvements or things that should be monitored.

**13.** Decide if for the trading hours for our symbols there is a single window or multiple. Do we need to kill the binary outside trading hours and relaunch it during it? Or do we do like paper trading outside live trading hours for our stock?
