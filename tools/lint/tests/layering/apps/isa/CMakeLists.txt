# The ISA override is for the audit's fixtures: under apps/, an image's level comes from its role.
helios_executable(fx-isa-app ROLE client ISA base NO_CPU_GATE SOURCES ${src} DEPS helios::core)
