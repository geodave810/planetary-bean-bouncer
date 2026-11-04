# Planetary Bean Bouncer — ESP32-S3 Hardware

This directory contains hardware design files for the ESP32-S3 version of the **Planetary Bean Bouncer Coffee Roaster**, including 3D-printable STL files, mechanical components, electronics design files, and reference documentation.

The ESP32-S3 version builds on the original Arduino-controlled Planetary Bean Bouncer, retaining the same basic roasting concept while incorporating an updated controller and several mechanical improvements.

The original Arduino version remains a fully functional, lower-cost alternative. Its hardware files are retained in the original project directories.

## Mechanical Design

The Planetary Bean Bouncer uses a modified 8-cup stainless steel flour sifter, a planetary gear-driven agitator, and a stationary heat gun.

The ESP32-S3 version includes several refinements intended to simplify assembly and improve the mechanical design.

### Planetary Gearbox

The original Planetary Bean Bouncer used a fully 3D-printed planetary gearbox based on an open-source design by Emmett Lalish.

The current ESP32-S3 build uses a commercially manufactured planetary gear assembly, substantially reducing the number of parts that need to be 3D printed.

The original printable gearbox remains available in the original hardware directory for builders who prefer to manufacture their own gears.

### Agitator Shaft and Wobble Disc

The original design used a 5 mm shaft.

The stainless steel 3D-printed wobble disc was subsequently drilled out to accommodate a 6 mm shaft. This arrangement has performed well under the operating load.

The updated 3D-printed shaft component includes a 6.5 mm clearance hole and is provided with the ESP32-S3 mechanical files.

Builders should use the matching shaft and wobble-disc components for their chosen assembly.

### 3D-Printed Components

The STL files include the custom mechanical components needed for the updated build.

Refer to the build guide for assembly instructions, part orientation, hardware requirements, and installation details.

Some components from the original design may still be suitable for the ESP32-S3 version. Consult the current build guide and BOM before selecting parts.

## ESP32-S3 Controller PCB

The updated controller uses a custom PCB designed in KiCad.

The PCB integrates the ESP32-S3 development module, motor-control circuitry, power regulation, and connections for the display, temperature sensor, real-time clock, and other peripherals.

The KiCad design files and manufacturing Gerbers are included with the project.

### Gerber Files

The supplied Gerber files include the decorative back silkscreen image used on the manufactured controller board.

The image was intentionally removed from the published KiCad PCB source file to reduce file size and eliminate image-clipping warnings.

Builders can use the supplied Gerbers to reproduce the original manufactured board or generate new Gerbers from the KiCad design without the decorative image.

## Bill of Materials (BOM)

A bill of materials is provided with the ESP32-S3 build documentation.

The BOM identifies the mechanical components, electronics, fasteners, and other hardware used in the current build.

The listed parts reflect the author's design and component selections. Equivalent components may be substituted where appropriate, provided they are mechanically and electrically compatible.

Actual costs will vary depending on suppliers, availability, and components already on hand.

## Fabricating Your Own Wobble Disc

For builders interested in fabricating their own wobble disc rather than using a metal 3D-printed component, the following guide helped inspire the original design:

**Wobble Disc Fabrication Guide**

https://library.sweetmarias.com/wp-content/uploads/2024/09/Wobble-Disc-Coffee-Roaster-3-red-tubes-model-build-instructions.pdf

The guide provides information about measurements, materials, and assembly considerations.

When fabricating a replacement disc, ensure that the shaft dimensions, balance, and mechanical clearances are appropriate for the selected assembly.

## Licensing

The original hardware design files in this project are licensed under:

**Creative Commons Attribution–NonCommercial 4.0 International (CC BY-NC 4.0)**

https://creativecommons.org/licenses/by-nc/4.0/

This license permits sharing and adaptation with appropriate attribution for noncommercial purposes.

Firmware is licensed separately under the MIT License.

Third-party components and designs retain their respective original licenses.

See the top-level license files for additional information.

## Commercial Use

If you are interested in using the original hardware designs for commercial purposes, including kits, manufactured products, or resale, please contact the author to discuss licensing options.

Commercial licensing helps support continued development of the Planetary Bean Bouncer.

## Credits and Attribution

### Original Planetary Gear System

The original 3D-printed planetary gearbox was based on:

- **Planetary Gear Set** by Emmett Lalish
- Source: https://www.thingiverse.com/thing:53451
- License: As specified by the original author on Thingiverse

The design was modified for the Planetary Bean Bouncer, including changes to dimensions, tolerances, and mechanical integration.

The original design remains available as an alternative to the commercially manufactured planetary gearbox.

All third-party designs remain subject to their original licensing terms.

## Additional Documentation

For complete assembly instructions, wiring, firmware configuration, and component information, refer to the ESP32-S3 build guide and bill of materials.

For project history, photographs, videos, and additional information, see the main repository README.