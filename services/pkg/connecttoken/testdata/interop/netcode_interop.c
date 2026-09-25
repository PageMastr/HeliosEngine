/*
    Helios connect-token interop harness (test-only, never shipped).

    Compiles the vendored netcode v1.4.8 (third_party/netcode) directly into this translation unit
    so it can use netcode's own internal token writer/reader/encryptor, and cross-checks the Go
    implementation in services/pkg/connecttoken:

      gen-fixed <out.json>       token from fixed inputs via netcode's internal writer + XChaCha20
                                 encryptor (byte-exact golden vector for Go)
      gen-api <out.json>         token via the public netcode_generate_connect_token (random nonce
                                 and session keys, fixed private key) for Go to parse and decrypt
      verify <token.bin> <key.hex>
                                 parse + decrypt a Go-generated token, print its fields as JSON
      connect <token.bin> <key.hex> <server-address>
                                 run a netcode server (modelled as up for 60 s) and client over
                                 netcode's network simulator and complete the full handshake with
                                 a Go-generated token

    Build (the Go test TestNetcodeCInterop does this automatically when HELIOS_NETCODE_INTEROP=1):
      cc -std=gnu11 -O1 -I third_party/netcode -I third_party/netcode/sodium \
         netcode_interop.c third_party/netcode/sodium/sodium.c -lm   (+ -lws2_32 -liphlpapi on Windows)
    (gnu11, not c11: netcode's POSIX timing calls need the POSIX declarations strict ISO C hides.)
*/

#define NETCODE_ENABLE_TESTS 0
#include "netcode.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INTEROP_PROTOCOL_ID 0x48454C494F530001ULL

static void fill_pattern( uint8_t * dst, int n, int mul, int add )
{
    int i;
    for ( i = 0; i < n; i++ )
        dst[i] = (uint8_t) ( ( i * mul + add ) & 0xFF );
}

static void print_hex( FILE * f, const uint8_t * data, int n )
{
    int i;
    for ( i = 0; i < n; i++ )
        fprintf( f, "%02x", data[i] );
}

static int parse_hex( const char * hex, uint8_t * out, int n )
{
    int i;
    if ( (int) strlen( hex ) < 2 * n )
        return 0;
    for ( i = 0; i < n; i++ )
    {
        unsigned int v;
        if ( sscanf( hex + 2 * i, "%2x", &v ) != 1 )
            return 0;
        out[i] = (uint8_t) v;
    }
    return 1;
}

static int read_file( const char * path, uint8_t * out, int n )
{
    FILE * f = fopen( path, "rb" );
    if ( !f )
        return 0;
    int got = (int) fread( out, 1, (size_t) n, f );
    fclose( f );
    return got == n;
}

static int read_key_file( const char * path, uint8_t * key )
{
    char hex[2 * NETCODE_KEY_BYTES + 8];
    memset( hex, 0, sizeof( hex ) );
    FILE * f = fopen( path, "rb" );
    if ( !f )
        return 0;
    size_t got = fread( hex, 1, 2 * NETCODE_KEY_BYTES, f );
    fclose( f );
    return got == 2 * NETCODE_KEY_BYTES && parse_hex( hex, key, NETCODE_KEY_BYTES );
}

static void print_address_list( FILE * f, struct netcode_address_t * addresses, int n )
{
    int i;
    char buffer[NETCODE_MAX_ADDRESS_STRING_LENGTH];
    fprintf( f, "[" );
    for ( i = 0; i < n; i++ )
        fprintf( f, "%s\"%s\"", i ? "," : "", netcode_address_to_string( &addresses[i], buffer ) );
    fprintf( f, "]" );
}

struct fixed_case
{
    const char * name;
    int num_addresses;
    const char * public_addresses[4];
    const char * internal_addresses[4];
    int timeout_seconds;
    uint64_t client_id;
    uint64_t create_timestamp;
    uint64_t expire_timestamp;
};

