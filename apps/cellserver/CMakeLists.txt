# helios-cell: headless cell server (04 §1, §3). ROLE cell (from the directory): HEADLESS modules
# only, AVX2 image with the CPU gate. See README.md.
helios_executable(helios-cell SOURCES main.cpp DEPS helios::server)
