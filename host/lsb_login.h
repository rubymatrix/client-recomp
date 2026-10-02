/* Signing in to a LandSandBoat server (lsb_login.c). */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct LsbLogin
{
    uint32_t server;                         /* IPv4, host byte order */
    uint16_t auth_port, data_port, view_port; /* 54231, 54230, 54001 */
    const char* user;
    const char* password;
    const char* otp; /* "" when the account has none */
    /* A single-use launch token from a server's own launcher (e.g. minted after a Discord
     * login), sent in place of the password and OTP; NULL for none. */
    const char* login_token;
    /* The loader version sent at sign-in, "major.minor.patch"; NULL or "" for LSB_LOADER_VERSION.
     * xi_connect refuses versions it does not expect, and servers pin their own. */
    const char* version;
    /* When not NULL (24 bytes): the version signed in with when the server refused the one sent
     * and named another, "" otherwise - to send it from the start next time. */
    char* version_used;
} LsbLogin;

#define LSB_LOADER_VERSION "2.1.2"

/* A loader version as major.minor.patch (each 0..65535); 0 when it is not one. */
int lsb_parse_version(const char* s, int out[3]);

/* Signs in on the auth port (TLS, xi_connect's JSON), opens the login data connection and
 * answers it for the rest of the run, and arranges what FFXI's lobby traffic needs: the session
 * hash in every lobby command (ws2), the gamecore session. Returns 1, or 0 with a message. */
int lsb_login(const LsbLogin* l, char* err, size_t errn);

/* A server name or dotted quad as an IPv4 address (host byte order); 0 if it does not resolve. */
int net_resolve_ipv4(const char* name, uint32_t* out);
/* Reads a line from the terminal without echoing it (the password prompt); 0 when there is no
 * terminal. */
int read_secret(const char* prompt, char* out, size_t n);