static const struct fixed_case fixed_cases[] =
{
    { "fixed-three-addresses", 3,
      { "127.0.0.1:40000", "[::1]:40001", "10.0.0.5:7777" },
      { "127.0.0.1:40000", "[::1]:40001", "192.168.1.20:7777" },
      10, 0x0123456789ABCDEFULL, 1790000000ULL, 1790000045ULL },
    { "fixed-ipv6-no-timeout", 1,
      { "[2001:db8:85a3::8a2e:370:7334]:65535" },
      { "[2001:db8:85a3::8a2e:370:7334]:65535" },
      -1, 0xFEDCBA9876543210ULL, 1790000000ULL, 0xFFFFFFFFFFFFFFFFULL },
};

static int write_fixed_case( FILE * f, const struct fixed_case * c, int index )
{
    uint8_t private_key[NETCODE_KEY_BYTES];
    uint8_t nonce[NETCODE_CONNECT_TOKEN_NONCE_BYTES];
    fill_pattern( private_key, NETCODE_KEY_BYTES, 1, 0x60 + index );
    fill_pattern( nonce, NETCODE_CONNECT_TOKEN_NONCE_BYTES, 7, 1 + index );

    struct netcode_connect_token_private_t priv;
    memset( &priv, 0, sizeof( priv ) );
    priv.client_id = c->client_id;
    priv.timeout_seconds = c->timeout_seconds;
    priv.num_server_addresses = c->num_addresses;
    int i;
    for ( i = 0; i < c->num_addresses; i++ )
    {
        if ( netcode_parse_address( c->internal_addresses[i], &priv.server_addresses[i] ) != NETCODE_OK )
            return 0;
    }
    fill_pattern( priv.client_to_server_key, NETCODE_KEY_BYTES, 1, 0xA0 + index );
    fill_pattern( priv.server_to_client_key, NETCODE_KEY_BYTES, 1, 0xC0 + index );
    fill_pattern( priv.user_data, NETCODE_USER_DATA_BYTES, 3, 5 + index );

    uint8_t private_data[NETCODE_CONNECT_TOKEN_PRIVATE_BYTES];
    netcode_write_connect_token_private( &priv, private_data, NETCODE_CONNECT_TOKEN_PRIVATE_BYTES );
    if ( netcode_encrypt_connect_token_private( private_data, NETCODE_CONNECT_TOKEN_PRIVATE_BYTES, NETCODE_VERSION_INFO,
                                                INTEROP_PROTOCOL_ID, c->expire_timestamp, nonce, private_key ) != NETCODE_OK )
        return 0;

    struct netcode_connect_token_t token;
    memset( &token, 0, sizeof( token ) );
    memcpy( token.version_info, NETCODE_VERSION_INFO, NETCODE_VERSION_INFO_BYTES );
    token.protocol_id = INTEROP_PROTOCOL_ID;
    token.create_timestamp = c->create_timestamp;
    token.expire_timestamp = c->expire_timestamp;
    memcpy( token.nonce, nonce, sizeof( nonce ) );
    memcpy( token.private_data, private_data, sizeof( private_data ) );
    token.timeout_seconds = c->timeout_seconds;
    token.num_server_addresses = c->num_addresses;
    for ( i = 0; i < c->num_addresses; i++ )
    {
        if ( netcode_parse_address( c->public_addresses[i], &token.server_addresses[i] ) != NETCODE_OK )
            return 0;
    }
    memcpy( token.client_to_server_key, priv.client_to_server_key, NETCODE_KEY_BYTES );
    memcpy( token.server_to_client_key, priv.server_to_client_key, NETCODE_KEY_BYTES );

    uint8_t buffer[NETCODE_CONNECT_TOKEN_BYTES];
    netcode_write_connect_token( &token, buffer, NETCODE_CONNECT_TOKEN_BYTES );

    fprintf( f, "    {\n" );
    fprintf( f, "      \"name\": \"%s\",\n", c->name );
    fprintf( f, "      \"protocol_id\": \"%016" PRIx64 "\",\n", (uint64_t) INTEROP_PROTOCOL_ID );
    fprintf( f, "      \"create_timestamp\": \"%" PRIu64 "\",\n", c->create_timestamp );
    fprintf( f, "      \"expire_timestamp\": \"%" PRIu64 "\",\n", c->expire_timestamp );
    fprintf( f, "      \"timeout_seconds\": %d,\n", c->timeout_seconds );
    fprintf( f, "      \"client_id\": \"%016" PRIx64 "\",\n", c->client_id );
    fprintf( f, "      \"public_addresses\": " ); print_address_list( f, token.server_addresses, c->num_addresses ); fprintf( f, ",\n" );
    fprintf( f, "      \"internal_addresses\": " ); print_address_list( f, priv.server_addresses, c->num_addresses ); fprintf( f, ",\n" );
    fprintf( f, "      \"private_key\": \"" ); print_hex( f, private_key, NETCODE_KEY_BYTES ); fprintf( f, "\",\n" );
    fprintf( f, "      \"nonce\": \"" ); print_hex( f, nonce, NETCODE_CONNECT_TOKEN_NONCE_BYTES ); fprintf( f, "\",\n" );
    fprintf( f, "      \"client_to_server_key\": \"" ); print_hex( f, priv.client_to_server_key, NETCODE_KEY_BYTES ); fprintf( f, "\",\n" );
    fprintf( f, "      \"server_to_client_key\": \"" ); print_hex( f, priv.server_to_client_key, NETCODE_KEY_BYTES ); fprintf( f, "\",\n" );
    fprintf( f, "      \"user_data\": \"" ); print_hex( f, priv.user_data, NETCODE_USER_DATA_BYTES ); fprintf( f, "\",\n" );
    fprintf( f, "      \"token\": \"" ); print_hex( f, buffer, NETCODE_CONNECT_TOKEN_BYTES ); fprintf( f, "\"\n" );
    fprintf( f, "    }" );
    return 1;
}

