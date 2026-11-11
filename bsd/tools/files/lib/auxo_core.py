"""Compatibility shim: lets the shared Auxo Linux modules (auxo_kde.py) run on AuxoBSD
by giving them the FreeBSD helpers under the name they import."""
from auxo_bsd import *  # noqa: F401,F403
from auxo_bsd import R, log, write_file, chown, home_of  # noqa: F401
