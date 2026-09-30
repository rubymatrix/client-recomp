# Local LuaJIT patches

Upstream commit: see PINNED.txt. Every change is marked in the source with a comment starting
`/* FFXIRecompile (Windower dialect): ...`.

Windower 4 runs addons on a modified Lua 5.1 that accepts syntax standard Lua rejects, and its
addons and libs rely on it. These patches let LuaJIT parse that dialect. Each one only gives
meaning to source that stock LuaJIT rejects with a syntax error, with the one exception noted
under 2. Every file in the Windower and Ashita v4 addon trees that stock LuaJIT can parse compiles
to identical bytecode with the patches applied.
Recheck with `python3 tools/lua_parse_corpus.py <dirs>`.

1. **Literals as prefix expressions** (`src/lj_parse.c`: `EXPR_F_PREFIX`, `expr_litsuffix`,
   `expr_simple`, `expr_primary_nav`). A number, string, `nil`, `true` or `false` literal, a
   table constructor or a function expression can be followed by `.`, `[` or `:` and then any
   suffix chain: `'%d':format(n)`, `"x":color(1)`, `0x01:char()`, `1:exp()`, `true:fn()`,
   `{a, b}[i]`, `{'p%i', 'a%i'}[k]:format(i)`. After a literal these tokens are never valid in
   standard Lua. Inside the middle of a `c ? a : b` ternary (a LuaJIT extension), `:` still ends
   the branch, as it does after a name.
2. **Calling a function expression directly** (`expr_litsuffix`, `call` argument):
   `function(x) ... end(x)` works only when the `(` is on the same line as the `end`. A `(` on
   the next line starts a new statement, as in standard Lua. Exception: standard Lua reads
   `local f = function() end (g)()` on one line as two statements, but the patched parser reads
   it as a call. No known code writes this.
3. **Unary plus** (`src/lj_parse.c`, `expr_unop`): `+x` (for example `UTC = +00.00`). It has
   the priority of the other unary operators (`+x^2` is `+(x^2)`) and yields its operand
   unchanged, truncated to one value, the same as `(x)`. It does not convert the operand to a
   number and invokes no metamethod.
4. **Unknown escapes** (`src/lj_lex.c`, `lex_string`): a backslash followed by a character that
   is not a known escape stands for that character (`'\.'` is `'.'`, `"\%"` is `"%"`), as in
   Lua 5.1. Valid escapes are unchanged: `\a \b \f \n \r \t \v \\ \" \'`, a backslash before a
   newline, `\ddd`, `\xXX`, `\u{...}` and `\z`. Malformed `\x`, `\u` and `\ddd` > 255 are still
   errors.

A lone `;` as an empty statement needed no patch: LuaJIT 2.1 already accepts it.

Not supported: the Ashita v4 LuaLS stub files `addons/libs/annotations/SDK/IGuiManager.lua` and
`IGuiManagerTypes.lua` (`---@meta`) use keywords as names (`repeat`, `end`). No Lua can parse
them, and they are never loaded. They exist only for editor type hints.

## ffi C parser: a single `L` suffix is 32 bits (lj_strscan.c)

Ashita's libs (win32types.lua) declare constants like `0x8000FFFFL` in `ffi.cdef`. They were written
for Windows, where `long` is 32 bits; on a 64-bit POSIX host LuaJIT made `L` a 64-bit integer, which
the C parser then rejected as malformed. A single `L` now means a 32-bit long, as on Windows; `LL`
is unchanged.
