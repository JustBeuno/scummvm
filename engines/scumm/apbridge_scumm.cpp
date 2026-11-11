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

#include "scumm/apbridge_scumm.h"
#include "scumm/verbs.h"
#include "scumm/actor.h"
#include "scumm/sound.h"
#include "scumm/scumm.h"
#include "scumm/object.h"
#include "scumm/script.h"
#include "scumm/resource.h"

namespace Scumm {

// True when obj has an entry in the inventory array, whatever its owner field says.
bool ScummBridgeAdapter::inInventory(int obj) const {
	for (int i = 0; i < _vm->_numInventory; i++)
		if (_vm->_inventory[i] == obj)
			return true;
	return false;
}

static APBridge::Bridge *bridge() {
	return APBridge::Bridge::instance();
}

static Common::String num(int n) {
	return Common::String::format("%d", n);
}

bool ScummBridgeAdapter::canInterrupt() {
	if (_vm->_haveMsg || _vm->vm.cutSceneStackPointer != 0 || _vm->_userPut <= 0)
		return false;
	// The player must be at the normal verb interface: conversations, close-ups
	// and map screens replace the action verbs (low ids) with choices of their own.
	// Games without verbs on screen (v7 and later) are not checked.
	if (_vm->_game.version >= 7)
		return true;
	for (int i = 1; i < _vm->_numVerbs; i++) {
		const VerbSlot &vs = _vm->_verbs[i];
		if (vs.verbid >= 1 && vs.verbid <= 12 && vs.curmode == 1 && vs.saveid == 0)
			return true;
	}
	return false;
}

void ScummBridgeAdapter::getContext(Common::HashMap<Common::String, int32> &ctx) {
	ctx["room"] = _vm->_currentRoom;
	if (_vm->VAR_EGO != 0xFF)
		ctx["ego"] = _vm->VAR(_vm->VAR_EGO);
	// Where the mouse is, in room coordinates: lets a client tell where a click landed
	if (_vm->VAR_VIRT_MOUSE_X != 0xFF) {
		ctx["mx"] = _vm->VAR(_vm->VAR_VIRT_MOUSE_X);
		ctx["my"] = _vm->VAR(_vm->VAR_VIRT_MOUSE_Y);
	}
	// Which part of the screen the mouse is over: 0 the room, 2 the verbs and inventory (the
	// virtual screen numbers), so a client can tell a room object from its inventory copy
	if (VirtScreen *vs = _vm->findVirtScreen(_vm->_mouse.y)) {
		ctx["screen"] = vs->number;
		// The room object under the mouse (0 for none, or off the room): the object a click in
		// the room is about, even when ego also holds an object of that number
		ctx["under"] = (vs->number == kMainVirtScreen && _vm->VAR_VIRT_MOUSE_X != 0xFF)
			? _vm->findObject(_vm->VAR(_vm->VAR_VIRT_MOUSE_X), _vm->VAR(_vm->VAR_VIRT_MOUSE_Y)) : 0;
	}
}

void ScummBridgeAdapter::tick() {
	if (APBridge::Bridge *b = bridge())
		b->tick();
	// A disco pattern moves, so its actors are redrawn every frame
	int ego = (_vm->VAR_EGO != 0xFF) ? _vm->VAR(_vm->VAR_EGO) : 0;
	for (uint i = 0; i < _patterns.size(); i++) {
		if (_patterns[i].kind != 7)
			continue;
		int n = _patterns[i].actor ? _patterns[i].actor : ego;
		if (n > 0 && n < _vm->_numActors && _vm->_actors[n]->isInCurrentRoom())
			_vm->_actors[n]->_needRedraw = true;
	}
	// The game darkens or tints the palette for scenes (RoomIntensity and the like); recolored
	// costumes follow their original colors, so redraw them when those colors change
	uint32 sig = 0;
	for (uint i = 0; i < _colors.size(); i++) {
		int idx = CLIP(_colors[i].index, 0, 255);
		sig = sig * 31 + _vm->_currentPalette[idx * 3] * 65536 + _vm->_currentPalette[idx * 3 + 1] * 256 +
		      _vm->_currentPalette[idx * 3 + 2];
	}
	if (sig != _lightSig) {
		_lightSig = sig;
		redrawActors();
	}
}

int ScummBridgeAdapter::matchColor(int r, int g, int b) const {
	// The palette's closest color, as remapPaletteColor finds it, but never one the room cycles
	// (signs, water, lights): a costume drawn in one would flicker along with it
	if (_vm->_game.version < 5 || _vm->_game.heversion)
		return _vm->remapPaletteColor(r, g, b, -1);
	bool cycled[256] = {};
	for (int i = 0; i < 16; i++) {
		const ColorCycle &c = _vm->_colorCycle[i];
		if (c.delay && c.start <= c.end)
			for (int j = c.start; j <= c.end; j++)
				cycled[j] = true;
	}
	r = CLIP(r, 0, 255) & ~3;
	g = CLIP(g, 0, 255) & ~3;
	b = CLIP(b, 0, 255) & ~3;
	uint best = 0xFFFFFFFF;
	int bestItem = 1;
	for (int i = 1; i < 255; i++) {
		if (cycled[i])
			continue;
		const byte *pal = _vm->_currentPalette + i * 3;
		int dr = (pal[0] & ~3) - r, dg = (pal[1] & ~3) - g, db = (pal[2] & ~3) - b;
		uint sum = 3 * dr * dr + 6 * dg * dg + 2 * db * db;
		if (sum < best) {
			best = sum;
			bestItem = i;
			if (!sum)
				break;
		}
	}
	return bestItem;
}

void ScummBridgeAdapter::applyLight(int color, int &r, int &g, int &b) const {
	// Scale a replacement color the way the scene has scaled the color it replaces: the
	// current palette against the room's own palette
	if (color < 0 || color > 255 || _vm->_game.version < 5 || _vm->_game.heversion || !_vm->_roomResource)
		return;
	const byte *base = _vm->getPalettePtr(_vm->_curPalIndex, _vm->_roomResource);
	if (!base)
		return;
	base += color * 3;
	const byte *cur = _vm->_currentPalette + color * 3;
	int baseSum = base[0] + base[1] + base[2], curSum = cur[0] + cur[1] + cur[2];
	if (baseSum == curSum && base[0] == cur[0] && base[1] == cur[1])
		return;
	int *c[3] = { &r, &g, &b };
	for (int i = 0; i < 3; i++) {
		if (base[i] >= 16)
			*c[i] = *c[i] * cur[i] / base[i];
		else if (baseSum)
			*c[i] = *c[i] * curSum / baseSum;
		*c[i] = CLIP(*c[i], 0, 255);
	}
}

bool ScummBridgeAdapter::onPutOwner(int obj, int owner) {
	APBridge::Bridge *b = bridge();
	if (!b || !b->wants("owner"))
		return false;
	APBridge::Keys keys;
	keys.push_back(num(obj));
	APBridge::Args args;
	args.push_back(owner);
	args.push_back(_vm->_objectOwnerTable[obj]);
	args.push_back((_vm->VAR_EGO != 0xFF && owner == _vm->VAR(_vm->VAR_EGO)) ? 1 : 0);
	bool veto = b->notify("owner", keys, args).veto;
	if (veto)
		_vetoedOwnerObj = obj;
	return veto;
}

void ScummBridgeAdapter::scriptContext(const char *&by, int &number) const {
	by = "global";
	number = 0;
	if (_vm->_currentScript != 0xFF) {
		const ScriptSlot &ss = _vm->vm.slot[_vm->_currentScript];
		number = ss.number;
		if (ss.number == kScriptNumENCD) {
			by = "entry";
			number = _vm->_currentRoom;
		} else if (ss.number == kScriptNumEXCD) {
			by = "exit";
			number = _vm->_currentRoom;
		} else if (ss.where == WIO_GLOBAL) {
			by = "global";
		} else if (ss.where == WIO_LOCAL) {
			by = "local";
		} else {
			by = "object";
		}
	}
}

bool ScummBridgeAdapter::onGetOwner(int obj, int &owner) {
	APBridge::Bridge *b = bridge();
	if (!b || !b->wants("getowner"))
		return false;
	const char *by;
	int number;
	scriptContext(by, number);
	APBridge::Keys keys;
	keys.push_back(num(obj));
	keys.push_back(by);
	keys.push_back(num(number));
	APBridge::Args args;
	args.push_back(owner);
	APBridge::Verdict v = b->notify("getowner", keys, args);
	if (v.veto)
		owner = v.value;
	return v.veto;
}

bool ScummBridgeAdapter::onGetClass(int obj, int cls, int &has) {
	APBridge::Bridge *b = bridge();
	if (!b || !b->wants("getclass"))
		return false;
	const char *by;
	int number;
	scriptContext(by, number);
	APBridge::Keys keys;
	keys.push_back(num(obj));
	keys.push_back(num(cls));
	keys.push_back(by);
	keys.push_back(num(number));
	APBridge::Args args;
	args.push_back(has ? 1 : 0);
	APBridge::Verdict v = b->notify("getclass", keys, args);
	if (v.veto)
		has = v.value;
	return v.veto;
}

bool ScummBridgeAdapter::onSetClass(int obj, int cls, bool set) {
	APBridge::Bridge *b = bridge();
	if (!b || _vm->_currentScript == 0xFF || !b->wants("setclass"))
		return false;
	const char *by;
	int number;
	scriptContext(by, number);
	APBridge::Keys keys;
	keys.push_back(num(obj));
	keys.push_back(num(cls));
	keys.push_back(by);
	keys.push_back(num(number));
	APBridge::Args args;
	args.push_back(set ? 1 : 0);
	return b->notify("setclass", keys, args).veto;
}

bool ScummBridgeAdapter::onGetBit(int bit, int &value) {
	APBridge::Bridge *b = bridge();
	if (!b || _vm->_currentScript == 0xFF || !b->wants("getbit"))
		return false;
	const char *by;
	int number;
	scriptContext(by, number);
	APBridge::Keys keys;
	keys.push_back(num(bit));
	keys.push_back(by);
	keys.push_back(num(number));
	APBridge::Args args;
	args.push_back(value);
	APBridge::Verdict v = b->notify("getbit", keys, args);
	if (v.veto)
		value = v.value ? 1 : 0;
	return v.veto;
}

bool ScummBridgeAdapter::onGetVar(int var, int &value) {
	APBridge::Bridge *b = bridge();
	if (!b || _vm->_currentScript == 0xFF || !b->wants("getvar") || !b->wantsFirstKey("getvar", num(var)))
		return false;
	const char *by;
	int number;
	scriptContext(by, number);
	APBridge::Keys keys;
	keys.push_back(num(var));
	keys.push_back(by);
	keys.push_back(num(number));
	APBridge::Args args;
	args.push_back(value);
	APBridge::Verdict v = b->notify("getvar", keys, args);
	if (v.veto)
		value = v.value;
	return v.veto;
}

void ScummBridgeAdapter::pickupEnd(int obj, int slot) {
	if (_vetoedOwnerObj != obj || slot < 0 || slot >= _vm->_numInventory || _vm->_inventory[slot] != obj)
		return;
	_vetoedOwnerObj = 0;
	// Undo the inventory entry the vetoed pickup created, closing the gap the
	// same way ScummEngine::clearOwnerOf does.
	_vm->_res->nukeResource(rtInventory, slot);
	_vm->_inventory[slot] = 0;
	for (int i = slot; i < _vm->_numInventory - 1; i++) {
		if (!_vm->_inventory[i] && _vm->_inventory[i + 1]) {
			_vm->_inventory[i] = _vm->_inventory[i + 1];
			_vm->_inventory[i + 1] = 0;
			_vm->_res->_types[rtInventory][i]._address = _vm->_res->_types[rtInventory][i + 1]._address;
			_vm->_res->_types[rtInventory][i]._size = _vm->_res->_types[rtInventory][i + 1]._size;
			_vm->_res->_types[rtInventory][i + 1]._address = nullptr;
			_vm->_res->_types[rtInventory][i + 1]._size = 0;
		}
	}
}

bool ScummBridgeAdapter::onWriteVar(unsigned int var, int value) {
	APBridge::Bridge *b = bridge();
	if (!b)
		return false;
	const char *kind;
	unsigned int index;
	if (!(var & 0xF000)) {
		kind = "var";
		index = var;
	} else if (var & 0x8000) {
		kind = "bitvar";
		index = var & 0x7FFF;
	} else {
		return false; // local/script variables are not reported
	}
	if (!b->wants(kind))
		return false;
	APBridge::Keys keys;
	keys.push_back(num(index));
	APBridge::Args args;
	args.push_back(value);
	args.push_back(_vm->readVar(var));
	return b->notify(kind, keys, args).veto;
}

void ScummBridgeAdapter::onRoom(int room) {
	APBridge::Bridge *b = bridge();
	if (!b || !b->wants("room"))
		return;
	APBridge::Keys keys;
	keys.push_back(num(room));
	b->notify("room", keys, APBridge::Args());
}

bool ScummBridgeAdapter::onRunScript(int script) {
	APBridge::Bridge *b = bridge();
	if (!b || !b->wants("script"))
		return false;
	APBridge::Keys keys;
	keys.push_back(num(script));
	return b->notify("script", keys, APBridge::Args()).veto;
}

const ScummBridgeAdapter::ColorOverride *ScummBridgeAdapter::findColor(int actor, int color) {
	if (_colors.empty())
		return nullptr;
	int ego = (_vm->VAR_EGO != 0xFF) ? _vm->VAR(_vm->VAR_EGO) : 0;
	Actor *a = _vm->derefActorSafe(actor, "apbridge recolor");
	int costume = a ? a->_costume : -1;
	for (uint i = 0; i < _colors.size(); i++) {
		const ColorOverride &c = _colors[i];
		if ((c.actor ? c.actor : ego) == actor && c.index == color && (!c.costume || c.costume == costume))
			return &c;
	}
	return nullptr;
}

int ScummBridgeAdapter::recolor(int actor, int color) {
	// The room palette's closest color, matched as the game matches remapped actor colors
	const ColorOverride *c = findColor(actor, color);
	if (!c)
		return color;
	int r = c->r, g = c->g, b = c->b;
	applyLight(color, r, g, b);
	return matchColor(r, g, b);
}

int ScummBridgeAdapter::recolorAlt(int actor, int color) {
	const ColorOverride *c = findColor(actor, color);
	if (!c || c->r2 < 0)
		return -1;
	int r = c->r2, g = c->g2, b = c->b2;
	applyLight(color, r, g, b);
	return matchColor(r, g, b);
}

int ScummBridgeAdapter::rainbow(int actor, int color, int band) {
	static const byte hues[6][3] = {
		{ 230, 40, 40 }, { 240, 140, 20 }, { 240, 220, 40 }, { 40, 180, 60 }, { 40, 90, 220 }, { 150, 50, 190 }
	};
	const ColorOverride *c = findColor(actor, color);
	int bright = (c && c->r2 >= 0) ? c->r2 : 255;
	const byte *h = hues[CLIP(band, 0, 5)];
	int r = h[0] * bright / 255, g = h[1] * bright / 255, b = h[2] * bright / 255;
	applyLight(color, r, g, b);
	return matchColor(r, g, b);
}

bool ScummBridgeAdapter::pattern(int actor, byte &kind, byte &size) {
	int ego = (_vm->VAR_EGO != 0xFF) ? _vm->VAR(_vm->VAR_EGO) : 0;
	for (uint i = 0; i < _patterns.size(); i++) {
		if ((_patterns[i].actor ? _patterns[i].actor : ego) == actor) {
			kind = _patterns[i].kind;
			size = _patterns[i].size;
			return kind != 0;
		}
	}
	return false;
}

void ScummBridgeAdapter::redrawActors() {
	for (int i = 1; i < _vm->_numActors; i++)
		_vm->_actors[i]->_needRedraw = true;
}

bool ScummBridgeAdapter::performAction(const Common::String &op, const APBridge::Keys &keys, const APBridge::Args &args,
                                       int32 &result, Common::String &error) {
	int ego = (_vm->VAR_EGO != 0xFF) ? _vm->VAR(_vm->VAR_EGO) : 0;

	if (op == "spots") {
		// Replace the list of spots (objects still to be found at their home spot)
		_spots.clear();
		for (uint i = 0; i < args.size(); i++)
			if (args[i] > 0)
				_spots[args[i]] = true;
		result = _spots.size();
		return true;
	}
	if (op == "soundRunning") {
		// Whether a sound (music or effect) is playing or queued
		result = args.size() && args[0] > 0 && _vm->_sound->isSoundRunning(args[0]) ? 1 : 0;
		return true;
	}
	if (op == "stopSound") {
		// Stop particular sounds, as a script's stopSound does
		for (uint i = 0; i < args.size(); i++)
			if (args[i] > 0)
				_vm->_sound->stopSound(args[i]);
		result = 1;
		return true;
	}
	if (op == "stopSounds") {
		// Silence music and effects, as a room change made by the game's own scripts would
		_vm->_sound->stopAllSounds();
		result = 1;
		return true;
	}
	if (op == "goto") {
		// Move ego to another room, as loadRoomWithEgo(obj, room, x, y) does: placed at
		// object obj's walk-to spot and walking to (x, y), or with obj 0 put at (x, y).
		if (args.empty()) {
			error = "goto needs [room, object, x, y]";
			return false;
		}
		int room = args[0];
		int obj = args.size() > 1 ? args[1] : 0;
		int x = args.size() > 3 ? args[2] : -1;
		int y = args.size() > 3 ? args[3] : -1;
		// Before v7, rooms from 0x80 up are pseudo rooms: they run a real room's
		// code through _resourceMapper (Monkey Island 2's jungle and tunnels are
		// rooms 140-167), as startScene() does
		int resource = room;
		if (room >= 0x80 && _vm->_game.version < 7 && _vm->_game.heversion <= 71 && room < 0x100)
			resource = _vm->_resourceMapper[room & 0x7F];
		if (room <= 0 || resource <= 0 || resource >= _vm->_numRooms) {
			error = "room out of range";
			return false;
		}
		Actor *a = _vm->derefActor(_vm->VAR(_vm->VAR_EGO), "apbridge goto");
		if (obj) {
			a->putActor(room);
			_vm->_egoPositioned = false;
			_vm->VAR(_vm->VAR_WALKTO_OBJ) = obj;
			_vm->startScene(a->_room, a, obj);
			_vm->VAR(_vm->VAR_WALKTO_OBJ) = 0;
		} else {
			// No entry object: load the room as loadRoom does, then place ego
			// (what putActorInRoom + putActor do in a script)
			_vm->startScene(room, nullptr, 0);
			a->putActor(x != -1 ? x : 160, x != -1 ? y : 100, room);
		}
		_vm->camera._cur.x = _vm->camera._dest.x = a->getPos().x;
		_vm->setCameraFollows(a);
		_vm->_fullRedraw = true;
		if (obj && x != -1)
			a->startWalkActor(x, y, -1);
		result = _vm->_currentRoom;
		return true;
	}
	if (op == "talk") {
		// An actor (args[0], 0 or none = ego) says keys[0] over their head, in the game's
		// font and their talk color, with the talk animation, as printEgo does. Returns 0
		// without speaking while someone is talking, a cutscene runs, the player has no
		// control or the actor is elsewhere, so the client can try again later. With
		// args[1] = 1 it also speaks while the player has no control (in a conversation,
		// while its choices wait), but still not over another line or in a cutscene.
		if (keys.empty() || keys[0].empty()) {
			error = "talk needs the text as its key";
			return false;
		}
		int actor = (!args.empty() && args[0] > 0) ? args[0] : ego;
		Actor *a = actor ? _vm->derefActorSafe(actor, "apbridge talk") : nullptr;
		bool inTalk = args.size() > 1 && args[1] == 1;
		if (_vm->_haveMsg || _vm->vm.cutSceneStackPointer > 0 || (_vm->_userPut <= 0 && !inTalk) || !a ||
				!a->isInCurrentRoom()) {
			result = 0;
			return true;
		}
		// Printable ASCII only (game charsets have nothing else); a newline starts a new line
		Common::String msg;
		for (uint i = 0; i < keys[0].size() && msg.size() < 240; i++) {
			byte c = (byte)keys[0][i];
			if (c == '\n') {
				msg += '\xFF';
				msg += '\x01';
			} else if (c >= 0x20 && c < 0x7F) {
				msg += (char)c;
			}
		}
		_vm->_string[0].loadDefault();
		_vm->_actorToPrintStrFor = actor;
		_vm->printString(0, (const byte *)msg.c_str());
		result = 1;
		return true;
	}
	if (op == "clearColors") {
		_colors.clear();
		_patterns.clear();
		redrawActors();
		result = 1;
		return true;
	}
	if (op == "actorPattern") {
		if (args.size() < 3) {
			error = "actorPattern needs [actor, kind, size]";
			return false;
		}
		for (uint i = 0; i < _patterns.size(); i++) {
			if (_patterns[i].actor == args[0]) {
				_patterns.remove_at(i);
				break;
			}
		}
		if (args[1] > 0) {
			Pattern p = { (int)args[0], (byte)CLIP<int32>(args[1], 0, 255), (byte)CLIP<int32>(args[2], 1, 64) };
			_patterns.push_back(p);
		}
		redrawActors();
		result = _patterns.size();
		return true;
	}
	if (op == "actorColor") {
		if (args.size() < 5) {
			error = "actorColor needs [actor, color, r, g, b]";
			return false;
		}
		int actor = args[0], index = args[1];
		int costume = args.size() > 5 ? args[5] : 0;
		if (index < 0 || index >= 256) {
			error = "color out of range";
			return false;
		}
		for (uint i = 0; i < _colors.size(); i++) {
			if (_colors[i].actor == actor && _colors[i].index == index && _colors[i].costume == costume) {
				_colors.remove_at(i);
				break;
			}
		}
		if (args[2] >= 0) {
			ColorOverride c = { actor, index, costume, (byte)CLIP<int32>(args[2], 0, 255),
			                    (byte)CLIP<int32>(args[3], 0, 255), (byte)CLIP<int32>(args[4], 0, 255), -1, 0, 0 };
			if (args.size() > 8 && args[6] >= 0) {
				c.r2 = CLIP<int32>(args[6], 0, 255);
				c.g2 = CLIP<int32>(args[7], 0, 255);
				c.b2 = CLIP<int32>(args[8], 0, 255);
			}
			_colors.push_back(c);
		}
		redrawActors();
		result = _colors.size();
		return true;
	}
	if (op == "invSlots") {
		// Raw inventory entries in use, whatever their owners (the cap is _numInventory)
		result = 0;
		for (int i = 0; i < _vm->_numInventory; i++)
			if (_vm->_inventory[i])
				result++;
		return true;
	}
	if (op == "room") {
		result = _vm->_currentRoom;
		return true;
	}
	if (args.empty()) {
		error = "missing arguments";
		return false;
	}

	if (op == "getVar" || op == "setVar" || op == "getBit" || op == "setBit") {
		bool bit = op.hasSuffix("Bit");
		uint var = bit ? (0x8000 | (uint)args[0]) : (uint)args[0];
		if (!bit && args[0] >= _vm->_numVariables) {
			error = "variable out of range";
			return false;
		}
		if (bit && args[0] >= _vm->_numBitVariables) {
			error = "bit variable out of range";
			return false;
		}
		if (op.hasPrefix("set")) {
			if (args.size() < 2) {
				error = "missing value";
				return false;
			}
			_vm->writeVar(var, bit ? (args[1] ? 1 : 0) : args[1]);
		}
		result = _vm->readVar(var);
		return true;
	}

	if (op == "stopScript") {
		// Stop running scripts, as a script's stopScript does (global or local numbers)
		for (uint i = 0; i < args.size(); i++)
			if (args[i] > 0)
				_vm->stopScript(args[i]);
		result = 1;
		return true;
	}
	if (op == "runScript") {
		int scriptArgs[NUM_SCRIPT_LOCAL];
		memset(scriptArgs, 0, sizeof(scriptArgs));
		for (uint i = 1; i < args.size() && i - 1 < NUM_SCRIPT_LOCAL; i++)
			scriptArgs[i - 1] = args[i];
		_vm->runScript(args[0], false, false, scriptArgs);
		result = 1;
		return true;
	}

	int obj = args[0];
	if (obj <= 0 || obj >= _vm->_numGlobalObjects) {
		error = "object out of range";
		return false;
	}

	if (op == "startObject") {
		// Run one of the object's verb scripts, as startObject(obj, verb, [args]) would
		if (args.size() < 2) {
			error = "startObject needs [object, verb, args...]";
			return false;
		}
		int scriptArgs[NUM_SCRIPT_LOCAL];
		memset(scriptArgs, 0, sizeof(scriptArgs));
		for (uint i = 2; i < args.size() && i - 2 < NUM_SCRIPT_LOCAL; i++)
			scriptArgs[i - 2] = args[i];
		_vm->runObjectScript(obj, args[1], false, false, scriptArgs);
		result = 1;
		return true;
	}
	if (op == "setClass") {
		// As a script's setClass(obj, [classes]) does: a class with bit 0x80 is set, without it cleared.
		// Objects below the actor count are actors, as in the game's own scripts.
		if (args.size() < 2 || obj < 1 || obj >= _vm->_numGlobalObjects) {
			error = "setClass needs [object, class...]";
			return false;
		}
		for (uint i = 1; i < args.size(); i++) {
			int cls = args[i];
			if ((cls & 0x7F) < 1 || (cls & 0x7F) > 32) {
				error = "class out of range";
				return false;
			}
			_vm->putClass(obj, cls, (cls & 0x80) != 0);
		}
		result = 1;
		return true;
	}
	if (op == "setName") {
		// An object's new name (keys[0]), as a script's setObjectName does: what the
		// sentence line and the inventory show from then on.
		if (keys.empty() || keys[0].empty() || args.empty() || obj < 1 || obj >= _vm->_numGlobalObjects ||
				_vm->objIsActor(obj)) {
			error = "setName needs the name as its key and [object]";
			return false;
		}
		int slot = -1;
		for (int i = 0; i < _vm->_numNewNames && slot < 0; i++)
			if (_vm->_newNames[i] == obj)
				slot = i;
		if (slot >= 0) {
			_vm->_res->nukeResource(rtObjectName, slot);
			_vm->_newNames[slot] = 0;
		} else {
			for (int i = 0; i < _vm->_numNewNames && slot < 0; i++)
				if (_vm->_newNames[i] == 0)
					slot = i;
		}
		if (slot < 0) {
			error = "the name table is full";
			return false;
		}
		_vm->loadPtrToResource(rtObjectName, slot, (const byte *)keys[0].c_str());
		_vm->_newNames[slot] = obj;
		_vm->runInventoryScript(0);
		result = 1;
		return true;
	}
	if (op == "getClass") {
		// Whether object args[0] has class args[1] (1-32), the way the game's classOfIs sees it
		if (args.size() < 2 || obj < 1 || obj >= _vm->_numGlobalObjects || args[1] < 1 || args[1] > 32) {
			error = "getClass needs [object, class]";
			return false;
		}
		result = _vm->getClass(obj, args[1]) ? 1 : 0;
		return true;
	}
	if (op == "getOwner") {
		result = _vm->getOwner(obj);
		return true;
	}
	if (op == "setOwner") {
		int owner = args.size() > 1 ? args[1] : -1;
		if (owner < 0)
			owner = ego;
		// Moving an object away from the inventory must clear its inventory
		// entry first, the way game scripts do (setOwnerOf(obj, 0) then the
		// new owner). Otherwise the entry stays and a later give duplicates it.
		if (owner != 0 && owner != ego && inInventory(obj))
			_vm->setOwnerOf(obj, 0);
		_vm->setOwnerOf(obj, owner);
		result = owner;
		return true;
	}
	if (op == "give") {
		// Check the inventory itself, not just the owner field, so a stale
		// entry can never be duplicated.
		if (!inInventory(obj)) {
			if (args.size() < 2) {
				error = "give needs [object, homeRoom]";
				return false;
			}
			_vm->addObjectToInventory(obj, args[1]);
		}
		_vm->putOwner(obj, ego);
		_vm->runInventoryScript(1);
		result = ego;
		return true;
	}
	if (op == "take") {
		_vm->setOwnerOf(obj, 0);
		result = 0;
		return true;
	}
	error = "unknown op " + op;
	return false;
}

} // End of namespace Scumm
