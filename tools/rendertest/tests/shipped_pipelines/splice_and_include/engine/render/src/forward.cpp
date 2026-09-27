// Fixture: review round 3 L8 and L9: a backslash-newline splice inside the identifier (also with a
// blank before the newline), and a direct call in an .inc file that forward.cpp includes (4 findings,
// each at the first physical line of its logical line).
    (void)device.createGraphics\
Pipeline(wire);
    (void)device.createCompute\ 
Pipeline(compute);
    (void)device.createGraphicsPipeline(wire);
#include "forward_variants.inc"
