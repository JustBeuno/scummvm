/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef SCUMM_APBRIDGE_SCUMM_H
#define SCUMM_APBRIDGE_SCUMM_H

#include "common/array.h"
#include "common/hashmap.h"
#include "engines/apbridge.h"

namespace Scumm {

class ScummEngine;
class Actor;

/**
 * SCUMM side of the Archipelago bridge.
 *
 * Events:
 *   owner   keys [object]  args [newOwner, oldOwner, toEgo(0|1)]   (vetoable)
 *           Every item pickup in every SCUMM version goes through here.
 *   var     keys [index]   args [newValue, oldValue]               (vetoable)
 *   bitvar  keys [index]   args [newValue, oldValue]               (vetoable)
 *   room    keys [room]    args []
 *   script  keys [number]  args []                                 (vetoable)
 *   getowner keys [object, by, number]  args [owner]               (vetoable)
 *           a script asked who owns an object (getObjectOwner). "by" names the
 *           asking script: object (number = object id), global, local, entry
 *           or exit (number = room). A veto replaces the answer with the
 *           rule's value; the real owner is unchanged, so the inventory and
 *           every other script still see the truth.
 *   getclass keys [object, class, by, number]  args [has]     (vetoable)
 *   getbit  keys [index, by, number]  args [value]            (vetoable)
 *           a script read a bit variable (v5 and later; reads outside scripts
 *           are not reported).
 *   getvar  keys [index, by, number]  args [value]            (vetoable)
 *           a script read a variable; only reported for variables a rule names
 *           (or a trace), as reads are frequent.
 *   setclass keys [object, class, by, number]  args [set]     (vetoable)
 *           a script sets or clears an object's class (v5); a veto leaves it as it was.
 *   loadroom keys [room, by, number]  args [object, fromRoom]  (vetoable)
 *           a script is about to change rooms (loadRoom, or loadRoomWithEgo with
 *           the entrance object; 0 for loadRoom). A veto skips the opcode: the
 *           room stays loaded, ego stays where he is, and the script goes on with
 *           its next instruction. v3 to v5.
 *   sentence keys [verb, objectA, objectB]  args []            (vetoable)
 *           a sentence (the player's click, or a script's doSentence) is about
 *           to reach the sentence script, also for objects with no verb script.
 *           A veto drops it.
 *   ctx: room, ego, ex/ey (where ego stands), mx/my (mouse in room coordinates)
 *
 * A pickup whose ownership change is vetoed also drops the inventory entry
 * the pickup had already created, so vetoed checks never fill the inventory.
 *
 * Actions (cmd "do"):
 *   getVar [n]   setVar [n, value]   getBit [n]   setBit [n, 0|1]
 *   getOwner [obj]   setOwner [obj, owner]   (owner -1 = ego)
 *   give [obj, homeRoom]   copies the object out of its home room into the
 *                          inventory and hands it to ego
 *   take [obj]             removes an object from the inventory
 *   runScript [n, args...]
 *   startObject [obj, verb, args...]   runs one of an object's verb scripts
 *   room
 *   goto [room, obj, x, y]   move ego to a room as loadRoomWithEgo does (obj 0: put at x, y)
 *   stopScript [n...]    stops those scripts, as a script's stopScript does
 *   stopSound [id...]    stops those sounds
 *   soundRunning [id]    1 while that sound plays or is queued
 *   stopSounds            stops all music and sound effects
 *   talk                  [text] [actor]: the actor (default ego) says text over their head
 *   actorColor [actor, color, r, g, b, costume]   in the actor's costumes (0 = ego; costume
 *                         0 or left out = every costume), the parts drawn in palette color
 *                         `color` use the room palette's closest match to r, g, b instead.
 *                         Scene lighting (RoomIntensity and the like) scales r, g, b as it scales `color`.
 *                         r = -1 removes it.
 *                         Three more values [r2, g2, b2] give a pattern's second color.
 *   actorPattern [actor, kind, size]   draws actorColor's second colors in a pattern over the
 *                         actor (0 = ego): 1 horizontal stripes, 2 vertical, 3 diagonal,
 *                         4 checks, 5 polka dots, 6 rainbow, 7 disco (moving rainbow),
 *                         `size` pixels wide; kind 0 removes it. For 6 and 7 the second
 *                         color's red value sets each color's brightness (255 = full).
 *   clearColors           removes every actorColor and actorPattern
 *   invSlots              inventory entries in use (SCUMM caps them, 80 in v5)
 */
class ScummBridgeAdapter : public APBridge::Adapter {
public:
	ScummBridgeAdapter(ScummEngine *vm) : _vm(vm) {}