static int cmd_gen_fixed( const char * out_path )
{
    FILE * f = fopen( out_path, "wb" );
    if ( !f )
        return 1;
    fprintf( f, "{\n  \"generator\": \"netcode %s internal writer + XChaCha20-Poly1305 (netcode_interop.c gen-fixed)\",\n", NETCODE_VERSION_FULL );
    fprintf( f, "  \"vectors\": [\n" );
    int i;
    int n = (int) ( sizeof( fixed_cases ) / sizeof( fixed_cases[0] ) );
    for ( i = 0; i < n; i++ )
    {
        if ( !write_fixed_case( f, &fixed_cases[i], i ) )
        {
            fclose( f );
            return 1;
        }
        fprintf( f, "%s\n", i + 1 < n ? "," : "" );
    }
    fprintf( f, "  ]\n}\n" );
    fclose( f );
    return 0;
}

static int cmd_gen_api( const char * out_path )
{
    uint8_t private_key[NETCODE_KEY_BYTES];
    fill_pattern( private_key, NETCODE_KEY_BYTES, 5, 0x11 );
    uint8_t user_data[NETCODE_USER_DATA_BYTES];
    fill_pattern( user_data, NETCODE_USER_DATA_BYTES, 11, 0x21 );
    NETCODE_CONST char * public_addresses[2] = { "203.0.113.7:7777", "[2001:db8::7]:7778" };
    NETCODE_CONST char * internal_addresses[2] = { "10.1.2.3:7777", "[fd00::3]:7778" };
    const uint64_t client_id = 0x1122334455667788ULL;

    uint8_t buffer[NETCODE_CONNECT_TOKEN_BYTES];
    if ( netcode_generate_connect_token( 2, public_addresses, internal_addresses, 45, 10, client_id,
                                         INTEROP_PROTOCOL_ID, private_key, user_data, buffer ) != NETCODE_OK )
        return 1;

    FILE * f = fopen( out_path, "wb" );
    if ( !f )
        return 1;
    fprintf( f, "{\n  \"generator\": \"netcode %s netcode_generate_connect_token (netcode_interop.c gen-api)\",\n", NETCODE_VERSION_FULL );
    fprintf( f, "  \"protocol_id\": \"%016" PRIx64 "\",\n", (uint64_t) INTEROP_PROTOCOL_ID );
    fprintf( f, "  \"client_id\": \"%016" PRIx64 "\",\n", client_id );
    fprintf( f, "  \"timeout_seconds\": 10,\n" );
    fprintf( f, "  \"expire_seconds\": 45,\n" );
    fprintf( f, "  \"public_addresses\": [\"%s\", \"%s\"],\n", public_addresses[0], public_addresses[1] );
    fprintf( f, "  \"internal_addresses\": [\"%s\", \"%s\"],\n", internal_addresses[0], internal_addresses[1] );
    fprintf( f, "  \"private_key\": \"" ); print_hex( f, private_key, NETCODE_KEY_BYTES ); fprintf( f, "\",\n" );
    fprintf( f, "  \"user_data\": \"" ); print_hex( f, user_data, NETCODE_USER_DATA_BYTES ); fprintf( f, "\",\n" );
    fprintf( f, "  \"token\": \"" ); print_hex( f, buffer, NETCODE_CONNECT_TOKEN_BYTES ); fprintf( f, "\"\n}\n" );
    fclose( f );
    return 0;
}

