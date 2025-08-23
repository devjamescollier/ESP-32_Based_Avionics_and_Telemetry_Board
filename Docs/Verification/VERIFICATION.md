# Verification record — ESP32-Based Avionics Board, Rev A

ERC, DRC, Gerber/drill regeneration, BOM/CPL reconciliation and the stock check were re-run on 2026-11-09 with KiCad 10.0.5 (`kicad-cli`) on the files in `Hardware/KiCad_Files/`, as committed.
The table below covers design and manufacturing-file checks. Bench results from the assembled prototype are in the [README](../../README.md#results).

| Check | Command / method | Result |
|---|---|---|
| ERC | `kicad-cli sch erc --severity-all` | **PASS:** 0 errors, 0 warnings ([ERC.rpt](ERC.rpt)). The "ignored checks" listed in the report are KiCad's defaults (single global label, four-way junction, SPICE model, footprint filters); none is set by this project |
| DRC + connectivity + parity | `kicad-cli pcb drc --severity-all --schematic-parity --refill-zones` | **PASS:** 0 violations, 0 unconnected pads, 0 footprint errors, 0 parity issues, no ignored checks ([DRC.rpt](DRC.rpt)) |
| JLCPCB capability limits | measured on the board against jlcpcb.com/capabilities (retrieved 2026-10-28) | **PASS for 4-layer.** Every feature also meets the 2-layer minimums, but the board is **4-layer** and cannot be built as 2-layer (see below) |
| BOM ↔ CPL ↔ PCB | `Manufacturing/Assembly` regenerated; designators reconciled against the PCB ([BOM_CPL_Check.json](BOM_CPL_Check.json)) | **PASS:** 87 SMT designators in BOM = CPL = PCB SMT footprints; 8 through-hole parts listed for hand assembly; 95 parts total = PCB |
| LCSC stock | JLCPCB/LCSC parts snapshot dated 2026-10-27, checked 2026-11-09 | **PASS (snapshot):** all 33 SMT lines and all 4 through-hole lines in stock; lowest is MS5611 (C15639) at 2,842. SMT: 17 Extended, 2 Preferred, 14 Basic. Re-confirm live stock at checkout |
| Gerber / drill freshness | re-exported from the PCB with `kicad-cli` and diffed against `Manufacturing/Gerbers` and the zip | **PASS:** every layer and drill file identical apart from timestamps; zip identical to folder |
| CPL ↔ PCB | `kicad-cli pcb export pos` compared to `positions.csv` | **PASS:** 87 parts, identical X/Y/rotation/side |
| Firmware ↔ schematic | GPIO, I²C address and WHO_AM_I defines in `fc_diag.c` compared to the netlist | **PASS:** all 22 pin assignments match |
| Gerber / drill check | independent RS-274X and Excellon parser ([Gerber_Check.txt](Gerber_Check.txt)) | **PASS:** 50.00 × 80.00 mm outline; 430 plated holes on copper; 4 non-plated M3 holes clear of copper; no copper within 0.2 mm of the edge |
| CPL rotations | `Manufacturing/Assembly/CPL_rotation_review.csv` (23 polarized parts) | **Check at order time:** rotations are KiCad-native; whether JLC's part models need an offset can only be seen in JLC's placement preview |
| Schematic layout change | netlist before vs after the Rev A re-layout | **PASS:** all 347 pins on identical nets; 13 unnamed nets given names; parts, values and footprints unchanged |
| Firmware | `idf.py set-target esp32 && idf.py build` (ESP-IDF v6.1-beta1) | **PASS:** builds with 0 warnings; run on the prototype (see README results) |

## JLCPCB capability comparison

| Feature | Board | JLC 2-layer | JLC 4-layer |
|---|---|---|---|
| Copper layers | **4** | 2 | 4 |
| Minimum trace width | 0.20 mm | 0.10 mm | 0.09 mm |
| Clearance (design rule) | 0.15 mm | 0.10 mm | 0.09 mm |
| Via drill / diameter | 0.30 / 0.60 mm | 0.15 / 0.25 mm | 0.15 / 0.25 mm |
| Minimum through-hole annular ring | 0.20 mm (J7 locating pegs) | ≥ 0.18 mm (0.25 recommended) | ≥ 0.15 mm (0.20 recommended) |
| Component hole to hole | 1.54 mm | ≥ 0.45 mm | ≥ 0.45 mm |
| Via hole to hole | 0.37 mm | ≥ 0.20 mm | ≥ 0.20 mm |
| Non-plated hole | 3.2 mm | ≥ 0.5 mm | ≥ 0.5 mm |
| Copper to board edge | 0.3 mm (rule) | ≥ 0.2 mm | ≥ 0.2 mm |
| Silkscreen text height / stroke | 1.0 / 0.15 mm | 1.0 / 0.15 mm | 1.0 / 0.15 mm |
| Solder-mask bridge | 0.15 mm | ≥ 0.10 mm | ≥ 0.10 mm |

The design needs four layers: In1 is a continuous GND plane under the USB, SPI and sensor routing, In2 carries +3.3 V, and
the router uses In2 for crossings. Order it as a 4-layer board (stackup JLC04161H-7628, 1.6 mm, ENIG).
