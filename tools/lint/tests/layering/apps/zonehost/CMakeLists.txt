# A helios_executable() in an apps/ directory with no known role and no ROLE (case unknown_role): with
# an empty role no role check would see that a cell-like image links render.
helios_executable(fx-zonehost SOURCES ${src} DEPS helios::world helios::render)