static int cmd_verify( const char * token_path, const char * key_path )
{
    uint8_t buffer[NETCODE_CONNECT_TOKEN_BYTES];
    uint8_t key[NETCODE_KEY_BYTES];
    if ( !read_file( token_path, buffer, NETCODE_CONNECT_TOKEN_BYTES ) || !read_key_file( key_path, key ) )
    {
        fprintf( stderr, "verify: cannot read inputs\n" );
        return 2;
    }

    struct netcode_connect_token_t token;
    if ( netcode_read_connect_token( buffer, NETCODE_CONNECT_TOKEN_BYTES, &token ) != NETCODE_OK )
    {
        fprintf( stderr, "verify: netcode_read_connect_token failed\n" );
        return 3;
    }

    uint8_t private_data[NETCODE_CONNECT_TOKEN_PRIVATE_BYTES];
    memcpy( private_data, token.private_data, sizeof( private_data ) );
    if ( netcode_decrypt_connect_token_private( private_data, NETCODE_CONNECT_TOKEN_PRIVATE_BYTES, token.version_info,
                                                token.protocol_id, token.expire_timestamp, token.nonce, key ) != NETCODE_OK )
    {
        fprintf( stderr, "verify: netcode_decrypt_connect_token_private failed\n" );
        return 4;
    }

    struct netcode_connect_token_private_t priv;
    if ( netcode_read_connect_token_private( private_data, NETCODE_CONNECT_TOKEN_PRIVATE_BYTES, &priv ) != NETCODE_OK )
    {
        fprintf( stderr, "verify: netcode_read_connect_token_private failed\n" );
        return 5;
    }

    if ( memcmp( priv.client_to_server_key, token.client_to_server_key, NETCODE_KEY_BYTES ) != 0 ||
         memcmp( priv.server_to_client_key, token.server_to_client_key, NETCODE_KEY_BYTES ) != 0 )
    {
        fprintf( stderr, "verify: public and private session keys differ\n" );
        return 6;
    }

    printf( "{\"protocol_id\":\"%016" PRIx64 "\",\"create_timestamp\":\"%" PRIu64 "\",\"expire_timestamp\":\"%" PRIu64 "\",",
            token.protocol_id, token.create_timestamp, token.expire_timestamp );
    printf( "\"timeout_seconds\":%d,\"client_id\":\"%016" PRIx64 "\",", priv.timeout_seconds, priv.client_id );
    printf( "\"public_addresses\":" ); print_address_list( stdout, token.server_addresses, token.num_server_addresses );
    printf( ",\"internal_addresses\":" ); print_address_list( stdout, priv.server_addresses, priv.num_server_addresses );
    printf( ",\"client_to_server_key\":\"" ); print_hex( stdout, priv.client_to_server_key, NETCODE_KEY_BYTES );
    printf( "\",\"server_to_client_key\":\"" ); print_hex( stdout, priv.server_to_client_key, NETCODE_KEY_BYTES );
    printf( "\",\"user_data\":\"" ); print_hex( stdout, priv.user_data, NETCODE_USER_DATA_BYTES );
    printf( "\"}\n" );
    return 0;
}

