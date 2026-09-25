# helios-gateway: the public game edge (04 §1, §2.3–2.6, §9). ROLE gateway (from the directory):
# HEADLESS modules only, AVX2 image with the CPU gate. See README.md.
helios_executable(helios-gateway SOURCES main.cpp DEPS helios::server)