	const char *engineId() const override { return "scumm"; }
	void getContext(Common::HashMap<Common::String, int32> &ctx) override;
	bool performAction(const Common::String &op, const APBridge::Keys &keys, const APBridge::Args &args,
	                   int32 &result, Common::String &error) override;
	bool canInterrupt() override;

	void tick();

	// Hooks; return true to veto.
	bool onPutOwner(int obj, int owner);
	bool onGetOwner(int obj, int &owner);
	/** A script tests whether @p obj has class @p cls (bit 7 stripped); @p has may be answered. */
	bool onGetClass(int obj, int cls, int &has);
	/** A script reads bit variable @p bit; @p value may be answered. */
	bool onGetBit(int bit, int &value);
	bool onGetVar(int var, int &value);
	/** A script sets (@p set) or clears class @p cls of @p obj; true when vetoed. */
	bool onSetClass(int obj, int cls, bool set);
	bool onLoadRoom(int room, int obj);
	bool onSentence(int verb, int objA, int objB);
	/**
	 * Spots: objects the client lists as still to be found at their home spot.
	 * Using such an object up (its owner leaving ego for anything but a room)
	 * would otherwise remove its spot too, since the item and the spot are one
	 * object; the engine keeps the spot while the object is listed.
	 */
	bool isSpot(int obj) const { return _spots.contains(obj); }
	void pickupBegin() { _vetoedOwnerObj = 0; }
	void pickupEnd(int obj, int slot);
	bool inInventory(int obj) const;
	bool onWriteVar(unsigned int var, int value);
	void onRoom(int room);
	bool onRunScript(int script);
	/** The palette color a costume of @p actor draws in place of @p color. */
	int recolor(int actor, int color);
	/** The pattern's second color for @p color, or -1. */
	int recolorAlt(int actor, int color);
	/** The actor's pattern; false when it has none. */
	bool pattern(int actor, byte &kind, byte &size);
	/** Rainbow and disco: hue @p band, as bright as the color's second color's red value says. */
	int rainbow(int actor, int color, int band);

private:
	struct ColorOverride {
		int actor, index, costume;
		byte r, g, b;
		int r2, g2, b2;   ///< a pattern's second color, r2 < 0 for none
	};
	Common::Array<ColorOverride> _colors;
	void redrawActors();
	const ColorOverride *findColor(int actor, int color);
	/** Darken or tint r, g, b as the scene has the palette color @p color. */
	void applyLight(int color, int &r, int &g, int &b) const;
	/** The current palette's closest color to r, g, b outside the room's color cycles. */
	int matchColor(int r, int g, int b) const;
	uint32 _lightSig = 0;   ///< the recolored colors' current palette values, to notice scene lighting
	struct Pattern {
		int actor;
		byte kind, size;
	};
	Common::Array<Pattern> _patterns;

	ScummEngine *_vm;
	int _vetoedOwnerObj = 0;  ///< last object whose owner change was vetoed
	Common::HashMap<int, bool> _spots;
	/** Which script is running, as getowner/getclass report it: by = object/global/local/entry/exit. */
	void scriptContext(const char *&by, int &number) const;
};

} // End of namespace Scumm

#endif