static int cmd_connect( const char * token_path, const char * key_path, const char * server_address )
{
    uint8_t connect_token[NETCODE_CONNECT_TOKEN_BYTES];
    uint8_t key[NETCODE_KEY_BYTES];
    if ( !read_file( token_path, connect_token, NETCODE_CONNECT_TOKEN_BYTES ) || !read_key_file( key_path, key ) )
    {
        fprintf( stderr, "connect: cannot read inputs\n" );
        return 2;
    }

    struct netcode_connect_token_t token;
    if ( netcode_read_connect_token( connect_token, NETCODE_CONNECT_TOKEN_BYTES, &token ) != NETCODE_OK )
    {
        fprintf( stderr, "connect: netcode_read_connect_token failed\n" );
        return 3;
    }

    if ( netcode_init() != NETCODE_OK )
        return 4;
    netcode_log_level( NETCODE_LOG_LEVEL_ERROR );

    /* A lossless, zero-latency simulator keeps the handshake deterministic and needs no sockets. */
    struct netcode_network_simulator_t * sim = netcode_network_simulator_create( NULL, NULL, NULL );

    double t = 0.0;
    struct netcode_server_config_t server_config;
    netcode_default_server_config( &server_config );
    server_config.protocol_id = token.protocol_id;
    server_config.network_simulator = sim;
    server_config.max_connect_token_lifetime = (int) ( token.expire_timestamp - token.create_timestamp );
    memcpy( server_config.private_key, key, NETCODE_KEY_BYTES );
    struct netcode_server_t * server = netcode_server_create( server_address, &server_config, t );
    if ( !server )
    {
        fprintf( stderr, "connect: netcode_server_create failed\n" );
        return 5;
    }
    netcode_server_start( server, 4 );

    /* Model a gateway that has been up for a minute: netcode refuses tokens whose lifetime began
       before the server started (their keys may already have been used), and a token minted a
       second before this freshly created server would otherwise be refused depending on where the
       wall-clock second boundary falls. Real gateways run long before the tokens they accept. */
    server->min_connect_token_expire_timestamp -= 60;

    struct netcode_client_config_t client_config;
    netcode_default_client_config( &client_config );
    client_config.network_simulator = sim;
    struct netcode_client_t * client = netcode_client_create( "[::]:50000", &client_config, t );
    if ( !client )
        return 6;

    netcode_client_connect( client, connect_token );

    int i;
    for ( i = 0; i < 200; i++ )
    {
        netcode_network_simulator_update( sim, t );
        netcode_client_update( client, t );
        netcode_server_update( server, t );
        if ( netcode_client_state( client ) <= NETCODE_CLIENT_STATE_DISCONNECTED ||
             netcode_client_state( client ) == NETCODE_CLIENT_STATE_CONNECTED )
            break;
        t += 0.1;
    }

    int state = netcode_client_state( client );
    int connected = state == NETCODE_CLIENT_STATE_CONNECTED && netcode_server_client_connected( server, 0 );
    printf( "{\"connected\":%s,\"client_state\":%d", connected ? "true" : "false", state );
    if ( connected )
    {
        printf( ",\"client_id\":\"%016" PRIx64 "\",\"user_data\":\"", netcode_server_client_id( server, 0 ) );
        print_hex( stdout, (const uint8_t *) netcode_server_client_user_data( server, 0 ), NETCODE_USER_DATA_BYTES );
        printf( "\"" );
    }
    printf( "}\n" );

    netcode_client_destroy( client );
    netcode_server_destroy( server );
    netcode_network_simulator_destroy( sim );
    netcode_term();
    return connected ? 0 : 7;
}

int main( int argc, char ** argv )
{
    if ( argc == 3 && strcmp( argv[1], "gen-fixed" ) == 0 )
        return cmd_gen_fixed( argv[2] );
    if ( argc == 3 && strcmp( argv[1], "gen-api" ) == 0 )
        return cmd_gen_api( argv[2] );
    if ( argc == 4 && strcmp( argv[1], "verify" ) == 0 )
        return cmd_verify( argv[2], argv[3] );
    if ( argc == 5 && strcmp( argv[1], "connect" ) == 0 )
        return cmd_connect( argv[2], argv[3], argv[4] );
    fprintf( stderr, "usage: netcode_interop gen-fixed <out.json> | gen-api <out.json> | "
                     "verify <token.bin> <key.hex> | connect <token.bin> <key.hex> <server-address>\n" );
    return 1;
}
