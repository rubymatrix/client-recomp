/* WS2_32 on host sockets (ws2.c). */
#pragma once

#include <stdint.h>

/* Starts host networking and registers the shims (before the images are mapped). */
void ws2_init(void);
/* Where the game's hosts are: gethostbyname answers the game's domain and every name under it with
 * this IPv4 address (host byte order) instead of asking DNS, whose answer is Square Enix's. */
void ws2_set_game_server(uint32_t ipv4_host_order);
/* A LandSandBoat sign-in (host/lsb_login.c): every lobby command FFXiMain sends to the login
 * server's data or view port carries this session hash at +12, where the server looks for it. */
void ws2_set_lobby_session(const uint8_t hash[16], uint16_t data_port, uint16_t view_port);
