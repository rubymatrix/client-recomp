# Direct login (dev)

`XIV_DIRECT_LOGIN=<character id>` starts the game straight into the world as that character
(`characters.id` in Meteor's database): no title menu, lobby list or character select. Unset, the
client behaves as retail.

```sh
./xivctl launch --direct 1                     # from the workspace root
XIV_DIRECT_LOGIN=1 tools/xivplay.sh            # or any launch that passes the environment
```

It uses the client's own developer ticket login (`runtime/portable/xiv_direct_login.c`): the
ticket is the character id, the lobby answers request 0x1F5 with the World address, and the
client switches to the game screen as soon as the World connection is open.

| variable | default | |
|---|---|---|
| `XIV_DIRECT_LOGIN` | unset | character id; turns it on |
| `XIV_LOBBY_HOST` / `XIV_LOBBY_PORT` | `127.0.0.1` / `54994` | lobby for the ticket request. Port `0` skips the lobby and connects to World directly (no server support needed, about 6 s faster) |
| `XIV_WORLD_NAME` | `Hyperion` | world name sent in the request (the client prefixes `@`) |
| `XIV_FRONT_HOST` / `XIV_FRONT_PORT` | `127.0.0.1` / `54992` | World, used when the lobby is skipped or does not answer (after 3 tries, 2 s apart) |

Progress is logged with a `[direct-login <seconds>]` prefix. Client-side details:
`decomp/docs/lobby_protocol.md`, "Patching the ticket login in recomp".
