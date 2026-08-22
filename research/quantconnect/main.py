# QuantConnect entry point. Switches which strategy runs.
#
# QC's cloud runner does not always put the project directory on
# sys.path, so `from HawkesOU_MR import ...` fails with
# "No module named 'HawkesOU_MR'". The sys.path.insert below fixes it
# so sibling files in the same project directory become importable.
#
# To switch strategies: uncomment exactly ONE of the imports below,
# comment the others. `Algorithm` inherits from whichever _Strategy
# is active, and QC picks it up as the algorithm class.
#
# Each strategy file is self-contained: its own constants, universe,
# signal parameters, state class, and QCAlgorithm subclass. Nothing
# lives in this file except the switch and the trivial subclass.

from AlgorithmImports import *
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from HawkesOU_MR                import HftHawkesOuMR              as _Strategy
# from HawkesOU_MR_Reinvest_Wide import HftHawkesOuMrReinvestWide as _Strategy
# from HawkesOU_MR_Reinvest_Open import HftHawkesOuMrReinvestOpen as _Strategy
# from SPY_Ladder                import HftSpyLadder               as _Strategy


class Algorithm(_Strategy):
    pass
