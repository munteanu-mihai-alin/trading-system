# QuantConnect entry point. Switches which strategy runs.
#
# To swap strategies: uncomment exactly ONE of the imports below,
# comment the others. The active _Strategy is what Algorithm inherits
# from, and QC picks Algorithm up as the algorithm class.
#
# Each strategy file is self-contained: its own constants, universe,
# signal parameters, state class, and QCAlgorithm subclass. Nothing
# lives in this file except the switch and the trivial subclass.
#
# File / class map:
#   HawkesOU_MR              -> HftHawkesOuMR              (baseline)
#   HawkesOU_MR_Reinvest_Wide -> HftHawkesOuMrReinvestWide (reinvest + wide gate)
#   HawkesOU_MR_Reinvest_Open -> HftHawkesOuMrReinvestOpen (reinvest, no gate)

from HawkesOU_MR                 import HftHawkesOuMR              as _Strategy
# from HawkesOU_MR_Reinvest_Wide  import HftHawkesOuMrReinvestWide  as _Strategy
# from HawkesOU_MR_Reinvest_Open  import HftHawkesOuMrReinvestOpen  as _Strategy


class Algorithm(_Strategy):
    pass
