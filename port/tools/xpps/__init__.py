"""Readers for the KCAP packages (.xpps) of ISS and the shader index built from them.

Modules:
    kcap   container header, sections, items and BSS
    knli   link tables: relocations (ILNK), class ids (CID), imports (XLNK), exports
    gnm    OrbShdr footer, code start rule and the Gnm VS/PS/CS headers
    scan   everything the precompiler needs from one package
    index  SQLite index of a whole folder (incremental), CLI: python -m xpps.index

Only structure is described here: no byte, hash or name from the game is part of the code.
"""
