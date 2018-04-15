// Copyright (c) 2017- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include "Core/Debugger/WebSocket.h"
#include "Core/Debugger/WebSocket/WebSocketUtils.h"

// This WebSocket (connected through the same port as disc sharing) allows API/debugger access to PPSSPP.
// Currently, the only subprotocol "debugger.ppsspp.org" uses a simple JSON based interface.
//
// Messages to and from PPSSPP follow the same basic format:
//    { "event": "NAME", ... }
//
// And are primarily of these types:
//  * Events from the debugger/client (you) to PPSSPP
//    If there's a response, it will generally use the same name.  It may not be immedate - it's an event.
//  * Spontaneous events from PPSSPP
//    Things like logs, breakpoint hits, etc. not directly requested.
//
// Otherwise you may see error events which indicate PPSSPP couldn't understand or failed internally:
//  - "event": "error"
//  - "message": A string describing what happened.
//  - "level": Integer severity level. (1 = NOTICE, 2 = ERROR, 3 = WARN, 4 = INFO, 5 = DEBUG, 6 = VERBOSE)
//  - "ticket": Optional, present if in response to an event with a "ticket" field, simply repeats that value.

#include "Core/Debugger/WebSocket/GameBroadcaster.h"
#include "Core/Debugger/WebSocket/LogBroadcaster.h"
#include "Core/Debugger/WebSocket/SteppingBroadcaster.h"

#include "Core/Debugger/WebSocket/CPUCoreSubscriber.h"

typedef void (*DebuggerEventHandler)(DebuggerRequest &req);
static const std::unordered_map<std::string, DebuggerEventHandler> debuggerEvents({
	{"cpu.getAllRegs", &WebSocketCPUGetAllRegs},
	{"cpu.getReg", &WebSocketCPUGetReg},
	{"cpu.setReg", &WebSocketCPUSetReg},
});

void HandleDebuggerRequest(const http::Request &request) {
	net::WebSocketServer *ws = net::WebSocketServer::CreateAsUpgrade(request, "debugger.ppsspp.org");
	if (!ws)
		return;

	LogBroadcaster logger;
	GameBroadcaster game;
	SteppingBroadcaster stepping;

	ws->SetTextHandler([&](const std::string &t) {
		JsonReader reader(t.c_str(), t.size());
		if (!reader.ok()) {
			ws->Send(DebuggerErrorEvent("Bad message: invalid JSON", LogTypes::LERROR));
			return;
		}

		const JsonGet root = reader.root();
		const char *event = root ? root.getString("event", nullptr) : nullptr;
		if (!event) {
			ws->Send(DebuggerErrorEvent("Bad message: no event property", LogTypes::LERROR, root));
			return;
		}

		DebuggerRequest req(event, ws, root);

		auto eventFunc = debuggerEvents.find(event);
		if (eventFunc != debuggerEvents.end()) {
			eventFunc->second(req);
			req.Finish();
		} else {
			req.Fail("Bad message: unknown event");
		}
	});
	ws->SetBinaryHandler([&](const std::vector<uint8_t> &d) {
		ws->Send(DebuggerErrorEvent("Bad message", LogTypes::LERROR));
	});

	while (ws->Process(1.0f / 60.0f)) {
		// These send events that aren't just responses to requests.
		logger.Broadcast(ws);
		game.Broadcast(ws);
		stepping.Broadcast(ws);
	}

	delete ws;
}
