// A file stem that is not a C identifier: helios_shaders() exposes it as dash_name() and finds it
// by its real stem, find("dash-name") (test_shaders.cpp).
[shader("compute")]
[numthreads(1, 1, 1)]
void csMain(uint3 id : SV_DispatchThreadID) {}
