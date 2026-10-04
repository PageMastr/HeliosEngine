// Initializer braces after =, a comma, a brace, return, a template's > and an array bound keep a read
// statement open.
net::Address listen = {
    net::Address::ipv4(127, 0, 0, 1, 7054)};
auto listen = pick(kFallback, {
    "127.0.0.1:7055"});
std::vector<std::string> listen{{
    "127.0.0.1:7056"}};
std::string pick(bool connect) {
    if (connect) return {
        "127.0.0.1:7057"};
    return {};
}
net::Address listen[1]{
    net::Address::ipv4(127, 0, 0, 1, 7058)};
auto listen = std::vector<std::string>{
    "127.0.0.1:7060"};
std::string listen[1][1]{
    {"127.0.0.1:7061"}};
auto* listen = new std::vector<std::string>[1]{
    {"127.0.0.1:7062"}};
// Lambdas closed inside parentheses leave the depth where `;` ends a statement as it was.
void probe() {
    run([&] { each([&] { step(); }); });
    for (auto listen = first(); listen.retry();
         next = net::Address::ipv4(127, 0, 0, 1, 7059)) {}
}
