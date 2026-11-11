# Archipelago bridge

A game-agnostic channel between a running ScummVM engine and an external
randomizer client (typically an [Archipelago](https://archipelago.gg) client).
ScummVM knows nothing about any game here: engines report low-level VM events,
the client decides what they mean. Everything game-specific (which events are
checks, how an item is granted) lives in the client's game definition.

## Enabling

In the launcher, select the game, click **Game Options…** and open the
**Archipelago** tab (shown for SCI and SCUMM games). Tick **Connect to an
Archipelago client**; the client address defaults to `127.0.0.1:38282`.
This is stored per game as `ap_bridge_enabled` / `ap_bridge_address`.

Alternatives for scripts and testing: a raw `ap_bridge=host:port` key, or the
`SCUMMVM_AP_BRIDGE=host:port` environment variable. Settings are read each
time a game starts. A client that starts ScummVM itself also sets
`SCUMMVM_AP_TARGET` to the game's target, so the environment address only
applies to that game and not to one switched to with Ctrl+F7.

The client listens on that port; ScummVM connects out (libcurl socket) when a
game starts and reconnects every 2 s if the client goes away. Builds without
libcurl warn and run normally.

## Wire format

Newline-delimited JSON, one object per line, both directions.

### Engine → client

| `type`   | Fields | Meaning |
|----------|--------|---------|
| `hello`  | `protocol`, `engine`, `gameid`, `extra`, `target` | Sent on connect and when a game starts. The client should (re)send its rules. |
| `match`  | `rule`, `kind`, `keys`, `args`, `ctx`, `action` | An event matched a `watch` rule. Queued while disconnected. |
| `trace`  | `kind`, `keys`, `args`, `ctx`, `action` | Every event of a traced kind (for mapping a new game). |
| `result` | `cmd`, `req`, `ok`, `value`, `error` | Reply to each command. |
| `travel` | `id`, `ctx` | The player chose a destination in the travel menu. |
| `bye`    | | The game is shutting down. |

`action` is true when the event was caused by the client's own `do` command.

### Client → engine

| `cmd`     | Fields | Meaning |
|-----------|--------|---------|
| `watch`   | `rule`, `kind`, `keys`, `args`, `ctx`, `veto`, `value`, `once` | Add or replace a rule. `keys` must match position by position, `args` as a prefix; `"*"` is a wildcard in both. `ctx` values must all match. `veto: true` makes the engine skip the event; `value` is the result a vetoed call returns. |
| `unwatch` | `rule` | Remove a rule. |
| `clear`   | | Remove all rules. |
| `trace`   | `kind`, `on` | Stream every event of a kind. Cleared on disconnect. |
| `do`      | `req`, `op`, `keys`, `args`, `asGame` | Run an engine action (below) at the next safe point. With `asGame: true` the action counts as the player's own (rules can veto what it causes), for test harnesses. |
| `say`     | `text` | Show an on-screen message. |
| `help`    | `text` | Set the Archipelago Help page (Markdown). Send it again whenever it changes. |
| `travel`  | `title`, `options` | Set the travel menu: `options` is a list of `{id, text, enabled}` (`enabled` defaults to true). Send it again whenever it changes; an empty list turns travel off. |
| `ping`    | `req` | Liveness check. |

Rules survive client disconnects, so suppressed pickups stay suppressed; they
are cleared when a new game starts. Events raised by a `do` action are reported
but never vetoed, so "give item" actions are not swallowed by "suppress
pickup" rules.

### Archipelago Help page

A game running with the bridge gets an **Archipelago Help** button in the
global main menu (Ctrl+F5), in the slot of the game's own help button; the
game's help, when it has one, is a button on that page. The global keymap
action "Archipelago help" (Ctrl+F6 by default, rebindable under Options,
Keymaps, Global) opens the page too. The page shows the text of the last
`help` message, rendered as Markdown, or a "waiting for the client" note.
ScummVM's Markdown renderer drops the first letter of a plain paragraph that
follows a list, so start such a block with a heading or a list item.

### Travel menu

The global keymap action "Archipelago travel" (Ctrl+F8 by default) opens a
list of the destinations from the last `travel` message. Entries sent with
`enabled: false` are shown greyed out as "(locked)" and cannot be chosen.
The menu only opens while the player is in control (SCUMM: no message on
screen, no cutscene, user input on and, up to v6, the action verbs showing,
so not in a conversation, close-up or map screen; SCI: no game window open); otherwise, or
when the client is not connected or has sent no destinations, a short
on-screen message explains why. A choice is reported as a `travel` message
with the option's `id`; ScummVM does nothing else. The client performs the
trip itself (state changes, then `goto` or the engine's equivalent), and can
refuse with `say` when a game-specific condition forbids it.

```json
{"cmd":"travel","title":"Travel","options":[{"id":1,"text":"Mêlée Island"},{"id":3,"text":"Monkey Island","enabled":false}]}
{"type":"travel","id":1,"ctx":{"room":19,"ego":1,"mx":189,"my":118}}
```

### Game switching

The global keymap action "Archipelago game switch" (Ctrl+F7 by default) lists
every other game whose Archipelago tab has **Include in game switching** ticked
(`ap_switch` in its domain), whatever its engine, and whether or not it uses the
bridge. Picking one saves the current game to its engine's last save slot
(`getMaximumSaveSlot()`, 99 for SCUMM and SCI; the autosave slot belongs to the
engine) as "Archipelago game switch", records that slot as `ap_switch_slot` in
the game's domain, and returns to the launcher with the other game chained (as
Day of the Tentacle chains Maniac Mansion). That game starts from its own
`ap_switch_slot` if it has one, or from the beginning. Switching needs the game
to be saveable at that moment; otherwise an on-screen message says so.

Each randomized game connects to its own client, so give them different client
addresses (and ports in the clients). The clients stay connected to the
multiworld while their game is switched out, keep its items, and hand them over
when the game is back and its save is loaded.

## SCI (Sierra SCI0–SCI1.1)

Events:

| kind      | keys | args |
|-----------|------|------|
| `call`    | `[script, export, by, method]` | procedure arguments (vetoable: the procedure is skipped and returns `value`) |
| `send`    | `[object, selector, class, by, method]` | method arguments (vetoable) |
| `setprop` | `[object, selector, class, by, method]` | `[new value]` (vetoable: the write is skipped) |
| `getprop` | `[object, selector, class, by, method]` | none (vetoable: `value` replaces what the script reads) |
| `getglobal` | `[index, by, method]` | `[value]` (vetoable: `value` replaces what the script reads) |
| `kernel`  | `[name]` | kernel call arguments |

`by` and `method` name the method whose code caused the event (its object and
selector; empty when unknown, e.g. for a procedure run outside any method). As
keys only need to match as a prefix, rules may leave them out; with them, a
rule answers one script's question only, e.g. "does ego have the hair?" as
asked by the ribbon's `doVerb`:

```json
{"cmd":"watch","rule":7,"kind":"send","keys":["ego","has","*","ribbon","doVerb"],"args":["4"],"veto":true,"value":0}
```

`getglobal` reports script reads of global variables (`lag`/`lsg` and their
indexed forms), but only for globals some rule names by index (a trace of
`getglobal` reports them all), so a story stage can be answered for one script
while the real value stays.

`getprop` only sees reads made by message (`(obj prop:)`). An object's own
methods read their properties with direct VM opcodes, which are not reported.
That lets a client change what *other* scripts see, e.g. keep a room showing
an item whose inventory object has already been handed to the player, while
the inventory itself (`has:`, the inventory window) still sees the real value.

Context: `room` (global 11).

Actions: `getGlobal [n]`, `setGlobal [n, v]`, `getBit [base, bit]`,
`setBit [base, bit, 0|1]` (Sierra bit arrays: word `base + bit/16`, mask
`0x8000 >> bit%16`), `getProp`/`setProp keys [obj, selector]`,
`send keys [obj, selector] args [...]`, `room`, `talk keys [text, title]` (a Sierra-style message window, titled when a title is given, that waits for a click, a key or a timeout like the games' own Print; 0 while the game has a window of its own open; 16-bit SCI only). Objects are named directly,
as `name@script` when a name is ambiguous, or as `global:N`.

The tick runs inside `kGetEvent`, so `send` actions execute from kernel
context like any other kernel-invoked selector.

## SCUMM (all versions, v0–v8)

Events:

| kind     | keys | args |
|----------|------|------|
| `owner`  | `[object]` | `[newOwner, oldOwner, toEgo]` (vetoable). Every pickup in every SCUMM version goes through `putOwner`. A script's `setOwnerOf` asks before it changes anything, so vetoing a removal also keeps the inventory entry. |
| `var`    | `[index]` | `[new, old]` (vetoable) |
| `bitvar` | `[index]` | `[new, old]` (vetoable) |
| `room`   | `[room]` | |
| `script` | `[number]` | (vetoable) |
| `getowner` | `[object, by, number]` | `[owner]` (vetoable; v5). A script asked who owns an object. `by` is `object` (number = the object whose verb script is running), `global`, `local`, `entry` or `exit` (number = room). |
| `getclass` | `[object, class, by, number]` | `[has]` (vetoable; v5). A script tested whether an object has a class (bit 7 stripped); `by` and `number` as for `getowner`. |
| `getbit` | `[bit, by, number]` | `[value]` (vetoable; v5 and later). A script read a bit variable; `by` and `number` as for `getowner`. Reads outside scripts (bridge actions) are not reported. |
| `getvar` | `[variable, by, number]` | `[value]` (vetoable). A script read a variable; `by` and `number` as for `getowner`. Reads are frequent, so only variables a rule names as its first key (or a trace) are reported. |
| `setclass` | `[object, class, by, number]` | `[set]` (vetoable; v5). A script set (1) or cleared (0) an object's class (bit 7 stripped); `by` and `number` as for `getowner`. A veto leaves the class as it was. |

Context: `room`, `ego`, `mx`/`my` (the mouse in room coordinates, so a client can
tell where a click landed), `screen` (the part of the screen the mouse is over: 0 the
room, 2 the verbs and inventory; an object picked up keeps its number in the inventory,
so this tells a click on the room's object from one on its inventory copy), `under` (the
room object under the mouse, 0 for none or when the mouse is off the room).

A `getowner`, `getclass`, `getbit` or `getvar` veto replaces the answer the script gets with the rule's `value`.

Spots: in a randomizer the item the player receives and the place it is found
are one game object, with one owner. When the player uses the item up (its
owner becomes neither ego nor a room), the engine would treat the object as
gone everywhere, and the place could no longer be picked up. The `spots`
action lists objects whose place is still to be found: while listed, such an
object is still present in its home room (scripts reading its owner still get
the real value). The client sends the list again whenever it changes.
The real owner does not change, so the inventory, other scripts and other
rooms still see the truth. This keeps a location checkable after the client
has already handed over its item: for example, answer `15` (the room) when the
pile of paper in Monkey Island 2 (object 67) asks whether Guybrush owns the
paper (object 68), and the pile hands it out again; the pickup is then vetoed
and reported.

```json
{"cmd":"watch","rule":10,"kind":"getowner","keys":["68","object","67"],"ctx":{"room":8},"veto":true,"value":15}
{"cmd":"watch","rule":11,"kind":"owner","keys":["68"],"args":["*","*","1"],"ctx":{"room":8},"veto":true}
```

A pickup (`pickupObject`) copies the object into the inventory before it sets
the owner. When the owner change is vetoed, the bridge also removes that new
inventory entry, so vetoed pickups never duplicate an item or use up
inventory space (v5 games hold at most 80 entries).

Actions: `getVar`, `setVar`, `getBit`, `setBit`, `getOwner`,
`setOwner [obj, owner|-1]` (clears the inventory entry first when moving an
object away from ego), `give [obj, homeRoom]` (copies the object out of its
home room into the inventory unless it is already there), `take [obj]`,
`runScript [n, args...]`, `stopScript [n...]` (stops those scripts, as a script's `stopScript` does), `startObject [obj, verb, args...]` (runs one of an
object's verb scripts), `goto [room, obj, x, y]` (moves ego to another room as `loadRoomWithEgo` does:
placed at object `obj`'s walk-to spot and walking to `x, y`; with `obj` 0 the room
loads as `loadRoom` does and ego is put at `x, y`), `room`, `stopSounds` (all music and effects), `stopSound [id...]` (those sounds), `soundRunning [id]` (1 while that sound plays or is queued), `talk [text] [actor, inTalk]` (the actor, default ego, says the text over their head in the game's font, talk color and animation; 0 means busy: someone is talking, a cutscene runs or the player has no control; with `inTalk` = 1 it also speaks while the player has no control, as when a conversation's choices wait), `actorColor [actor, color, r, g, b, costume]` (in the actor's costumes, 0 = ego, or only in `costume` when given, the parts drawn in palette color `color` use the room palette's closest match to r, g, b, darkened or tinted as the scene has darkened the color it replaces, so a recolored coat follows the game's lighting effects; r = -1 removes it), `clearColors`, `spots [object...]` (replace the list of spots, see above), `setClass [object, class...]` (as a script's `setClass`: a class with bit 0x80 is set, without it cleared; actors below the actor count), `getClass [object, class]` (1 when the object has the class), `setName [object]` with the new name in `keys[0]` (as a script's `setObjectName`; the inventory redraws), `invSlots` (inventory
entries in use).
The tick runs at the top of `scummLoop`.

## Example: King's Quest VI

```json
{"cmd":"watch","rule":1,"kind":"send","keys":["*","givePoints"]}
{"cmd":"watch","rule":2,"kind":"send","keys":["ego","get"],"veto":true}
{"cmd":"do","req":7,"op":"send","keys":["ego","get"],"args":[45]}
{"cmd":"do","req":8,"op":"setBit","args":[137,3,1]}
```

Rule 1 reports every point award (with `ctx.room`) as a location check, and
rule 2 suppresses the game's own item pickups. The first `do` gives the
spellbook (item 45), which passes rule 2 because it is the client's own action.
The second sets flag 3, which reveals the Isle of the Mists on the map.

## Adding an engine

Implement `APBridge::Adapter` (`engines/apbridge.h`): `engineId`,
`getContext`, `performAction`. Then call `Bridge::notify` from the VM's
natural choke points, `Bridge::tick` once per frame from a point where
running game code is safe, and `attach`/`detach` around the game's lifetime.
