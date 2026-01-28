# Fit & Tolerance Test Prints

These test prints are provided to validate printer accuracy, filament behavior, and hardware fit before printing the main Planetary Bean Bouncer parts.

## When to Use These
- New printer or newly tuned printer
- New filament brand or material
- If previous prints showed tight fits, binding, or gear noise

---

## Test_1.stl – Hardware Fit Test

**Purpose:**  
Validates all commonly used hardware fits in this project.

**Covers:**
- Screw and nut sizes used throughout the build
- Bearing fits
- Lever-lock electrical terminal fit

**How to Evaluate:**
- Screws should insert without excessive force
- Nuts should press in firmly without cracking
- Bearings should be snug but removable by hand
- Lever-lock terminal should slide in without distortion

If fits are too tight or loose, adjust:
- Horizontal expansion
- Flow rate
- Extrusion multiplier

---

## Gearing_5_1_16_24_64_Test_Rev36.stl – Planetary Gear Test

**Purpose:**  
A compact planetary gear set using the same tolerances as the full gearbox.

**Notes:**
- Functionally equivalent tolerances to the full-size gear system
- Can be assembled and rotated by hand
- Small enough for quick test prints
- Also doubles as a fidget after printing

**Pass Criteria:**
- Gears rotate smoothly by hand
- No binding or tooth fusion
- Minimal backlash consistent with printed gears

---

## Recommended Print Settings
(Use the same settings intended for the final parts)

- Layer height: ___
- Nozzle size: ___
- Material: ___
- Perimeters: ___

---

## If Tests Fail
- Do **not** force assemblies
- Adjust tolerances or slicer settings
- Reprint tests before printing final parts