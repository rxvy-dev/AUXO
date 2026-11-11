"""auxo-installer for AuxoBSD: a graphical (Qt) installer for the live ISO.

  config.py   what the user chose
  system.py   facts read from the live system (disks, keyboards, time zones, GPUs)
  plan.py     turns the choices into install steps — pure, so tests can check every command
  runner.py   runs the steps (with progress)
  gui.py      the PyQt6 wizard
"""
