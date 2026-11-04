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

#ifndef SCI_ENGINE_APBRIDGE_SCI_H
#define SCI_ENGINE_APBRIDGE_SCI_H

#include "engines/apbridge.h"
#include "sci/engine/vm_types.h"

namespace Sci {

struct EngineState;

/**
 * SCI side of the Archipelago bridge.
 *
 * Events:
 *   call     keys [script, export]          args = procedure arguments
 *   send     keys [object, selector, class] args = method arguments
 *   setprop  keys [object, selector, class] args = [new value]  (vetoable: skip write)
 *   getprop  keys [object, selector, class] args = []           (vetoable: replace value)
 *            only reads made by message, e.g. "((gInventory at: 9) owner:)";
 *            an object's own methods read properties directly and are unaffected
 *   kernel   keys [name]                    args = kernel arguments
 *   ctx: room (global 11)
 *
 * Actions (cmd "do"):
 *   getGlobal  args [n]            setGlobal args [n, value]
 *   getBit     args [base, bit]    setBit    args [base, bit, 0|1]
 *       (bit arrays as used by Sierra's Btst/Bset: word base + bit/16,
 *        mask 0x8000 >> bit%16)
 *   getProp    keys [obj, selector]          setProp keys [obj, selector] args [value]
 *   send       keys [obj, selector]          args = method arguments
 *       obj is an object name, "name@script" to pick the object defined in
 *       that script when a name is ambiguous, or "global:N" for global N
 *   talk       keys [text, title]
 *       Shows text in a Sierra-style message window (titled when a title is
 *       given) and waits for a click, a key or a timeout, as the games' own
 *       Print does. Result 0, without showing anything, while another window
 *       is open; SCI16 games only.
 */
class SciBridgeAdapter : public APBridge::Adapter {
public:
	SciBridgeAdapter(EngineState *s) : _state(s), _kArgc(0), _kArgv(nullptr) {}

	const char *engineId() const override { return "sci"; }
	void getContext(Common::HashMap<Common::String, int32> &ctx) override;
	bool performAction(const Common::String &op, const APBridge::Keys &keys, const APBridge::Args &args,
	                   int32 &result, Common::String &error) override;
	bool canInterrupt() override;

	/** Run queued client actions; call from inside a kernel call. */
	void tick(int argc, reg_t *argv);

	void setState(EngineState *s) { _state = s; }

private:
	reg_t resolveObject(const Common::String &name);
	bool showTextBox(const APBridge::Keys &keys, int32 &result, Common::String &error);
	bool globalIndex(int32 n) const;

	EngineState *_state;
	int _kArgc;
	reg_t *_kArgv;
};

/** Hook helpers called from the VM. Return true to veto, with @p acc set. */
bool apBridgeCall(EngineState *s, uint16 script, uint16 pubfunct, uint16 argc, const reg_t *argp, reg_t &acc);
bool apBridgeSend(EngineState *s, reg_t obj, int selector, bool isMethod, uint16 argc, const reg_t *argp, reg_t &acc);
void apBridgeKernel(EngineState *s, const char *name, int argc, const reg_t *argv);

} // End of namespace Sci

#endif
