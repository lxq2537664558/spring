/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "UnsyncedGameCommands.h"

#include "UnsyncedActionExecutor.h"
#include "SyncedGameCommands.h"
#include "SyncedActionExecutor.h"
#include "Action.h"
#include "CameraHandler.h"
#include "ConsoleHistory.h"
#include "CommandMessage.h"
#include "Game.h"
#include "GameSetup.h"
#include "GlobalUnsynced.h"
#include "SelectedUnitsHandler.h"
#include "WordCompletion.h"
#include "InMapDraw.h"
#include "InMapDrawModel.h"
#include "IVideoCapturing.h"
#ifdef _WIN32
#  include "winerror.h" // TODO someone on windows (MinGW? VS?) please check if this is required
#endif

#include "ExternalAI/AILibraryManager.h"
#include "ExternalAI/SkirmishAIHandler.h"

#include "Game/Players/Player.h"
#include "Game/Players/PlayerHandler.h"
#include "Game/UI/CommandColors.h"
#include "Game/UI/EndGameBox.h"
#include "Game/UI/GameInfo.h"
#include "Game/UI/GuiHandler.h"
#include "Game/UI/InfoConsole.h"
#include "Game/UI/InputReceiver.h"
#include "Game/UI/KeyBindings.h"
#include "Game/UI/KeyCodes.h"
#include "Game/UI/MiniMap.h"
#include "Game/UI/ProfileDrawer.h"
#include "Game/UI/QuitBox.h"
#include "Game/UI/ResourceBar.h"
#include "Game/UI/SelectionKeyHandler.h"
#include "Game/UI/ShareBox.h"
#include "Game/UI/TooltipConsole.h"
#include "Game/UI/UnitTracker.h"
#include "Game/UI/Groups/GroupHandler.h"
#include "Game/UI/PlayerRoster.h"

#include "Lua/LuaOpenGL.h"
#include "Lua/LuaUI.h"

#include "Map/Ground.h"
#include "Map/MetalMap.h"
#include "Map/ReadMap.h"
#include "Map/SMF/SMFGroundDrawer.h"
#include "Map/SMF/ROAM/Patch.h"
#include "Map/SMF/ROAM/RoamMeshDrawer.h"

#include "Net/GameServer.h"
#include "Net/Protocol/NetProtocol.h"

#include "Rendering/DebugColVolDrawer.h"
#include "Rendering/DebugDrawerAI.h"
#include "Rendering/IPathDrawer.h"
#include "Rendering/FeatureDrawer.h"
#include "Rendering/HUDDrawer.h"
#include "Rendering/LuaObjectDrawer.h"
#include "Rendering/Screenshot.h"
#include "Rendering/ShadowHandler.h"
#include "Rendering/SmoothHeightMeshDrawer.h"
#include "Rendering/TeamHighlight.h"
#include "Rendering/UnitDrawer.h"
#include "Rendering/VerticalSync.h"
#include "Rendering/Env/IGroundDecalDrawer.h"
#include "Rendering/Env/ISky.h"
#include "Rendering/Env/ITreeDrawer.h"
#include "Rendering/Env/IWater.h"
#include "Rendering/Env/GrassDrawer.h"
#include "Rendering/Env/Particles/ProjectileDrawer.h"
#include "Rendering/Fonts/glFont.h"
#include "Rendering/Map/InfoTexture/IInfoTextureHandler.h"
#include "Rendering/Map/InfoTexture/Modern/Path.h"
#include "Rendering/Shaders/ShaderHandler.h"

#include "Sim/MoveTypes/MoveDefHandler.h"
#include "Sim/Misc/TeamHandler.h"
#include "Sim/Misc/ModInfo.h"
#include "Sim/Projectiles/ProjectileHandler.h"
#include "Sim/Units/UnitDef.h"
#include "Sim/Units/UnitDefHandler.h"
#include "Sim/Units/UnitHandler.h"

#include "System/EventHandler.h"
#include "System/GlobalConfig.h"
#include "System/SafeUtil.h"
#include "System/TimeProfiler.h"
#include "System/Log/ILog.h"
#include "System/Config/ConfigHandler.h"
#include "System/FileSystem/SimpleParser.h"
#include "System/Sound/ISound.h"
#include "System/Sound/ISoundChannels.h"
#include "System/Sync/DumpState.h"

#include <SDL_events.h>
#include <SDL_video.h>


static std::vector<std::string> _local_strSpaceTokenize(const std::string& text) {
	static const char* const SPACE_DELIMS = " \t";

	std::vector<std::string> tokens;

	// Skip delimiters at beginning.
	std::string::size_type lastPos = text.find_first_not_of(SPACE_DELIMS, 0);
	// Find first "non-delimiter".
	std::string::size_type pos     = text.find_first_of(SPACE_DELIMS, lastPos);

	while (std::string::npos != pos || std::string::npos != lastPos) {
		// Found a token, add it to the vector.
		tokens.push_back(text.substr(lastPos, pos - lastPos));

		// Skip delimiters.  Note the "not_of"
		lastPos = text.find_first_not_of(SPACE_DELIMS, pos);
		// Find next "non-delimiter"
		pos = text.find_first_of(SPACE_DELIMS, lastPos);
	}

	return tokens;
}



namespace { // prevents linking problems in case of duplicate symbols

/**
 * Special case executor which is used for creating aliases to other commands.
 * The inner executor will be delet'ed in this executors dtor.
 */
class AliasActionExecutor : public IUnsyncedActionExecutor {
public:
	AliasActionExecutor(IUnsyncedActionExecutor* innerExecutor, const std::string& commandAlias)
		: IUnsyncedActionExecutor(commandAlias, "Alias for command \"" + commandAlias + "\"")
		, innerExecutor(innerExecutor)
	{
		assert(innerExecutor != nullptr);
	}

	virtual ~AliasActionExecutor() {
		delete innerExecutor;
	}

	bool Execute(const UnsyncedAction& action) const {
		return innerExecutor->ExecuteAction(action);
	}

private:
	IUnsyncedActionExecutor* innerExecutor;
};



/**
 * Special case executor which allows to combine multiple commands into one,
 * by calling them sequentially.
 * The inner executors will be delet'ed in this executors dtor.
 */
class SequentialActionExecutor : public IUnsyncedActionExecutor {
public:
	SequentialActionExecutor(const std::string& command): IUnsyncedActionExecutor(command, "Executes the following commands in order:") {
	}

	virtual ~SequentialActionExecutor() {
		for (IUnsyncedActionExecutor* e: innerExecutors) {
			delete e;
		}
	}

	void AddExecutor(IUnsyncedActionExecutor* innerExecutor) {
		innerExecutors.push_back(innerExecutor);
		SetDescription(GetDescription() + " " + innerExecutor->GetCommand());
	}

	bool Execute(const UnsyncedAction& action) const {
		for (IUnsyncedActionExecutor* e: innerExecutors) {
			e->ExecuteAction(action);
		}

		return true;
	}

private:
	std::vector<IUnsyncedActionExecutor*> innerExecutors;
};



class SelectActionExecutor : public IUnsyncedActionExecutor {
public:
	SelectActionExecutor() : IUnsyncedActionExecutor("Select", "<chat command description: Select>") {
	} // TODO

	bool Execute(const UnsyncedAction& action) const {
		selectionKeys->DoSelection(action.GetArgs()); //TODO give it a return argument?
		return true;
	}
};

class SelectUnitsActionExecutor : public IUnsyncedActionExecutor {
public:
	SelectUnitsActionExecutor() : IUnsyncedActionExecutor("SelectUnits", "<chat command description: SelectUnits>") {
	} // TODO

	bool Execute(const UnsyncedAction& action) const {
		selectedUnitsHandler.SelectUnits(action.GetArgs()); //TODO give it a return argument?
		return true;
	}
};

class SelectCycleActionExecutor : public IUnsyncedActionExecutor {
public:
	SelectCycleActionExecutor() : IUnsyncedActionExecutor("SelectCycle", "<chat command description: SelectUnits>") {
	} // TODO

	bool Execute(const UnsyncedAction& action) const {
		selectedUnitsHandler.SelectCycle(action.GetArgs()); //TODO give it a return argument?
		return true;
	}
};

class DeselectActionExecutor : public IUnsyncedActionExecutor {
public:
	DeselectActionExecutor() : IUnsyncedActionExecutor("Deselect", "Deselects all currently selected units") {
	}

	bool Execute(const UnsyncedAction& action) const {
		selectedUnitsHandler.ClearSelected();
		return true;
	}
};



class MapMeshDrawerActionExecutor : public IUnsyncedActionExecutor {
public:
	MapMeshDrawerActionExecutor() : IUnsyncedActionExecutor("mapmeshdrawer", "Switch map-mesh rendering modes: 0=GCM, 1=HLOD, 2=ROAM") {
	}

	bool Execute(const UnsyncedAction& action) const {
		CSMFGroundDrawer* smfGD = dynamic_cast<CSMFGroundDrawer*>(readMap->GetGroundDrawer());

		if (smfGD == nullptr)
			return false;

		if (!action.GetArgs().empty()) {
			int rendererMode = -1;
			int roamPatchMode = -1;

			sscanf((action.GetArgs()).c_str(), "%i %i", &rendererMode, &roamPatchMode);

			smfGD->SwitchMeshDrawer(rendererMode);
		} else {
			smfGD->SwitchMeshDrawer();
		}

		return true;
	}
};


class MapBorderActionExecutor : public IUnsyncedActionExecutor {
public:
	MapBorderActionExecutor() : IUnsyncedActionExecutor("MapBorder", "Set or toggle map-border rendering") {
	}

	bool Execute(const UnsyncedAction& action) const {
		CSMFGroundDrawer* smfGD = dynamic_cast<CSMFGroundDrawer*>(readMap->GetGroundDrawer());

		if (smfGD == nullptr)
			return false;

		if (!action.GetArgs().empty()) {
			bool enable = true;
			InverseOrSetBool(enable, action.GetArgs());

			if (enable != smfGD->ToggleMapBorder())
				smfGD->ToggleMapBorder();

		} else {
			smfGD->ToggleMapBorder();
		}

		return true;
	}
};


class ShadowsActionExecutor : public IUnsyncedActionExecutor {
public:
	ShadowsActionExecutor() : IUnsyncedActionExecutor(
		"Shadows",
		"Disables/Enables shadows rendering: -1=disabled, 0=off, 1=full shadows, 2=skip terrain shadows"
	) {
	}

	bool Execute(const UnsyncedAction& action) const {
		if (shadowHandler.shadowConfig < 0) {
			LOG_L(L_WARNING, "Shadows are disabled; change your configuration and restart to use them");
			return true;
		}
		if (!CShadowHandler::ShadowsSupported()) {
			LOG_L(L_WARNING, "Your hardware/driver setup does not support shadows");
			return true;
		}

		shadowHandler.Reload(((action.GetArgs()).empty())? nullptr: (action.GetArgs()).c_str());
		LOG("Set \"shadows\" config-parameter to %i", shadowHandler.shadowConfig);
		return true;
	}
};

class MapShadowPolyOffsetActionExecutor: public IUnsyncedActionExecutor {
public:
	MapShadowPolyOffsetActionExecutor(): IUnsyncedActionExecutor("MapShadowPolyOffset", "") {
	}

	bool Execute(const UnsyncedAction& action) const {
		std::istringstream buf(action.GetArgs() + " 0.0 0.0");

		float& pofs = (readMap->GetGroundDrawer())->spPolygonOffsetScale;
		float& pofu = (readMap->GetGroundDrawer())->spPolygonOffsetUnits;

		buf >> pofs;
		buf >> pofu;

		LOG("MapShadowPolygonOffset{Scale,Units}={%f,%f}", pofs, pofu);
		return true;
	}
};



class WaterActionExecutor : public IUnsyncedActionExecutor {
public:
	WaterActionExecutor() : IUnsyncedActionExecutor(
		"Water",
		"Set water rendering mode: 0=basic, 1=reflective, 2=dynamic, 3=reflective&refractive, 4=bump-mapped"
	) {
	}

	bool Execute(const UnsyncedAction& action) const
	{
		int nextWaterRendererMode = 0;

		if (!(action.GetArgs()).empty()) {
			nextWaterRendererMode = atoi((action.GetArgs()).c_str());
		} else {
			nextWaterRendererMode = -1;
		}

		IWater::PushWaterMode(nextWaterRendererMode);
		return true;
	}
};



class SayActionExecutor : public IUnsyncedActionExecutor {
public:
	SayActionExecutor() : IUnsyncedActionExecutor("Say", "Say something in (public) chat") {
	}

	bool Execute(const UnsyncedAction& action) const {
		game->SendNetChat(action.GetArgs());
		return true;
	}
};



class SayPrivateActionExecutor : public IUnsyncedActionExecutor {
public:
	SayPrivateActionExecutor() : IUnsyncedActionExecutor("W", "Say something in private to a specific player, by player-name") {
	}

	bool Execute(const UnsyncedAction& action) const {
		const std::string::size_type pos = action.GetArgs().find_first_of(" ");

		if (pos != std::string::npos) {
			const std::string name = action.GetArgs().substr(0, pos);
			const int playerID = playerHandler.Player(name);

			if (playerID >= 0) {
				game->SendNetChat(action.GetArgs().substr(pos + 1), playerID);
			} else {
				LOG_L(L_WARNING, "/w: Player not found: %s", name.c_str());
			}
		} else {
			LOG_L(L_WARNING, "/w: wrong syntax (which is '/w %%playername')");
		}

		return true;
	}
};



class SayPrivateByPlayerIDActionExecutor : public IUnsyncedActionExecutor {
public:
	SayPrivateByPlayerIDActionExecutor() : IUnsyncedActionExecutor("WByNum", "Say something in private to a specific player, by player-ID") {
	}

	bool Execute(const UnsyncedAction& action) const {
		const std::string::size_type pos = action.GetArgs().find_first_of(" ");

		if (pos != std::string::npos) {
			std::istringstream buf(action.GetArgs().substr(0, pos));
			int playerID;
			buf >> playerID;

			if (playerID >= 0) {
				game->SendNetChat(action.GetArgs().substr(pos + 1), playerID);
			} else {
				LOG_L(L_WARNING, "Player-ID invalid: %i", playerID);
			}
		} else {
			LOG_L(L_WARNING, "/WByNum: wrong syntax (which is '/WByNum %%playerid')");
		}

		return true;
	}
};



class EchoActionExecutor : public IUnsyncedActionExecutor {
public:
	EchoActionExecutor() : IUnsyncedActionExecutor("Echo", "Write a string to the log file") {
	}

	bool Execute(const UnsyncedAction& action) const {
		LOG("%s", action.GetArgs().c_str());
		return true;
	}
};



class SetActionExecutor : public IUnsyncedActionExecutor {
public:
	SetActionExecutor() : IUnsyncedActionExecutor("Set", "Set a config key=value pair") {
	}

	bool Execute(const UnsyncedAction& action) const {
		const std::string::size_type pos = action.GetArgs().find_first_of(" ");

		if (pos != std::string::npos) {
			const std::string varName = action.GetArgs().substr(0, pos);
			configHandler->SetString(varName, action.GetArgs().substr(pos+1));
		} else {
			LOG_L(L_WARNING, "/set: wrong syntax (which is '/set %%cfgtag %%cfgvalue')");
		}

		return true;
	}
};



class SetOverlayActionExecutor : public IUnsyncedActionExecutor {
public:
	SetOverlayActionExecutor() : IUnsyncedActionExecutor(
		"TSet",
		"Set a config key=value pair in the overlay, meaning it will not persist for future games"
	) {
	}

	bool Execute(const UnsyncedAction& action) const {
		const std::string::size_type pos = action.GetArgs().find_first_of(" ");

		if (pos != std::string::npos) {
			const std::string varName = action.GetArgs().substr(0, pos);
			configHandler->SetString(varName, action.GetArgs().substr(pos+1), true);
		} else {
			LOG_L(L_WARNING, "/tset: wrong syntax (which is '/tset %%cfgtag %%cfgvalue')");
		}

		return true;
	}
};



class EnableDrawInMapActionExecutor : public IUnsyncedActionExecutor {
public:
	EnableDrawInMapActionExecutor() : IUnsyncedActionExecutor("DrawInMap", "Enables drawing on the map") {
	}

	bool Execute(const UnsyncedAction& action) const {
		inMapDrawer->SetDrawMode(true);
		return true;
	}
};



class DrawLabelActionExecutor : public IUnsyncedActionExecutor {
public:
	DrawLabelActionExecutor() : IUnsyncedActionExecutor("DrawLabel", "Draws a label on the map at the current mouse-pointer position") {
	}

	bool Execute(const UnsyncedAction& action) const {
		const float3 pos = inMapDrawer->GetMouseMapPos();

		if (pos.x >= 0.0f) {
			inMapDrawer->SetDrawMode(false);
			inMapDrawer->PromptLabel(pos);
		} else {
			LOG_L(L_WARNING, "/DrawLabel: move mouse over the map");
		}

		return true;
	}
};



class MouseActionExecutor : public IUnsyncedActionExecutor {
public:
	MouseActionExecutor(int _button): IUnsyncedActionExecutor(
		"Mouse" + IntToString(_button),
		"Simulates a mouse button press of button " + IntToString(_button)
	) {
		button = _button;
	}

	bool Execute(const UnsyncedAction& action) const {
		if (!action.IsRepeat())
			mouse->MousePress(mouse->lastx, mouse->lasty, button);

		return true;
	}

private:
	int button;
};



class ViewSelectionActionExecutor : public IUnsyncedActionExecutor {
public:
	ViewSelectionActionExecutor() : IUnsyncedActionExecutor(
		"ViewSelection",
		"Moves the camera to the center of the currently selected units"
	) {
	}

	bool Execute(const UnsyncedAction& action) const {
		const auto& selUnits = selectedUnitsHandler.selectedUnits;

		if (selUnits.empty())
			return false;

		// XXX this code is duplicated in CGroup::CalculateCenter()
		float3 pos;

		for (const int unitID: selUnits) {
			pos += (unitHandler.GetUnit(unitID))->midPos;
		}

		camHandler->CameraTransition(0.6f);
		camHandler->GetCurrentController().SetPos(pos * (1.0f / selUnits.size()));
		return true;
	}
};



class CameraMoveActionExecutor : public IUnsyncedActionExecutor {
public:
	CameraMoveActionExecutor(
		int _moveStateIdx,
		const std::string& commandPostfix
	): IUnsyncedActionExecutor("Move" + commandPostfix, "Moves the camera " + commandPostfix + " a bit") {
		moveStateIdx = _moveStateIdx;
	}

	bool Execute(const UnsyncedAction& action) const {
		camera->SetMovState(moveStateIdx, true);
		return true;
	}

private:
	int moveStateIdx;
};



class AIKillReloadActionExecutor : public IUnsyncedActionExecutor {
public:
	/**
	 * @param kill whether this executor should function as the kill-
	 * or the reload-AI command
	 */
	AIKillReloadActionExecutor(bool kill_): IUnsyncedActionExecutor(
		(kill_ ? "AIKill" : "AIReload"),
		std::string(kill_ ? "Kills" : "Reloads") + " the Skirmish AI controlling a specified team"
	) {
		kill = kill_;
	}

	bool Execute(const UnsyncedAction& action) const {
		bool badArgs = false;

		const CPlayer* fromPlayer     = playerHandler.Player(gu->myPlayerNum);
		const int      fromTeamId     = (fromPlayer != nullptr) ? fromPlayer->team : -1;

		const bool cheating           = gs->cheatEnabled;
		const bool singlePlayer       = (playerHandler.ActivePlayers() <= 1);

		const std::vector<std::string>& args = _local_strSpaceTokenize(action.GetArgs());
		const std::string actionName  = StringToLower(GetCommand()).substr(2);

		if (!args.empty()) {
			size_t skirmishAIId = 0; // will only be used if !badArgs
			bool share = false;

			int teamToReceiveUnitsId = -1;
			int teamToKillId = atoi(args[0].c_str());

			if ((args.size() >= 2) && kill) {
				teamToReceiveUnitsId = atoi(args[1].c_str());
				share = true;
			}

			CTeam* teamToKill = (teamHandler.IsActiveTeam(teamToKillId))? teamHandler.Team(teamToKillId) : NULL;
			const CTeam* teamToReceiveUnits = (teamHandler.IsActiveTeam(teamToReceiveUnitsId))? teamHandler.Team(teamToReceiveUnitsId): NULL;

			if (teamToKill == nullptr) {
				LOG_L(L_WARNING, "Team to %s: not a valid team number: \"%s\"", actionName.c_str(), args[0].c_str());
				badArgs = true;
			}
			if (share && teamToReceiveUnits == NULL) {
				LOG_L(L_WARNING, "Team to receive units: not a valid team number: \"%s\"", args[1].c_str());
				badArgs = true;
			}
			if (!badArgs && (skirmishAIHandler.GetSkirmishAIsInTeam(teamToKillId).size() == 0)) {
				LOG_L(L_WARNING, "Team to %s: not a Skirmish AI team: %i", actionName.c_str(), teamToKillId);
				badArgs = true;
			} else {
				const std::vector<uint8_t>& teamAIs = skirmishAIHandler.GetSkirmishAIsInTeam(teamToKillId, gu->myPlayerNum);
				if (!teamAIs.empty()) {
					skirmishAIId = teamAIs[0];
				} else {
					LOG_L(L_WARNING, "Team to %s: not a local Skirmish AI team: %i", actionName.c_str(), teamToKillId);
					badArgs = true;
				}
			}
			if (!badArgs && skirmishAIHandler.GetSkirmishAI(skirmishAIId)->isLuaAI) {
				LOG_L(L_WARNING, "Team to %s: it is not yet supported to %s Lua AIs", actionName.c_str(), actionName.c_str());
				badArgs = true;
			}
			if (!badArgs) {
				const bool weAreAllied  = teamHandler.AlliedTeams(fromTeamId, teamToKillId);
				const bool weAreAIHost  = (skirmishAIHandler.GetSkirmishAI(skirmishAIId)->hostPlayer == gu->myPlayerNum);
				const bool weAreLeader  = (teamToKill->GetLeader() == gu->myPlayerNum);

				if (!(weAreAIHost || weAreLeader || singlePlayer || (weAreAllied && cheating))) {
					LOG_L(L_WARNING, "Team to %s: player %s is not allowed to %s Skirmish AI controlling team %i (try with /cheat)",
							actionName.c_str(), fromPlayer->name.c_str(), actionName.c_str(), teamToKillId);
					badArgs = true;
				}
			}
			if (!badArgs && teamToKill->isDead) {
				LOG_L(L_WARNING, "Team to %s: is a dead team already: %i", actionName.c_str(), teamToKillId);
				badArgs = true;
			}

			if (!badArgs) {
				if (kill) {
					if (share) {
						clientNet->Send(CBaseNetProtocol::Get().SendGiveAwayEverything(gu->myPlayerNum, teamToReceiveUnitsId, teamToKillId));
						// when the AIs team has no units left,
						// the AI will be destroyed automatically
					} else {
						if (skirmishAIHandler.IsLocalSkirmishAI(skirmishAIId))
							skirmishAIHandler.SetLocalKillFlag(skirmishAIId, 3 /* = AI killed */);
					}
				} else {
					// reload
					clientNet->Send(CBaseNetProtocol::Get().SendAIStateChanged(gu->myPlayerNum, skirmishAIId, SKIRMAISTATE_RELOADING));
				}

				LOG("Skirmish AI controlling team %i is beeing %sed ...", teamToKillId, actionName.c_str());
			}
		} else {
			LOG_L(L_WARNING, "/%s: missing mandatory argument \"teamTo%s\"", GetCommand().c_str(), actionName.c_str());
			badArgs = true;
		}

		if (badArgs) {
			if (kill) {
				LOG("description: "
					"Kill a Skirmish AI controlling a team. The team itself will remain alive "
					"unless a second argument is given, which specifies an active team "
					"that will receive all the units of the AI team.");
				LOG("usage:   /%s teamToKill [teamToReceiveUnits]", GetCommand().c_str());
			} else {
				// reload
				LOG("description: "
					"Reload a Skirmish AI controlling a team."
					"The team itself will remain alive during the process.");
				LOG("usage:   /%s teamToReload", GetCommand().c_str());
			}
		}

		return true;
	}

private:
	bool kill;
};



class AIControlActionExecutor : public IUnsyncedActionExecutor {
public:
	AIControlActionExecutor() : IUnsyncedActionExecutor(
		"AIControl",
		"Creates a new instance of a Skirmish AI, to let it control a specific team"
	) {
	}

	bool Execute(const UnsyncedAction& action) const {
		bool badArgs = false;

		const CPlayer* fromPlayer     = playerHandler.Player(gu->myPlayerNum);
		const int      fromTeamId     = (fromPlayer != nullptr) ? fromPlayer->team : -1;

		const bool cheating           = gs->cheatEnabled;
		const bool singlePlayer       = (playerHandler.ActivePlayers() <= 1);

		const std::vector<std::string>& args = _local_strSpaceTokenize(action.GetArgs());

		if (!args.empty()) {
			std::string aiShortName;
			std::string aiVersion;
			std::string aiName;
			spring::unordered_map<std::string, std::string> aiOptions;

			const int teamToControlId = atoi(args[0].c_str());
			const CTeam* teamToControl = teamHandler.IsActiveTeam(teamToControlId) ?
				teamHandler.Team(teamToControlId) : nullptr;

			if (args.size() >= 2) {
				aiShortName = args[1];
			} else {
				LOG_L(L_WARNING, "/%s: missing mandatory argument \"aiShortName\"", GetCommand().c_str());
			}

			if (args.size() >= 3)
				aiVersion = args[2];

			if (args.size() >= 4)
				aiName = args[3];

			if (teamToControl == nullptr) {
				LOG_L(L_WARNING, "Team to control: not a valid team number: \"%s\"", args[0].c_str());
				badArgs = true;
			}
			if (!badArgs) {
				const bool weAreAllied  = teamHandler.AlliedTeams(fromTeamId, teamToControlId);
				const bool weAreLeader  = (teamToControl->GetLeader() == gu->myPlayerNum);
				const bool noLeader     = (!teamToControl->HasLeader());

				if (!(weAreLeader || singlePlayer || (weAreAllied && (cheating || noLeader)))) {
					LOG_L(L_WARNING, "Team to control: player %s is not allowed to let a Skirmish AI take over control of team %i (try with /cheat)",
							fromPlayer->name.c_str(), teamToControlId);
					badArgs = true;
				}
			}
			if (!badArgs && teamToControl->isDead) {
				LOG_L(L_WARNING, "Team to control: is a dead team: %i", teamToControlId);
				badArgs = true;
			}
			// TODO remove this, if support for multiple Skirmish AIs per team is in place
			if (!badArgs && (!skirmishAIHandler.GetSkirmishAIsInTeam(teamToControlId).empty())) {
				LOG_L(L_WARNING, "Team to control: there is already an AI controlling this team: %i", teamToControlId);
				badArgs = true;
			}
			if (!badArgs && (skirmishAIHandler.GetLocalSkirmishAIInCreation(teamToControlId) != nullptr)) {
				LOG_L(L_WARNING, "Team to control: there is already an AI beeing created for team: %i", teamToControlId);
				badArgs = true;
			}
			if (!badArgs) {
				const spring::unordered_set<std::string>& luaAIImplShortNames = skirmishAIHandler.GetLuaAIImplShortNames();
				if (luaAIImplShortNames.find(aiShortName) != luaAIImplShortNames.end()) {
					LOG_L(L_WARNING, "Team to control: it is currently not supported to initialize Lua AIs mid-game");
					badArgs = true;
				}
			}

			if (!badArgs) {
				SkirmishAIKey aiKey(aiShortName, aiVersion);
				aiKey = aiLibManager->ResolveSkirmishAIKey(aiKey);

				if ((badArgs = aiKey.IsUnspecified())) {
					LOG_L(L_WARNING, "Skirmish AI: not a valid Skirmish AI: %s %s", aiShortName.c_str(), aiVersion.c_str());
				} else {
					const CSkirmishAILibraryInfo& aiLibInfo = aiLibManager->GetSkirmishAIInfos().find(aiKey)->second;

					SkirmishAIData aiData;
					aiData.name       = (aiName != "") ? aiName : aiShortName;
					aiData.team       = teamToControlId;
					aiData.hostPlayer = gu->myPlayerNum;
					aiData.shortName  = aiShortName;
					aiData.version    = aiVersion;

					for (const auto& opt: aiOptions)
						aiData.optionKeys.push_back(opt.first);

					aiData.options = aiOptions;
					aiData.isLuaAI = aiLibInfo.IsLuaAI();

					skirmishAIHandler.CreateLocalSkirmishAI(aiData);
				}
			}
		} else {
			LOG_L(L_WARNING, "/%s: missing mandatory arguments \"teamToControl\" and \"aiShortName\"", GetCommand().c_str());
			badArgs = true;
		}

		if (badArgs) {
			LOG("description: Let a Skirmish AI take over control of a team.");
			LOG("usage:   /%s teamToControl aiShortName [aiVersion] [name] [options...]", GetCommand().c_str());
			LOG("example: /%s 1 RAI 0.601 my_RAI_Friend difficulty=2 aggressiveness=3", GetCommand().c_str());
		}

		return true;
	}
};



class AIListActionExecutor : public IUnsyncedActionExecutor {
public:
	AIListActionExecutor() : IUnsyncedActionExecutor("AIList", "Prints a list of all currently active Skirmish AIs") {
	}

	bool Execute(const UnsyncedAction& action) const {
		const spring::unordered_map<uint8_t, SkirmishAIData>& ais = skirmishAIHandler.GetAllSkirmishAIs();

		if (ais.empty()) {
			LOG("<There are no active Skirmish AIs in this game>");
			return false;
		}

		LOG("%s | %s | %s | %s | %s | %s",
				"ID",
				"Team",
				"Local",
				"Lua",
				"Name",
				"(Hosting player name) or (Short name & Version)");

		for (const auto& p: ais) {
			const bool isLocal = (p.second.hostPlayer == gu->myPlayerNum);
			std::string lastPart;

			if (isLocal) {
				lastPart = "(Key:)  " + p.second.shortName + " " + p.second.version;
			} else {
				lastPart = "(Host:) " + playerHandler.Player(gu->myPlayerNum)->name;
			}

			LOG("%i | %i | %s | %s | %s | %s",
					p.first,
					p.second.team,
					(isLocal ? "yes" : "no "),
					(p.second.isLuaAI ? "yes" : "no "),
					p.second.name.c_str(),
					lastPart.c_str());
		}

		return true;
	}
};



class TeamActionExecutor : public IUnsyncedActionExecutor {
public:
	TeamActionExecutor() : IUnsyncedActionExecutor("Team", "Lets the local user change to another team", true) {
	}

	bool Execute(const UnsyncedAction& action) const {
		const int teamId = atoi(action.GetArgs().c_str());

		if (teamHandler.IsValidTeam(teamId)) {
			clientNet->Send(CBaseNetProtocol::Get().SendJoinTeam(gu->myPlayerNum, teamId));
		} else {
			LOG_L(L_WARNING, "[%s] team %d does not exist", __func__, teamId);
		}

		return true;
	}
};



class SpectatorActionExecutor : public IUnsyncedActionExecutor {
public:
	SpectatorActionExecutor() : IUnsyncedActionExecutor(
		"Spectator",
		"Lets the local user give up control over a team and start spectating"
	) {
	}

	bool Execute(const UnsyncedAction& action) const {
		if (gu->spectating)
			return false;

		clientNet->Send(CBaseNetProtocol::Get().SendResign(gu->myPlayerNum));
		return true;
	}
};



class SpecTeamActionExecutor : public IUnsyncedActionExecutor {
public:
	SpecTeamActionExecutor() : IUnsyncedActionExecutor(
		"SpecTeam",
		"Lets the local user specify the team to follow if he is a spectator"
	) {
	}

	bool Execute(const UnsyncedAction& action) const {
		if (!gu->spectating)
			return false;

		const int teamId = atoi(action.GetArgs().c_str());

		if (!teamHandler.IsValidTeam(teamId))
			return false;

		gu->myTeam = teamId;
		gu->myAllyTeam = teamHandler.AllyTeam(teamId);

		CLuaUI::UpdateTeams();
		return true;
	}
};



class SpecFullViewActionExecutor : public IUnsyncedActionExecutor {
public:
	SpecFullViewActionExecutor() : IUnsyncedActionExecutor(
		"SpecFullView",
		"Sets or toggles between full LOS or ally-team LOS if the local user is a spectator"
	) {
	}

	bool Execute(const UnsyncedAction& action) const {
		if (!gu->spectating)
			return false;

		if (!action.GetArgs().empty()) {
			const int mode = atoi(action.GetArgs().c_str());
			gu->spectatingFullView   = !!(mode & 1);
			gu->spectatingFullSelect = !!(mode & 2);
		} else {
			gu->spectatingFullView = !gu->spectatingFullView;
			gu->spectatingFullSelect = gu->spectatingFullView;
		}

		CLuaUI::UpdateTeams();

		// NOTE: unsynced event
		eventHandler.PlayerChanged(gu->myPlayerNum);
		return true;
	}
};



class AllyActionExecutor : public IUnsyncedActionExecutor {
public:
	AllyActionExecutor() : IUnsyncedActionExecutor(
		"Ally",
		"Starts/Ends alliance of the local player's ally-team with another ally-team"
	) {
	}

	bool Execute(const UnsyncedAction& action) const {
		if (gu->spectating)
			return false;

		if (action.GetArgs().size() > 0) {
			if (!gameSetup->fixedAllies) {
				std::istringstream is(action.GetArgs());
				int otherAllyTeam = -1;
				is >> otherAllyTeam;
				int state = -1;
				is >> state;

				if (state >= 0 && state < 2 && otherAllyTeam >= 0 && otherAllyTeam != gu->myAllyTeam)
					clientNet->Send(CBaseNetProtocol::Get().SendSetAllied(gu->myPlayerNum, otherAllyTeam, state));
				else
					LOG_L(L_WARNING, "/%s: wrong parameters (usage: /%s <other team> [0|1])", GetCommand().c_str(), GetCommand().c_str());
			} else {
				LOG_L(L_WARNING, "In-game alliances are not allowed");
			}
		} else {
			LOG_L(L_WARNING, "/%s: wrong parameters (usage: /%s <other team> [0|1])", GetCommand().c_str(), GetCommand().c_str());
		}

		return true;
	}
};



class GroupActionExecutor : public IUnsyncedActionExecutor {
public:
	GroupActionExecutor() : IUnsyncedActionExecutor("Group", "Allows modifying the members of a group") {
	}

	bool Execute(const UnsyncedAction& action) const {
		const auto& actionArgs = action.GetArgs();

		if ((actionArgs[0] >= '0') && (actionArgs[0] <= '9')) {
			const int teamId = int(actionArgs[0] - '0');
			const size_t firstCmdChar = actionArgs.find_first_not_of(" \t\n\r", 1);

			if (firstCmdChar != std::string::npos) {
				grouphandlers[gu->myTeam]->GroupCommand(teamId, actionArgs.substr(firstCmdChar));
			} else {
				LOG_L(L_WARNING, "/%s: wrong syntax", GetCommand().c_str());
			}
		} else {
			LOG_L(L_WARNING, "/%s: wrong groupid", GetCommand().c_str());
		}

		return true;
	}
};



class GroupIDActionExecutor : public IUnsyncedActionExecutor {
public:
	GroupIDActionExecutor(int _groupId): IUnsyncedActionExecutor(
		"Group" + IntToString(_groupId),
		"Allows modifying the members of group " + IntToString(_groupId)
	) {
		groupId = _groupId;
	}

	bool Execute(const UnsyncedAction& action) const {
		if (!action.IsRepeat())
			return grouphandlers[gu->myTeam]->GroupCommand(groupId);

		return false;
	}

private:
	int groupId;
};



class LastMessagePositionActionExecutor : public IUnsyncedActionExecutor {
public:
	LastMessagePositionActionExecutor() : IUnsyncedActionExecutor(
		"LastMsgPos",
		"Moves the camera to show the position of the last message"
	) {
	}

	bool Execute(const UnsyncedAction& action) const {
		if (infoConsole->GetMsgPosCount() == 0)
			return false;

		// cycle through the positions
		camHandler->CameraTransition(0.6f);
		camHandler->GetCurrentController().SetPos(infoConsole->GetMsgPos());
		return true;
	}
};



class ChatActionExecutor : public IUnsyncedActionExecutor {

	ChatActionExecutor(
		const std::string& _commandPostfix,
		const std::string& _userInputPrefix,
		bool _setUserInputPrefix
	): IUnsyncedActionExecutor(
		"Chat" + _commandPostfix,
		"Starts waiting for intput to be sent to " + _commandPostfix
	),
		userInputPrefix(_userInputPrefix),
		setUserInputPrefix(_setUserInputPrefix)
	{}

public:
	bool Execute(const UnsyncedAction& action) const {
		SDL_StartTextInput();

		gameTextInput.PromptInput(setUserInputPrefix? &userInputPrefix: nullptr);
		gameConsoleHistory.ResetPosition();
		inMapDrawer->SetDrawMode(false);
		return true;
	}

	static void RegisterCommandVariants() {
		unsyncedGameCommands->AddActionExecutor(new ChatActionExecutor("",     "",   false));
		unsyncedGameCommands->AddActionExecutor(new ChatActionExecutor("All",  "",   true));
		unsyncedGameCommands->AddActionExecutor(new ChatActionExecutor("Ally", "a:", true));
		unsyncedGameCommands->AddActionExecutor(new ChatActionExecutor("Spec", "s:", true));
	}

private:
	const std::string userInputPrefix;
	const bool setUserInputPrefix;
};



// TODO merge together with "TrackOff" to "Track 0|1", and deprecate the two old ones
class TrackActionExecutor : public IUnsyncedActionExecutor {
public:
	TrackActionExecutor() : IUnsyncedActionExecutor("Track", "Start following the selected unit(s) with the camera") {
	}

	bool Execute(const UnsyncedAction& action) const {
		unitTracker.Track();
		return true;
	}
};

class TrackOffActionExecutor : public IUnsyncedActionExecutor {
public:
	TrackOffActionExecutor() : IUnsyncedActionExecutor("TrackOff", "Stop following the selected unit(s) with the camera") {
	}

	bool Execute(const UnsyncedAction& action) const {
		unitTracker.Disable();
		return true;
	}
};

class TrackModeActionExecutor : public IUnsyncedActionExecutor {
public:
	TrackModeActionExecutor() : IUnsyncedActionExecutor("TrackMode", "Cycle through different following modes") {
	}

	bool Execute(const UnsyncedAction& action) const {
		unitTracker.IncMode();
		return true;
	}
};



class PauseActionExecutor : public IUnsyncedActionExecutor {
public:
	PauseActionExecutor() : IUnsyncedActionExecutor("Pause", "Pause/Unpause the game") {
	}

	bool Execute(const UnsyncedAction& action) const {
		// disallow pausing prior to start of game proper
		if (!game->playing)
			return false;

		// do not need to update lastReadNetTime, gets
		// done when NETMSG_PAUSE makes the round-trip
		bool newPause = gs->paused;
		InverseOrSetBool(newPause, action.GetArgs());
		clientNet->Send(CBaseNetProtocol::Get().SendPause(gu->myPlayerNum, newPause));
		return true;
	}

};



class DebugActionExecutor : public IUnsyncedActionExecutor {
public:
	DebugActionExecutor() : IUnsyncedActionExecutor("Debug", "Enable/Disable debug rendering mode") {
	}

	bool Execute(const UnsyncedAction& action) const {
		// toggle
		ProfileDrawer::SetEnabled(globalRendering->drawdebug = !globalRendering->drawdebug);
		profiler.SetEnabled(globalRendering->drawdebug);

		LogSystemStatus("debug-info rendering mode", globalRendering->drawdebug);
		return true;
	}
};

class DebugGLActionExecutor : public IUnsyncedActionExecutor {
public:
	DebugGLActionExecutor() : IUnsyncedActionExecutor("DebugGL", "Enable/Disable OpenGL debug-context output") {
	}

	bool Execute(const UnsyncedAction& action) const {
		// append zeros so all args can be safely omitted
		std::istringstream buf(action.GetArgs() + " 0 0 0");

		unsigned int msgSrceIdx = 0;
		unsigned int msgTypeIdx = 0;
		unsigned int msgSevrIdx = 0;

		buf >> msgSrceIdx;
		buf >> msgTypeIdx;
		buf >> msgSevrIdx;

		globalRendering->ToggleGLDebugOutput(msgSrceIdx, msgTypeIdx, msgSevrIdx);
		return true;
	}
};

class DebugGLErrorsActionExecutor : public IUnsyncedActionExecutor {
public:
	DebugGLErrorsActionExecutor() : IUnsyncedActionExecutor("DebugGLErrors", "Enable/Disable OpenGL debug-errors") {
	}

	bool Execute(const UnsyncedAction& action) const {
		LogSystemStatus("GL debug-errors", globalRendering->glDebugErrors = !globalRendering->glDebugErrors);
		return true;
	}
};


class MuteActionExecutor : public IUnsyncedActionExecutor {
public:
	MuteActionExecutor() : IUnsyncedActionExecutor("MuteSound", "Mute/Unmute the current sound system") {
	}

	bool Execute(const UnsyncedAction& action) const {
		// toggle
		sound->Mute();
		LogSystemStatus("Mute", sound->IsMuted());
		return true;
	}
};

class SoundActionExecutor : public IUnsyncedActionExecutor {
public:
	SoundActionExecutor() : IUnsyncedActionExecutor("SoundDevice", "Switch the sound output system (currently only OpenAL / NullAudio)") {
	}

	bool Execute(const UnsyncedAction& action) const {
		// toggle
		LogSystemStatus("Sound", !sound->ChangeOutput());
		return true;
	}
};




class SoundChannelEnableActionExecutor : public IUnsyncedActionExecutor {
public:
	SoundChannelEnableActionExecutor() : IUnsyncedActionExecutor(
		"SoundChannelEnable",
		"Enable/Disable specific sound channels: UnitReply, General, Battle, UserInterface, Music"
	) {
	}

	bool Execute(const UnsyncedAction& action) const {
		std::string channel;
		std::istringstream buf(action.GetArgs());
		int enable;
		buf >> channel;
		buf >> enable;

		switch (hashString(channel.c_str())) {
			case hashString("UnitReply"): {
				Channels::UnitReply->Enable(enable != 0);
			} break;
			case hashString("General"): {
				Channels::General->Enable(enable != 0);
			} break;
			case hashString("Battle"): {
				Channels::Battle->Enable(enable != 0);
			} break;
			case hashString("UserInterface"): {
				Channels::UserInterface->Enable(enable != 0);
			} break;
			case hashString("Music"): {
				Channels::BGMusic->Enable(enable != 0);
			} break;
			default: {
				LOG_L(L_WARNING, "/%s: wrong channel name \"%s\"", GetCommand().c_str(), channel.c_str());
			} break;
		}

		return true;
	}
};



class CreateVideoActionExecutor : public IUnsyncedActionExecutor {
public:
	CreateVideoActionExecutor() : IUnsyncedActionExecutor("CreateVideo", "Start/Stop capturing a video of the game in progress") {
	}

	bool Execute(const UnsyncedAction& action) const {
		// toggle
		videoCapturing->SetCapturing(!videoCapturing->IsCapturing());
		LogSystemStatus("Video capturing", videoCapturing->IsCapturing());
		return true;
	}
};



class DrawGrassActionExecutor : public IUnsyncedActionExecutor {
public:
	DrawGrassActionExecutor() : IUnsyncedActionExecutor("DrawGrass", "Enable/Disable grass rendering") {
	}

	bool Execute(const UnsyncedAction& action) const {
		const char* fmt = "{engine, Lua} grass rendering {%s, %s}";
		const char* strs[] = {"disabled", "enabled"};

		grassDrawer->HandleAction((action.GetArgs()).empty()? -1: atoi((action.GetArgs()).c_str()));
		LOG(fmt, strs[grassDrawer->DefDrawGrass()], strs[grassDrawer->LuaDrawGrass()]);
		return true;
	}
};

class DrawTreesActionExecutor : public IUnsyncedActionExecutor {
public:
	DrawTreesActionExecutor() : IUnsyncedActionExecutor("DrawTrees", "Enable/Disable tree rendering") {
	}

	bool Execute(const UnsyncedAction& action) const {
		const char* fmt = "{engine, Lua} tree rendering {%s, %s}";
		const char* strs[] = {"disabled", "enabled"};

		treeDrawer->HandleAction((action.GetArgs()).empty()? -1: atoi((action.GetArgs()).c_str()));
		LOG(fmt, strs[treeDrawer->DefDrawTrees()], strs[treeDrawer->LuaDrawTrees()]);
		return true;
	}
};



class NetPingActionExecutor : public IUnsyncedActionExecutor {
public:
	NetPingActionExecutor() : IUnsyncedActionExecutor("NetPing", "Send a ping request to the server") {
	}

	bool Execute(const UnsyncedAction& action) const {
		game->QueuePing(); // tell ClientReadNet to expect a ping
		clientNet->Send(CBaseNetProtocol::Get().SendPing(gu->myPlayerNum, spring_tomsecs(spring_now())));
		return true;
	}
};

class NetMsgSmoothingActionExecutor : public IUnsyncedActionExecutor {
public:
	NetMsgSmoothingActionExecutor() : IUnsyncedActionExecutor(
		"NetMsgSmoothing",
		"Toggles whether client will use net-message smoothing; better for unstable connections"
	) {
	}

	bool Execute(const UnsyncedAction& action) const {
		const char* fmt = "net-message smoothing %s";
		const char* strs[] = {"disabled", "enabled"};

		LOG(fmt, strs[globalConfig->useNetMessageSmoothingBuffer = !globalConfig->useNetMessageSmoothingBuffer]);
		return true;
	}
};

class SpeedControlActionExecutor : public IUnsyncedActionExecutor {
public:
	SpeedControlActionExecutor() : IUnsyncedActionExecutor(
		"SpeedControl",
		"Sets how server adjusts speed according to player's CPU load, 1: use average, 2: use highest"
	) {
	}

	bool Execute(const UnsyncedAction& action) const {
		if (gameServer == nullptr)
			return false;

		int speedCtrl = game->speedControl;

		if (action.GetArgs().empty()) {
			// switch to next value
			speedCtrl = mix(1, 2, speedCtrl == 1);
		} else {
			// set value
			speedCtrl = Clamp(atoi(action.GetArgs().c_str()), 1, 2);
		}

		// constrain to bounds
		gameServer->UpdateSpeedControl(game->speedControl = speedCtrl);
		return true;
	}
};



class GameInfoActionExecutor : public IUnsyncedActionExecutor {
public:
	GameInfoActionExecutor() : IUnsyncedActionExecutor("GameInfo", "Enables/Disables game-info panel rendering") {
	}

	bool Execute(const UnsyncedAction& action) const {
		if (!action.IsRepeat()) {
			if (!CGameInfo::IsActive()) {
				CGameInfo::Enable();
			} else {
				CGameInfo::Disable();
			}
		}

		return true;
	}
};



class HideInterfaceActionExecutor : public IUnsyncedActionExecutor {
public:
	HideInterfaceActionExecutor() : IUnsyncedActionExecutor("HideInterface", "Hide/Show the GUI controlls") {
	}

	bool Execute(const UnsyncedAction& action) const {
		InverseOrSetBool(game->hideInterface, action.GetArgs());
		return true;
	}
};



class HardwareCursorActionExecutor : public IUnsyncedActionExecutor {
public:
	HardwareCursorActionExecutor() : IUnsyncedActionExecutor("HardwareCursor", "Enables/Disables hardware mouse-cursor support") {
	}

	bool Execute(const UnsyncedAction& action) const {
		const bool enable = (atoi(action.GetArgs().c_str()) != 0);
		mouse->ToggleHwCursor(enable);
		configHandler->Set("HardwareCursor", enable);
		LogSystemStatus("Hardware mouse-cursor", enable);
		return true;
	}
};



class FullscreenActionExecutor : public IUnsyncedActionExecutor {
public:
	FullscreenActionExecutor() : IUnsyncedActionExecutor("Fullscreen", "Switches fullscreen mode") {
	}

	bool Execute(const UnsyncedAction& action) const {
		if (!action.GetArgs().empty()) {
			configHandler->Set("Fullscreen", (atoi(action.GetArgs().c_str()) != 0));
		} else {
			configHandler->Set("Fullscreen", !globalRendering->fullScreen);
		}

		return true;
	}
};



class IncreaseViewRadiusActionExecutor : public IUnsyncedActionExecutor {
public:
	IncreaseViewRadiusActionExecutor() : IUnsyncedActionExecutor("IncreaseViewRadius", "Increase terrain tessellation level") {
	}

	bool Execute(const UnsyncedAction& action) const {
		readMap->GetGroundDrawer()->IncreaseDetail();
		return true;
	}
};

class DecreaseViewRadiusActionExecutor : public IUnsyncedActionExecutor {
public:
	DecreaseViewRadiusActionExecutor() : IUnsyncedActionExecutor("DecreaseViewRadius", "Decrease terrain tessellation level") {
	}

	bool Execute(const UnsyncedAction& action) const {
		readMap->GetGroundDrawer()->DecreaseDetail();
		return true;
	}
};



class GroundDetailActionExecutor : public IUnsyncedActionExecutor {
public:
	GroundDetailActionExecutor() : IUnsyncedActionExecutor("GroundDetail", "Set the terrain tessellation level") {
	}

	bool Execute(const UnsyncedAction& action) const {
		if (action.GetArgs().empty()) {
			LOG_L(L_WARNING, "/%s: missing argument", GetCommand().c_str());
			return false;
		}

		readMap->GetGroundDrawer()->SetDetail(atoi((action.GetArgs()).c_str()));
		return true;
	}

};



class MoreGrassActionExecutor : public IUnsyncedActionExecutor {
public:
	MoreGrassActionExecutor() : IUnsyncedActionExecutor("MoreGrass", "Increases distance from the camera at which grass is still drawn") {
	}

	bool Execute(const UnsyncedAction& action) const {
		LOG("grass draw-distance increased to %f", grassDrawer->IncrDrawDistance());
		return true;
	}
};

class LessGrassActionExecutor : public IUnsyncedActionExecutor {
public:
	LessGrassActionExecutor() : IUnsyncedActionExecutor("LessGrass", "Decreases distance from the camera at which grass are still drawn") {
	}

	bool Execute(const UnsyncedAction& action) const {
		LOG("grass draw-distance decreased to %f", grassDrawer->DecrDrawDistance());
		return true;
	}
};


class MoreTreesActionExecutor : public IUnsyncedActionExecutor {
public:
	MoreTreesActionExecutor() : IUnsyncedActionExecutor("MoreTrees", "Increases distance from the camera at which trees are still drawn") {
	}

	bool Execute(const UnsyncedAction& action) const {
		LOG("tree draw-distance increased to %f", treeDrawer->IncrDrawDistance());
		return true;
	}
};

class LessTreesActionExecutor : public IUnsyncedActionExecutor {
public:
	LessTreesActionExecutor() : IUnsyncedActionExecutor("LessTrees", "Decreases distance from the camera at which trees are still drawn") {
	}

	bool Execute(const UnsyncedAction& action) const {
		LOG("tree draw-distance decreased to %f", treeDrawer->DecrDrawDistance());
		return true;
	}
};



class SpeedUpActionExecutor : public IUnsyncedActionExecutor {
public:
	SpeedUpActionExecutor() : IUnsyncedActionExecutor(
		"SpeedUp",
		"Increases the simulation speed. The engine will try to simulate more frames per second"
	) {
	}

	bool Execute(const UnsyncedAction& action) const {
		int index = 0;

		float speed = gs->wantedSpeedFactor;
		float fract = 0.0f;

		// [0,2], [2,5], [5,10], [5,inf]
		constexpr float range[] = {2.0f, 5.0f, 10.0f, std::numeric_limits<float>::max()};
		constexpr float steps[] = {0.1f, 0.2f, 0.5f, 1.0f};

		while (index < 4 && speed >= range[index])
			index++;

		speed += steps[index];
		fract  = speed - std::floor(speed);

		clientNet->Send(CBaseNetProtocol::Get().SendUserSpeed(gu->myPlayerNum, mix(speed, std::round(speed), fract < 0.01f || fract > 0.99f)));
		return true;
	}
};



class SlowDownActionExecutor : public IUnsyncedActionExecutor {
public:
	SlowDownActionExecutor() : IUnsyncedActionExecutor(
		"SlowDown",
		"Decreases the simulation speed. The engine will try to simulate less frames per second"
	) {
	}

	bool Execute(const UnsyncedAction& action) const {
		int index = 0;

		float speed = gs->wantedSpeedFactor;
		float fract = 0.0f;

		constexpr float range[] = {2.0f, 5.0f, 10.0f, std::numeric_limits<float>::max()};
		constexpr float steps[] = {0.1f, 0.2f, 0.5f, 1.0f};

		while (index < 4 && speed > range[index])
			index++;

		speed -= steps[index];
		fract  = speed - std::floor(speed);

		clientNet->Send(CBaseNetProtocol::Get().SendUserSpeed(gu->myPlayerNum, mix(speed, std::round(speed), fract < 0.01f || fract > 0.99f)));
		return true;
	}
};



class ControlUnitActionExecutor : public IUnsyncedActionExecutor {
public:
	ControlUnitActionExecutor() : IUnsyncedActionExecutor("ControlUnit", "Start to first-person-control a unit") {
	}

	bool Execute(const UnsyncedAction& action) const {
		if (gu->spectating)
			return false;

		// we must cause the to-be-controllee to be put in
		// netSelected[myPlayerNum] by giving it an order
		selectedUnitsHandler.SendCommand(Command(CMD_STOP));
		clientNet->Send(CBaseNetProtocol::Get().SendDirectControl(gu->myPlayerNum));
		return true;
	}
};



class ShowStandardActionExecutor : public IUnsyncedActionExecutor {
public:
	ShowStandardActionExecutor() : IUnsyncedActionExecutor("ShowStandard", "Disable rendering of all auxiliary map overlays") {
	}

	bool Execute(const UnsyncedAction& action) const {
		infoTextureHandler->SetMode("");
		return true;
	}
};

class ShowElevationActionExecutor : public IUnsyncedActionExecutor {
public:
	ShowElevationActionExecutor() : IUnsyncedActionExecutor("ShowElevation", "Enable rendering of the auxiliary height-map overlay") {
	}

	bool Execute(const UnsyncedAction& action) const {
		infoTextureHandler->ToggleMode("height");
		return true;
	}
};

class ShowMetalMapActionExecutor : public IUnsyncedActionExecutor {
public:
	ShowMetalMapActionExecutor() : IUnsyncedActionExecutor("ShowMetalMap", "Enable rendering of the auxiliary metal-map overlay") {
	}

	bool Execute(const UnsyncedAction& action) const {
		infoTextureHandler->ToggleMode("metal");
		return true;
	}
};

class ShowPathTravActionExecutor : public IUnsyncedActionExecutor {
public:
	ShowPathTravActionExecutor() : IUnsyncedActionExecutor("ShowPathTraversability", "Enable rendering of the path traversability-map overlay") {
	}

	bool Execute(const UnsyncedAction& action) const {
		CPathTexture* pathTexInfo = dynamic_cast<CPathTexture*>(infoTextureHandler->GetInfoTexture("path"));

		if (pathTexInfo != nullptr)
			pathTexInfo->ShowMoveDef(-1);

		infoTextureHandler->ToggleMode("path");
		return true;
	}
};

class ShowPathHeatActionExecutor : public IUnsyncedActionExecutor {
public:
	ShowPathHeatActionExecutor() : IUnsyncedActionExecutor("ShowPathHeat", "Enable/Disable rendering of the path heat-map overlay", true) {
	}

	bool Execute(const UnsyncedAction& action) const {
		infoTextureHandler->ToggleMode("heat");
		return true;
	}
};

class ShowPathFlowActionExecutor : public IUnsyncedActionExecutor {
public:
	ShowPathFlowActionExecutor() : IUnsyncedActionExecutor("ShowPathFlow", "Enable/Disable rendering of the path flow-map overlay", true) {
	}

	bool Execute(const UnsyncedAction& action) const {
		infoTextureHandler->ToggleMode("flow");
		return true;
	}
};

class ShowPathCostActionExecutor : public IUnsyncedActionExecutor {
public:
	ShowPathCostActionExecutor() : IUnsyncedActionExecutor("ShowPathCost", "Enable rendering of the path cost-map overlay", true) {
	}

	bool Execute(const UnsyncedAction& action) const {
		infoTextureHandler->ToggleMode("pathcost");
		return true;
	}
};

class ToggleLOSActionExecutor : public IUnsyncedActionExecutor {
public:
	ToggleLOSActionExecutor() : IUnsyncedActionExecutor("ToggleLOS", "Enable rendering of the auxiliary LOS-map overlay") {
	}

	bool Execute(const UnsyncedAction& action) const {
		infoTextureHandler->ToggleMode("los");
		return true;
	}
};

class ToggleInfoActionExecutor : public IUnsyncedActionExecutor {
public:
	ToggleInfoActionExecutor() : IUnsyncedActionExecutor("ToggleInfo", "Toggles current info texture view") {
	}

	bool Execute(const UnsyncedAction& action) const {
		infoTextureHandler->ToggleMode(action.GetArgs());
		return true;
	}
};


class ShowPathTypeActionExecutor : public IUnsyncedActionExecutor {
public:
	ShowPathTypeActionExecutor() : IUnsyncedActionExecutor(
		"ShowPathType",
		"Shows path traversability for a given MoveDefName, MoveDefID or UnitDefName"
	) {
	}

	bool Execute(const UnsyncedAction& action) const {
		CPathTexture* pathTexInfo = dynamic_cast<CPathTexture*>(infoTextureHandler->GetInfoTexture("path"));

		if (pathTexInfo != nullptr) {
			bool shown = false;
			bool failed = false;

			if (!action.GetArgs().empty()) {
				unsigned int i = StringToInt(action.GetArgs(), &failed);

				if (failed)
					i = -1;

				const MoveDef* md = moveDefHandler.GetMoveDefByName(action.GetArgs());
				const UnitDef* ud = unitDefHandler->GetUnitDefByName(action.GetArgs());

				if (md == nullptr && i < moveDefHandler.GetNumMoveDefs())
					md = moveDefHandler.GetMoveDefByPathType(i);

				shown = (md != nullptr || ud != nullptr);

				if (md != nullptr) {
					pathTexInfo->ShowMoveDef(md->pathType);
					LOG("Showing PathView for MoveDef: %s", md->name.c_str());
				} else {
					if (ud != nullptr) {
						pathTexInfo->ShowUnitDef(ud->id);
						LOG("Showing BuildView for UnitDef: %s", ud->name.c_str());
					}
				}
			}

			if (!shown) {
				pathTexInfo->ShowMoveDef(-1);
				LOG("Switching back to automatic PathView");
			} else if (infoTextureHandler->GetMode() != "path") {
				infoTextureHandler->ToggleMode("path");
			}
		}

		return true;
	}
};



class ShareDialogActionExecutor : public IUnsyncedActionExecutor {
public:
	ShareDialogActionExecutor() : IUnsyncedActionExecutor(
		"ShareDialog",
		"Opens the share dialog for sending units and resources to other players"
	) {
	}

	bool Execute(const UnsyncedAction& action) const {
		if (gu->spectating)
			return false;

		// already shown?
		const auto& inputReceivers = CInputReceiver::GetReceivers();

		if (inputReceivers.empty() || (dynamic_cast<CShareBox*>(inputReceivers.front()) != nullptr))
			return false;

		new CShareBox();
		return true;
	}
};



class QuitMessageActionExecutor : public IUnsyncedActionExecutor {
public:
	QuitMessageActionExecutor() : IUnsyncedActionExecutor(
		"QuitMessage",
		"Deprecated, see /Quit instead (was used to quit the game immediately)"
	) {
	}

	bool Execute(const UnsyncedAction& action) const {
		// already shown?
		const auto& inputReceivers = CInputReceiver::GetReceivers();

		if (inputReceivers.empty() || dynamic_cast<CQuitBox*>(inputReceivers.front()) != nullptr)
			return false;

		const CKeyBindings::HotkeyList& quitList = keyBindings->GetHotkeys("quitmenu");
		const std::string& quitKey = quitList.empty() ? "<none>" : *quitList.begin();

		LOG("Press %s to access the quit menu", quitKey.c_str());
		return true;
	}
};



class QuitMenuActionExecutor : public IUnsyncedActionExecutor {
public:
	QuitMenuActionExecutor() : IUnsyncedActionExecutor("QuitMenu", "Opens the quit-menu, if it is not already open") {
	}

	bool Execute(const UnsyncedAction& action) const {
		// already shown?
		const auto& inputReceivers = CInputReceiver::GetReceivers();

		if (inputReceivers.empty() || dynamic_cast<CQuitBox*>(inputReceivers.front()) != nullptr)
			return false;

		new CQuitBox();
		return true;
	}
};



class QuitActionExecutor : public IUnsyncedActionExecutor {
public:
	QuitActionExecutor() : IUnsyncedActionExecutor("QuitForce", "Exits game to system") {
	}

	bool Execute(const UnsyncedAction& action) const {
		LOG("[QuitAction] user exited to system");

		gu->globalQuit = true;
		return true;
	}
};

class ReloadActionExecutor : public IUnsyncedActionExecutor {
public:
	ReloadActionExecutor() : IUnsyncedActionExecutor("ReloadForce", "Exits game to menu") {
	}

	bool Execute(const UnsyncedAction& action) const {
		LOG("[ReloadAction] user exited to menu");

		gameSetup->reloadScript = "";
		gu->globalReload = true;
		return true;
	}
};



class IncreaseGUIOpacityActionExecutor : public IUnsyncedActionExecutor {
public:
	IncreaseGUIOpacityActionExecutor() : IUnsyncedActionExecutor("IncGUIOpacity", "Increases the opacity of GUI elements") {
	}

	bool Execute(const UnsyncedAction& action) const {
		configHandler->Set("GuiOpacity", CInputReceiver::guiAlpha = std::min(CInputReceiver::guiAlpha + 0.1f, 1.0f));
		return true;
	}
};

class DecreaseGUIOpacityActionExecutor : public IUnsyncedActionExecutor {
public:
	DecreaseGUIOpacityActionExecutor() : IUnsyncedActionExecutor("DecGUIOpacity", "Decreases the topacity of GUI elements") {
	}

	bool Execute(const UnsyncedAction& action) const {
		configHandler->Set("GuiOpacity", CInputReceiver::guiAlpha = std::max(CInputReceiver::guiAlpha - 0.1f, 0.0f));
		return true;
	}
};



class ScreenShotActionExecutor : public IUnsyncedActionExecutor {
public:
	ScreenShotActionExecutor() : IUnsyncedActionExecutor("ScreenShot", "Take a screen-shot of the current view") {
	}

	bool Execute(const UnsyncedAction& action) const {
		TakeScreenshot(action.GetArgs());
		return true;
	}
};



class GrabInputActionExecutor : public IUnsyncedActionExecutor {
public:
	GrabInputActionExecutor() : IUnsyncedActionExecutor("GrabInput", "Prevents/Enables the mouse from leaving the game window (windowed mode only)") {
	}

	bool Execute(const UnsyncedAction& action) const {
		const std::string& args = action.GetArgs();

		if (args.empty()) {
			LogSystemStatus("Input grabbing", globalRendering->ToggleWindowInputGrabbing());
		} else {
			LogSystemStatus("Input grabbing", globalRendering->SetWindowInputGrabbing(atoi(args.c_str())));
		}

		return true;
	}
};



class ClockActionExecutor : public IUnsyncedActionExecutor {
public:
	ClockActionExecutor() : IUnsyncedActionExecutor("Clock", "Shows a small digital clock indicating the local time") {
	}

	bool Execute(const UnsyncedAction& action) const {
		InverseOrSetBool(game->showClock, action.GetArgs());
		configHandler->Set("ShowClock", game->showClock ? 1 : 0);
		LogSystemStatus("small digital clock", game->showClock);
		return true;
	}
};



class CrossActionExecutor : public IUnsyncedActionExecutor {
public:
	CrossActionExecutor() : IUnsyncedActionExecutor(
		"Cross",
		"Allows one to exchange and modify the appearance of the"
		" cross/mouse-pointer in first-person-control view"
	) {
	}

	bool Execute(const UnsyncedAction& action) const {
		if (action.GetArgs().empty()) {
			if (mouse->crossSize > 0.0f) {
				mouse->crossSize = -mouse->crossSize;
			} else {
				mouse->crossSize = std::max(1.0f, -mouse->crossSize);
			}
		} else {
			float size;
			float alpha;
			float scale;

			const char* args = action.GetArgs().c_str();
			const int argcount = sscanf(args, "%f %f %f", &size, &alpha, &scale);

			if (argcount > 1)
				configHandler->Set("CrossAlpha", mouse->crossAlpha = alpha);
			if (argcount > 2)
				configHandler->Set("CrossMoveScale", mouse->crossMoveScale = scale);

			mouse->crossSize = size;
		}

		configHandler->Set("CrossSize", mouse->crossSize);
		return true;
	}
};



class FPSActionExecutor : public IUnsyncedActionExecutor {
public:
	FPSActionExecutor() : IUnsyncedActionExecutor("FPS", "Shows/Hides the frames-per-second indicator") {
	}

	bool Execute(const UnsyncedAction& action) const {
		InverseOrSetBool(game->showFPS, action.GetArgs());
		configHandler->Set("ShowFPS", game->showFPS ? 1 : 0);
		LogSystemStatus("frames-per-second indicator", game->showFPS);
		return true;
	}
};



class SpeedActionExecutor : public IUnsyncedActionExecutor {
public:
	SpeedActionExecutor() : IUnsyncedActionExecutor("Speed", "Shows/Hides the simulation speed indicator") {
	}

	bool Execute(const UnsyncedAction& action) const {
		InverseOrSetBool(game->showSpeed, action.GetArgs());
		configHandler->Set("ShowSpeed", game->showSpeed ? 1 : 0);
		LogSystemStatus("simulation speed indicator", game->showSpeed);
		return true;
	}
};


class TeamHighlightActionExecutor : public IUnsyncedActionExecutor {
public:
	TeamHighlightActionExecutor() : IUnsyncedActionExecutor("TeamHighlight", "Enables/Disables uncontrolled team blinking") {
	}

	bool Execute(const UnsyncedAction& action) const {
		if (action.GetArgs().empty()) {
			globalConfig->teamHighlight = abs(globalConfig->teamHighlight + 1) % CTeamHighlight::HIGHLIGHT_SIZE;
		} else {
			globalConfig->teamHighlight = abs(atoi(action.GetArgs().c_str())) % CTeamHighlight::HIGHLIGHT_SIZE;
		}

		LOG("Team highlighting: %s",
				((globalConfig->teamHighlight == CTeamHighlight::HIGHLIGHT_PLAYERS) ? "Players only"
				: ((globalConfig->teamHighlight == CTeamHighlight::HIGHLIGHT_ALL) ? "Players and spectators"
				: "Disabled")));

		configHandler->Set("TeamHighlight", globalConfig->teamHighlight);
		return true;
	}
};



class InfoActionExecutor : public IUnsyncedActionExecutor {
public:
	InfoActionExecutor() : IUnsyncedActionExecutor("Info", "Shows/Hides the player roster") {
	}

	bool Execute(const UnsyncedAction& action) const {
		if (action.GetArgs().empty()) {
			if (playerRoster.GetSortType() == PlayerRoster::Disabled) {
				playerRoster.SetSortTypeByCode(PlayerRoster::Allies);
			} else {
				playerRoster.SetSortTypeByCode(PlayerRoster::Disabled);
			}
		} else {
			playerRoster.SetSortTypeByName(action.GetArgs());
		}

		if (playerRoster.GetSortType() != PlayerRoster::Disabled)
			LOG("Sorting roster by %s", playerRoster.GetSortName());

		configHandler->Set("ShowPlayerInfo", (int)playerRoster.GetSortType());
		return true;
	}
};



class CmdColorsActionExecutor : public IUnsyncedActionExecutor {
public:
	CmdColorsActionExecutor() : IUnsyncedActionExecutor("CmdColors", "Reloads cmdcolors.txt") {
	}

	bool Execute(const UnsyncedAction& action) const {
		const std::string& fileName = action.GetArgs().empty() ? "cmdcolors.txt" : action.GetArgs();
		cmdColors.LoadConfigFromFile(fileName);
		LOG("Reloaded cmdcolors from file: %s", fileName.c_str());
		return true;
	}
};



class CtrlPanelActionExecutor : public IUnsyncedActionExecutor {
public:
	CtrlPanelActionExecutor() : IUnsyncedActionExecutor("CtrlPanel", "Reloads GUI config") {
	}

	bool Execute(const UnsyncedAction& action) const {
		guihandler->ReloadConfigFromFile(action.GetArgs());
		return true;
	}
};



class FontActionExecutor : public IUnsyncedActionExecutor {
public:
	FontActionExecutor() : IUnsyncedActionExecutor("Font", "Reloads default or custom fonts") {
	}

	bool Execute(const UnsyncedAction& action) const {
		// FIXME: same file for both?
		CglFont::LoadCustomFonts(action.GetArgs(), action.GetArgs());
		return true;
	}
};



class VSyncActionExecutor : public IUnsyncedActionExecutor {
public:
	VSyncActionExecutor() : IUnsyncedActionExecutor("VSync", "Enables/Disables vertical-sync") {
	}

	bool Execute(const UnsyncedAction& action) const {
		if (action.GetArgs().empty()) {
			verticalSync->Toggle();
		} else {
			verticalSync->SetInterval(atoi(action.GetArgs().c_str()));
		}

		return true;
	}
};



class SafeGLActionExecutor : public IUnsyncedActionExecutor {
public:
	SafeGLActionExecutor() : IUnsyncedActionExecutor("SafeGL", "Enables/Disables LuaOpenGL safe-mode") {
	}

	bool Execute(const UnsyncedAction& action) const {
		bool safeMode = LuaOpenGL::GetSafeMode();
		InverseOrSetBool(safeMode, action.GetArgs());
		LuaOpenGL::SetSafeMode(safeMode);
		LogSystemStatus("LuaOpenGL safe-mode", LuaOpenGL::GetSafeMode());
		return true;
	}
};



class ResBarActionExecutor : public IUnsyncedActionExecutor {
public:
	ResBarActionExecutor() : IUnsyncedActionExecutor("ResBar", "Shows/Hides team resource storage indicator bar") {
	}

	bool Execute(const UnsyncedAction& action) const {
		if (resourceBar == nullptr)
			return false;

		InverseOrSetBool(resourceBar->enabled, action.GetArgs());
		return true;
	}
};



class ToolTipActionExecutor : public IUnsyncedActionExecutor {
public:
	ToolTipActionExecutor() : IUnsyncedActionExecutor(
		"ToolTip",
		"Enables/Disables the general tool-tips, displayed when hovering over units. features or the map"
	) {
	}

	bool Execute(const UnsyncedAction& action) const {
		if (tooltip == nullptr)
			return false;

		InverseOrSetBool(tooltip->enabled, action.GetArgs());
		return true;
	}
};



class ConsoleActionExecutor : public IUnsyncedActionExecutor {
public:
	ConsoleActionExecutor() : IUnsyncedActionExecutor("Console", "Enables/Disables the in-game console") {
	}

	bool Execute(const UnsyncedAction& action) const {
		if (infoConsole == nullptr)
			return false;

		InverseOrSetBool(infoConsole->enabled, action.GetArgs());
		return true;
	}
};



class EndGraphActionExecutor : public IUnsyncedActionExecutor {
public:
	EndGraphActionExecutor() : IUnsyncedActionExecutor("EndGraph", "Enables/Disables the statistics graphs shown at the end of the game") {
	}

	bool Execute(const UnsyncedAction& action) const {
		InverseOrSetBool(CEndGameBox::enabled, action.GetArgs());
		return true;
	}
};



class FPSHudActionExecutor : public IUnsyncedActionExecutor {
public:
	FPSHudActionExecutor() : IUnsyncedActionExecutor("FPSHud", "Enables/Disables HUD (GUI interface) shown in first-person-control mode") {
	}

	bool Execute(const UnsyncedAction& action) const {
		bool drawHUD = hudDrawer->GetDraw();
		InverseOrSetBool(drawHUD, action.GetArgs());
		hudDrawer->SetDraw(drawHUD);
		return true;
	}
};



class DebugDrawAIActionExecutor : public IUnsyncedActionExecutor {
public:
	DebugDrawAIActionExecutor() : IUnsyncedActionExecutor("DebugDrawAI", "Enables/Disables debug drawing for AIs") {
	}

	bool Execute(const UnsyncedAction& action) const {
		bool aiDebugDraw = debugDrawerAI->IsEnabled();
		InverseOrSetBool(aiDebugDraw, action.GetArgs());
		debugDrawerAI->SetEnabled(aiDebugDraw);
		LogSystemStatus("SkirmishAI debug drawing", debugDrawerAI->IsEnabled());
		return true;
	}
};



class MapMarksActionExecutor : public IUnsyncedActionExecutor {
public:
	MapMarksActionExecutor() : IUnsyncedActionExecutor("MapMarks", "Enables/Disables map-marker rendering") {
	}

	bool Execute(const UnsyncedAction& action) const {
		InverseOrSetBool(globalRendering->drawMapMarks, action.GetArgs());
		LogSystemStatus("map-marker rendering", globalRendering->drawMapMarks);
		return true;
	}
};



class AllMapMarksActionExecutor : public IUnsyncedActionExecutor {
public:
	AllMapMarksActionExecutor() : IUnsyncedActionExecutor("AllMapMarks", "Show/Hide all map-markers drawn so far", true) {
	}

	bool Execute(const UnsyncedAction& action) const {
		bool allMarksVisible = inMapDrawerModel->GetAllMarksVisible();
		InverseOrSetBool(allMarksVisible, action.GetArgs());
		inMapDrawerModel->SetAllMarksVisible(allMarksVisible);
		return true;
	}
};



class ClearMapMarksActionExecutor : public IUnsyncedActionExecutor {
public:
	ClearMapMarksActionExecutor() : IUnsyncedActionExecutor("ClearMapMarks", "Remove all map-markers drawn so far") {
	}

	bool Execute(const UnsyncedAction& action) const {
		inMapDrawerModel->EraseAll();
		return true;
	}
};



// XXX unlucky command-name, remove the "No"
class NoLuaDrawActionExecutor : public IUnsyncedActionExecutor {
public:
	NoLuaDrawActionExecutor() : IUnsyncedActionExecutor("NoLuaDraw", "Allow/Disallow Lua to draw on the map") {
	}

	bool Execute(const UnsyncedAction& action) const {
		bool luaMapDrawingAllowed = inMapDrawer->GetLuaMapDrawingAllowed();
		InverseOrSetBool(luaMapDrawingAllowed, action.GetArgs());
		inMapDrawer->SetLuaMapDrawingAllowed(luaMapDrawingAllowed);
		return true;
	}
};



class LuaUIActionExecutor : public IUnsyncedActionExecutor {
public:
	LuaUIActionExecutor() : IUnsyncedActionExecutor(
		"LuaUI",
		"reload or disable LuaUI, or alternatively to send a chat message to LuaUI"
	) {
	}

	bool Execute(const UnsyncedAction& action) const {
		if (guihandler == nullptr)
			return false;

		const std::string& command = action.GetArgs();

		if (command == "reload" || command == "enable") {
			guihandler->EnableLuaUI(command == "enable");
			return true;
		}
		if (command == "disable") {
			guihandler->DisableLuaUI();
			return true;
		}
		if (luaUI != nullptr) {
			luaUI->GotChatMsg(command, 0);
			return true;
		}

		LOG_L(L_DEBUG, "LuaUI is not loaded");
		return true;
	}
};


class MiniMapActionExecutor : public IUnsyncedActionExecutor {
public:
	MiniMapActionExecutor() : IUnsyncedActionExecutor("MiniMap", "FIXME document subcommands") {
	}

	bool Execute(const UnsyncedAction& action) const {
		if (minimap == nullptr)
			return false;

		minimap->ConfigCommand(action.GetArgs());
		return true;
	}
};



class GroundDecalsActionExecutor : public IUnsyncedActionExecutor {
public:
	GroundDecalsActionExecutor() : IUnsyncedActionExecutor(
		"GroundDecals",
		"Enable/Disable ground-decal rendering."
	) {
	}

	bool Execute(const UnsyncedAction& action) const {
		bool drawDecals = IGroundDecalDrawer::GetDrawDecals();

		InverseOrSetBool(drawDecals, action.GetArgs());
		IGroundDecalDrawer::SetDrawDecals(drawDecals);

		LogSystemStatus("Ground-decal rendering", IGroundDecalDrawer::GetDrawDecals());
		return true;
	}
};



class DistSortProjectilesActionExecutor: public IUnsyncedActionExecutor {
public:
	DistSortProjectilesActionExecutor(): IUnsyncedActionExecutor(
		"DistSortProjectiles",
		"Enable/Disable sorting drawn projectiles by camera distance"
	) {
	}

	bool Execute(const UnsyncedAction& action) const {
		const auto& args = action.GetArgs();

		const char* fmt = "ProjectileDrawer distance-sorting %s";
		const char* strs[] = {"disabled", "enabled"};

		if (!args.empty()) {
			LOG(fmt, strs[projectileDrawer->EnableSorting(atoi(args.c_str()))]);
		} else {
			LOG(fmt, strs[projectileDrawer->ToggleSorting()]);
		}

		return true;
	}
};

class MaxParticlesActionExecutor : public IUnsyncedActionExecutor {
public:
	MaxParticlesActionExecutor() : IUnsyncedActionExecutor(
		"MaxParticles",
		"Set the maximum number of particles (Graphics setting)"
	) {
	}

	bool Execute(const UnsyncedAction& action) const {
		const auto& args = action.GetArgs();

		if (!args.empty()) {
			const int value = std::max(1, atoi(args.c_str()));
			projectileHandler.SetMaxParticles(value);
			LOG("Set maximum particles to: %i", value);
		} else {
			LOG_L(L_WARNING, "/%s: wrong syntax", GetCommand().c_str());
		}

		return true;
	}
};

class MaxNanoParticlesActionExecutor : public IUnsyncedActionExecutor {
public:
	MaxNanoParticlesActionExecutor() : IUnsyncedActionExecutor(
		"MaxNanoParticles",
		"Set the maximum number of nano-particles (Graphic setting)"
	) {
	}

	bool Execute(const UnsyncedAction& action) const {
		const auto& args = action.GetArgs();

		if (!args.empty()) {
			const int value = std::max(1, atoi(args.c_str()));
			projectileHandler.SetMaxNanoParticles(value);
			LOG("Set maximum nano-particles to: %i", value);
		} else {
			LOG_L(L_WARNING, "/%s: wrong syntax", GetCommand().c_str());
		}

		return true;
	}
};



class GatherModeActionExecutor : public IUnsyncedActionExecutor {
public:
	GatherModeActionExecutor() : IUnsyncedActionExecutor("GatherMode", "Enter/Leave gather-wait command mode") {
	}

	bool Execute(const UnsyncedAction& action) const {
		if (guihandler == nullptr)
			return false;

		bool gatherMode = guihandler->GetGatherMode();
		InverseOrSetBool(gatherMode, action.GetArgs());
		guihandler->SetGatherMode(gatherMode);
		LogSystemStatus("Gather-Mode", guihandler->GetGatherMode());
		return true;
	}
};



class PasteTextActionExecutor : public IUnsyncedActionExecutor {
public:
	PasteTextActionExecutor() : IUnsyncedActionExecutor(
		"PasteText",
		"Paste either the argument string(s) or the content of the clip-board to chat input"
	) {
	}

	bool Execute(const UnsyncedAction& action) const {
		return (gameTextInput.CheckHandlePasteCommand(action.GetInnerAction().rawline));
	}
};

class BufferTextActionExecutor : public IUnsyncedActionExecutor {
public:
	BufferTextActionExecutor() : IUnsyncedActionExecutor("BufferText", "Write the argument string(s) directly to the console history") {
	}

	bool Execute(const UnsyncedAction& action) const {
		// we cannot use extra commands because tokenization strips multiple
		// spaces or even trailing spaces, the text should be copied verbatim
		const std::string bufferCmd = "buffertext ";
		const std::string& rawLine = action.GetInnerAction().rawline;

		if (rawLine.length() > bufferCmd.length() ) {
			gameConsoleHistory.AddLine(rawLine.substr(bufferCmd.length(), rawLine.length() - bufferCmd.length()));
		} else {
			LOG_L(L_WARNING, "/%s: wrong syntax", GetCommand().c_str());
		}

		return true;
	}
};



class InputTextGeoActionExecutor : public IUnsyncedActionExecutor {
public:
	InputTextGeoActionExecutor() : IUnsyncedActionExecutor(
		"InputTextGeo",
		"Move and/or resize the input-text field (the \"Say: \" thing)"
	) {
	}

	bool Execute(const UnsyncedAction& action) const {
		if (!action.GetArgs().empty()) {
			game->ParseInputTextGeometry(action.GetArgs());
		} else {
			LOG_L(L_WARNING, "/%s: wrong syntax", GetCommand().c_str());
		}

		return true;
	}
};



class DistIconActionExecutor : public IUnsyncedActionExecutor {
public:
	DistIconActionExecutor() : IUnsyncedActionExecutor(
		"DistIcon",
		"Set the distance between units and camera at which they are rendered as icons"
	) {
	}

	bool Execute(const UnsyncedAction& action) const {
		if (!action.GetArgs().empty()) {
			const int iconDist = atoi(action.GetArgs().c_str());

			unitDrawer->SetUnitIconDist(iconDist * 1.0f);
			configHandler->Set("UnitIconDist", iconDist);
			LOG("Set UnitIconDist to %i", iconDist);
		} else {
			LOG_L(L_WARNING, "/%s: wrong syntax", GetCommand().c_str());
		}

		return true;
	}
};

class DistDrawActionExecutor : public IUnsyncedActionExecutor {
public:
	DistDrawActionExecutor() : IUnsyncedActionExecutor(
		"DistDraw",
		"Set the distance between units and camera at which they are rendered as far-textures"
	) {
	}

	bool Execute(const UnsyncedAction& action) const {
		if (!action.GetArgs().empty()) {
			const int drawDist = atoi(action.GetArgs().c_str());

			unitDrawer->SetUnitDrawDist(drawDist * 1.0f);
			configHandler->Set("UnitLodDist", drawDist);
			LOG("Set UnitLodDist to %i", drawDist);
		} else {
			LOG_L(L_WARNING, "/%s: wrong syntax", GetCommand().c_str());
		}

		return true;
	}
};



class LODScaleActionExecutor : public IUnsyncedActionExecutor {
public:
	LODScaleActionExecutor() : IUnsyncedActionExecutor(
		"LODScale",
		"Set the scale for either of: LOD (level-of-detail), shadow-LOD, reflection-LOD, refraction-LOD"
	) {
	}

	bool Execute(const UnsyncedAction& action) const {
		if (!action.GetArgs().empty()) {
			const vector<string> &args = CSimpleParser::Tokenize(action.GetArgs(), 0);

			if (args.size() == 2) {
				const int objType = Clamp(atoi(args[0].c_str()), int(LUAOBJ_UNIT), int(LUAOBJ_FEATURE));
				const float lodScale = atof(args[1].c_str());

				LuaObjectDrawer::SetLODScale(objType, lodScale);
			}
			else if (args.size() == 3) {
				const int objType = Clamp(atoi(args[1].c_str()), int(LUAOBJ_UNIT), int(LUAOBJ_FEATURE));
				const float lodScale = atof(args[2].c_str());

				if (args[0] == "shadow") {
					LuaObjectDrawer::SetLODScaleShadow(objType, lodScale);
				} else if (args[0] == "reflection") {
					LuaObjectDrawer::SetLODScaleReflection(objType, lodScale);
				} else if (args[0] == "refraction") {
					LuaObjectDrawer::SetLODScaleRefraction(objType, lodScale);
				}
			} else {
				LOG_L(L_WARNING, "/%s: wrong syntax", GetCommand().c_str());
			}
		} else {
			LOG_L(L_WARNING, "/%s: wrong syntax", GetCommand().c_str());
		}

		return true;
	}
};



class AirMeshActionExecutor: public IUnsyncedActionExecutor {
public:
	AirMeshActionExecutor(): IUnsyncedActionExecutor("airmesh", "Show/Hide the smooth air-mesh map overlay") {
	}

	bool Execute(const UnsyncedAction& action) const {
		InverseOrSetBool(smoothHeightMeshDrawer->DrawEnabled(), action.GetArgs());
		LogSystemStatus("smooth air-mesh map overlay", smoothHeightMeshDrawer->DrawEnabled());
		return true;
	}
};


class WireModelActionExecutor: public IUnsyncedActionExecutor {
public:
	WireModelActionExecutor(): IUnsyncedActionExecutor("WireModel", "Toggle wireframe-mode drawing of model geometry") {
	}

	bool Execute(const UnsyncedAction& action) const {
		// note: affects feature and projectile render-state for free
		LogSystemStatus("wireframe model-drawing mode", unitDrawer->WireFrameModeRef() = !unitDrawer->WireFrameModeRef());
		return true;
	}
};

class WireMapActionExecutor: public IUnsyncedActionExecutor {
public:
	WireMapActionExecutor(): IUnsyncedActionExecutor("WireMap", "Toggle wireframe-mode drawing of map geometry") {
	}

	bool Execute(const UnsyncedAction& action) const {
		CBaseGroundDrawer* gd = readMap->GetGroundDrawer();

		LogSystemStatus("wireframe map-drawing mode", gd->WireFrameModeRef() = !gd->WireFrameModeRef());
		return true;
	}
};

class WireSkyActionExecutor: public IUnsyncedActionExecutor {
public:
	WireSkyActionExecutor(): IUnsyncedActionExecutor("WireSky", "Toggle wireframe-mode drawing of skydome geometry") {
	}

	bool Execute(const UnsyncedAction& action) const {
		LogSystemStatus("wireframe sky-drawing mode", sky->WireFrameModeRef() = !sky->WireFrameModeRef());
		return true;
	}
};

class WireTreeActionExecutor: public IUnsyncedActionExecutor {
public:
	WireTreeActionExecutor(): IUnsyncedActionExecutor("WireTree", "Toggle wireframe-mode drawing of tree geometry") {
	}

	bool Execute(const UnsyncedAction& action) const {
		LogSystemStatus("wireframe tree-drawing mode", treeDrawer->WireFrameModeRef() = !treeDrawer->WireFrameModeRef());
		return true;
	}
};

class WireWaterActionExecutor: public IUnsyncedActionExecutor {
public:
	WireWaterActionExecutor(): IUnsyncedActionExecutor("WireWater", "Toggle wireframe-mode drawing of water geometry") {
	}

	bool Execute(const UnsyncedAction& action) const {
		LogSystemStatus("wireframe water-drawing mode", water->WireFrameModeRef() = !water->WireFrameModeRef());
		return true;
	}
};



class DebugColVolDrawerActionExecutor : public IUnsyncedActionExecutor {
public:
	DebugColVolDrawerActionExecutor(): IUnsyncedActionExecutor("DebugColVol", "Enable/Disable drawing of collision volumes") {
	}

	bool Execute(const UnsyncedAction& action) const {
		InverseOrSetBool(DebugColVolDrawer::enable, action.GetArgs());
		return true;
	}
};


class DebugPathDrawerActionExecutor : public IUnsyncedActionExecutor {
public:
	DebugPathDrawerActionExecutor(): IUnsyncedActionExecutor("DebugPath", "Enable/Disable drawing of pathfinder debug-data") {
	}

	bool Execute(const UnsyncedAction& action) const {
		LogSystemStatus("path-debug rendering mode", pathDrawer->ToggleEnabled());
		return true;
	}
};


class DebugTraceRayDrawerActionExecutor : public IUnsyncedActionExecutor {
public:
	DebugTraceRayDrawerActionExecutor(): IUnsyncedActionExecutor("DebugTraceRay", "Enable/Disable drawing of traceray debug-data") {
	}

	bool Execute(const UnsyncedAction& action) const {
		globalRendering->drawdebugtraceray = !globalRendering->drawdebugtraceray;
		LogSystemStatus("traceray debug rendering mode", globalRendering->drawdebugtraceray);
		return true;
	}
};



class CrashActionExecutor : public IUnsyncedActionExecutor {
public:
	CrashActionExecutor() : IUnsyncedActionExecutor("Crash", "Invoke an artificial crash through a NULL-pointer dereference (SIGSEGV)", true) {
	}

	bool Execute(const UnsyncedAction& action) const {
		int* a = 0;
		*a = 0;
		return true;
	}
};



class ExceptionActionExecutor : public IUnsyncedActionExecutor {
public:
	ExceptionActionExecutor() : IUnsyncedActionExecutor("Exception", "Invoke an artificial crash by throwing an std::runtime_error", true) {
	}

	bool Execute(const UnsyncedAction& action) const {
		throw std::runtime_error("Exception test");
		return true;
	}
};



class DivByZeroActionExecutor : public IUnsyncedActionExecutor {
public:
	DivByZeroActionExecutor() : IUnsyncedActionExecutor("DivByZero", "Invoke an artificial crash by performing a division-by-Zero", true) {
	}

	bool Execute(const UnsyncedAction& action) const {
		constexpr float a = 0.0f;
		LOG("Result: %f", 1.0f / a);
		return true;
	}
};



class GiveActionExecutor : public IUnsyncedActionExecutor {
public:
	GiveActionExecutor() : IUnsyncedActionExecutor(
		"Give",
		"Places one or multiple units of a single or multiple types on the map, instantly; by default to your own team", true
	) {
	}

	bool Execute(const UnsyncedAction& action) const {
		if (action.GetArgs().find('@') == string::npos) {
			CInputReceiver* ir = nullptr;

			if (!game->hideInterface && !mouse->offscreen)
				ir = CInputReceiver::GetReceiverAt(mouse->lastx, mouse->lasty);

			float3 givePos;

			if (ir == minimap) {
				givePos = minimap->GetMapPosition(mouse->lastx, mouse->lasty);
			} else {
				const float3& pos = camera->GetPos();
				const float3& dir = mouse->dir;
				const float dist = CGround::LineGroundCol(pos, pos + (dir * 30000.0f));
				givePos = pos + (dir * dist);
			}

			char message[256];
			SNPRINTF(message, sizeof(message),
					"%s %s @%.0f,%.0f,%.0f",
					GetCommand().c_str(), action.GetArgs().c_str(),
					givePos.x, givePos.y, givePos.z);

			CommandMessage pckt(message, gu->myPlayerNum);
			clientNet->Send(pckt.Pack());
		} else {
			// forward (as synced command)
			CommandMessage pckt(action.GetInnerAction(), gu->myPlayerNum);
			clientNet->Send(pckt.Pack());
		}

		return true;
	}
};

class DestroyActionExecutor : public IUnsyncedActionExecutor {
public:
	DestroyActionExecutor() : IUnsyncedActionExecutor("Destroy", "Destroys one or multiple units by unit-ID, instantly", true) {
	}

	bool Execute(const UnsyncedAction& action) const {
		if (selectedUnitsHandler.selectedUnits.empty())
			return false;

		// kill selected units
		std::stringstream ss;
		ss << GetCommand();

		for (const int unitID: selectedUnitsHandler.selectedUnits) {
			ss << " " << unitID;
		}

		CommandMessage pckt(ss.str(), gu->myPlayerNum);
		clientNet->Send(pckt.Pack());
		return true;
	}
};



class SendActionExecutor : public IUnsyncedActionExecutor {
public:
	SendActionExecutor() : IUnsyncedActionExecutor("Send", "Send a string as raw network message to the game host (for debugging only)") {
	}

	bool Execute(const UnsyncedAction& action) const {
		CommandMessage pckt(Action(action.GetArgs()), gu->myPlayerNum);
		clientNet->Send(pckt.Pack());
		return true;
	}
};



class SaveGameActionExecutor : public IUnsyncedActionExecutor {
public:
	SaveGameActionExecutor() : IUnsyncedActionExecutor("SaveGame", "Save the game state to QuickSave.ssf (BROKEN)") {
	}

	bool Execute(const UnsyncedAction& action) const {
		game->SaveGame("Saves/QuickSave.ssf", true, true);
		return true;
	}
};



class DumpStateActionExecutor: public IUnsyncedActionExecutor {
public:
	DumpStateActionExecutor(): IUnsyncedActionExecutor("DumpState", "dump game-state to file") {
	}

	bool Execute(const UnsyncedAction& action) const {
		const std::vector<std::string>& args = _local_strSpaceTokenize(action.GetArgs());

		switch (args.size()) {
			case 2: { DumpState(atoi(args[0].c_str()), atoi(args[1].c_str()),                     1); } break;
			case 3: { DumpState(atoi(args[0].c_str()), atoi(args[1].c_str()), atoi(args[2].c_str())); } break;
			default: { LOG_L(L_WARNING, "/DumpState: wrong syntax");  } break;
		}

		return true;
	}
};



/// /save [-y ]<savename>
class SaveActionExecutor : public IUnsyncedActionExecutor {
public:
	SaveActionExecutor(bool _usecreg) : IUnsyncedActionExecutor(
		(_usecreg)? "Save" : "LuaSave",
		"Save the game state to a specific file, add -y to overwrite when file is already present"
	) {
		usecreg = _usecreg;
	}

	bool Execute(const UnsyncedAction& action) const {
		const std::vector<std::string>& args = _local_strSpaceTokenize(action.GetArgs());

		switch (args.size()) {
			case 1: {
				game->SaveGame("Saves/" + args[0] + ((usecreg)? ".ssf": ".slsf"), false, usecreg);
			} break;
			case 2: {
				game->SaveGame("Saves/" + args[0] + ((usecreg)? ".ssf": ".slsf"), (args[1] == "-y"), usecreg);
			} break;
			default: {
				return false;
			} break;
		}

		return true;
	}
private:
	bool usecreg;
};



class ReloadGameActionExecutor : public IUnsyncedActionExecutor {
public:
	ReloadGameActionExecutor() : IUnsyncedActionExecutor("ReloadGame", "Restarts the game with the initially provided start-script") {
	}

	bool Execute(const UnsyncedAction& action) const {
		game->ReloadGame();
		return true;
	}
};



class ReloadShadersActionExecutor : public IUnsyncedActionExecutor {
public:
	ReloadShadersActionExecutor() : IUnsyncedActionExecutor("ReloadShaders", "Reloads all engine shaders") {
	}

	bool Execute(const UnsyncedAction& action) const {
		LOG("Reloading all engine shaders");
		//FIXME make threadsafe!
		shaderHandler->ReloadAll();
		return true;
	}
};



class DebugInfoActionExecutor : public IUnsyncedActionExecutor {
public:
	DebugInfoActionExecutor() : IUnsyncedActionExecutor(
		"DebugInfo",
		"Print debug info to the chat/log-file about either: sound, profiling"
	) {
	}

	bool Execute(const UnsyncedAction& action) const {
		if (action.GetArgs() == "sound") {
			sound->PrintDebugInfo();
		} else if (action.GetArgs() == "profiling") {
			profiler.PrintProfilingInfo();
		} else {
			LOG_L(L_WARNING, "Give either of these as argument: sound, profiling");
		}
		return true;
	}
};



class RedirectToSyncedActionExecutor : public IUnsyncedActionExecutor {
public:
	RedirectToSyncedActionExecutor(const std::string& command): IUnsyncedActionExecutor(
		command,
		"Redirects command /" + command + " to its synced processor"
	) {
	}

	bool Execute(const UnsyncedAction& action) const {
		// redirect as a synced command
		CommandMessage pckt(action.GetInnerAction(), gu->myPlayerNum);
		clientNet->Send(pckt.Pack());
		return true;
	}
};



class CommandListActionExecutor : public IUnsyncedActionExecutor {
public:
	CommandListActionExecutor() : IUnsyncedActionExecutor(
		"CommandList",
		"Prints all the available chat commands with description (if available) to the console"
	) {
	}

	bool Execute(const UnsyncedAction& action) const {
		LOG("Chat commands plus description");
		LOG("==============================");
		PrintToConsole(syncedGameCommands->GetActionExecutors());
		PrintToConsole(unsyncedGameCommands->GetActionExecutors());
		return true;
	}

	template<class action_t, bool synced_v>
	static void PrintExecutorToConsole(const IActionExecutor<action_t, synced_v>* executor) {
		LOG("/%-30s  %s  %s",
				executor->GetCommand().c_str(),
				(executor->IsSynced() ? "(synced)  " : "(unsynced)"),
				executor->GetDescription().c_str());
	}

private:
	void PrintToConsole(const std::map<std::string, ISyncedActionExecutor*>& executors) const {
		for (const auto& p: executors) {
			PrintExecutorToConsole(p.second);
		}
	}

	void PrintToConsole(const std::map<std::string, IUnsyncedActionExecutor*>& executors) const {
		for (const auto& p: executors) {
			PrintExecutorToConsole(p.second);
		}
	}
};



class CommandHelpActionExecutor : public IUnsyncedActionExecutor {
public:
	CommandHelpActionExecutor() : IUnsyncedActionExecutor("CommandHelp", "Prints info about a specific chat command") {
	}

	bool Execute(const UnsyncedAction& action) const {
		const std::vector<std::string> args = CSimpleParser::Tokenize(action.GetArgs(), 1);

		if (!args.empty()) {
			const std::string commandLower = StringToLower(args[0]);

			// try if an unsynced chat command with this name is available
			const IUnsyncedActionExecutor* unsyncedExecutor = unsyncedGameCommands->GetActionExecutor(commandLower);

			if (unsyncedExecutor != nullptr) {
				PrintExecutorHelpToConsole(unsyncedExecutor);
				return true;
			}

			// try if a synced chat command with this name is available
			const ISyncedActionExecutor* syncedExecutor = syncedGameCommands->GetActionExecutor(commandLower);

			if (syncedExecutor != nullptr) {
				PrintExecutorHelpToConsole(syncedExecutor);
				return true;
			}

			LOG_L(L_WARNING, "No chat command registered with name \"%s\" (case-insensitive)", args[0].c_str());
		} else {
			LOG_L(L_WARNING, "missing command-name");
		}

		return true;
	}

private:
	template<class action_t, bool synced_v>
	static void PrintExecutorHelpToConsole(const IActionExecutor<action_t, synced_v>* executor)
	{
		// XXX extend this in case more info about commands are available (for example "Usage: name {args}")
		CommandListActionExecutor::PrintExecutorToConsole(executor);
	}
};

} // namespace (unnamed)





// TODO CGame stuff in UnsyncedGameCommands: refactor (or move)
bool CGame::ActionReleased(const Action& action)
{
	switch (hashString(action.command.c_str())) {
		case hashString("drawinmap"): {
			inMapDrawer->SetDrawMode(false);
		} break;

		case hashString("moveforward"): {
			camera->SetMovState(CCamera::MOVE_STATE_FWD, false);
		} break;
		case hashString("moveback"): {
			camera->SetMovState(CCamera::MOVE_STATE_BCK, false);
		} break;
		case hashString("moveleft"): {
			camera->SetMovState(CCamera::MOVE_STATE_LFT, false);
		} break;
		case hashString("moveright"): {
			camera->SetMovState(CCamera::MOVE_STATE_RGT, false);
		} break;
		case hashString("moveup"): {
			camera->SetMovState(CCamera::MOVE_STATE_UP, false);
		} break;
		case hashString("movedown"): {
			camera->SetMovState(CCamera::MOVE_STATE_DWN, false);
		} break;

		case hashString("movefast"): {
			camera->SetMovState(CCamera::MOVE_STATE_FST, false);
		} break;
		case hashString("moveslow"): {
			camera->SetMovState(CCamera::MOVE_STATE_SLW, false);
		} break;

		case hashString("mouse1"): {
			mouse->MouseRelease(mouse->lastx, mouse->lasty, 1);
		} break;
		case hashString("mouse2"): {
			mouse->MouseRelease(mouse->lastx, mouse->lasty, 2);
		} break;
		case hashString("mouse3"): {
			mouse->MouseRelease(mouse->lastx, mouse->lasty, 3);
		} break;

		#if 0
		// HACK   somehow weird things happen when MouseRelease is called for button 4 and 5.
		// Note that SYS_WMEVENT on windows also only sends MousePress events for these buttons.
		case hashString("mouse4"): {
			mouse->MouseRelease(mouse->lastx, mouse->lasty, 4);
		} break;
		case hashString("mouse5"): {
			mouse->MouseRelease(mouse->lastx, mouse->lasty, 5);
		} break;
		#endif

		case hashString("mousestate"): {
			mouse->ToggleMiddleClickScroll();
		} break;
		case hashString("gameinfoclose"): {
			CGameInfo::Disable();
		} break;

		default: {
		} break;
	}

	return 0;
}




void UnsyncedGameCommands::AddDefaultActionExecutors() {

	AddActionExecutor(new SelectActionExecutor());
	AddActionExecutor(new SelectUnitsActionExecutor());
	AddActionExecutor(new SelectCycleActionExecutor());
	AddActionExecutor(new DeselectActionExecutor());
	AddActionExecutor(new ShadowsActionExecutor());
	AddActionExecutor(new MapShadowPolyOffsetActionExecutor());
	AddActionExecutor(new MapMeshDrawerActionExecutor());
	AddActionExecutor(new MapBorderActionExecutor());
	AddActionExecutor(new WaterActionExecutor());
	AddActionExecutor(new SayActionExecutor());
	AddActionExecutor(new SayPrivateActionExecutor());
	AddActionExecutor(new SayPrivateByPlayerIDActionExecutor());
	AddActionExecutor(new EchoActionExecutor());
	AddActionExecutor(new SetActionExecutor());
	AddActionExecutor(new SetOverlayActionExecutor());
	AddActionExecutor(new EnableDrawInMapActionExecutor());
	AddActionExecutor(new DrawLabelActionExecutor());
	AddActionExecutor(new MouseActionExecutor(1));
	AddActionExecutor(new MouseActionExecutor(2));
	AddActionExecutor(new MouseActionExecutor(3));
	AddActionExecutor(new MouseActionExecutor(4));
	AddActionExecutor(new MouseActionExecutor(5));
	AddActionExecutor(new ViewSelectionActionExecutor());
	AddActionExecutor(new CameraMoveActionExecutor(0, "Forward"));
	AddActionExecutor(new CameraMoveActionExecutor(1, "Back"));
	AddActionExecutor(new CameraMoveActionExecutor(2, "Left"));
	AddActionExecutor(new CameraMoveActionExecutor(3, "Right"));
	AddActionExecutor(new CameraMoveActionExecutor(4, "Up"));
	AddActionExecutor(new CameraMoveActionExecutor(5, "Down"));
	AddActionExecutor(new CameraMoveActionExecutor(6, "Fast"));
	AddActionExecutor(new CameraMoveActionExecutor(7, "Slow"));
	AddActionExecutor(new AIKillReloadActionExecutor(true));
	AddActionExecutor(new AIKillReloadActionExecutor(false));
	AddActionExecutor(new AIControlActionExecutor());
	AddActionExecutor(new AIListActionExecutor());
	AddActionExecutor(new TeamActionExecutor());
	AddActionExecutor(new SpectatorActionExecutor());
	AddActionExecutor(new SpecTeamActionExecutor());
	AddActionExecutor(new SpecFullViewActionExecutor());
	AddActionExecutor(new AllyActionExecutor());
	AddActionExecutor(new GroupActionExecutor());
	AddActionExecutor(new GroupIDActionExecutor(0));
	AddActionExecutor(new GroupIDActionExecutor(1));
	AddActionExecutor(new GroupIDActionExecutor(2));
	AddActionExecutor(new GroupIDActionExecutor(3));
	AddActionExecutor(new GroupIDActionExecutor(4));
	AddActionExecutor(new GroupIDActionExecutor(5));
	AddActionExecutor(new GroupIDActionExecutor(6));
	AddActionExecutor(new GroupIDActionExecutor(7));
	AddActionExecutor(new GroupIDActionExecutor(8));
	AddActionExecutor(new GroupIDActionExecutor(9));
	AddActionExecutor(new LastMessagePositionActionExecutor());
	ChatActionExecutor::RegisterCommandVariants();
	AddActionExecutor(new TrackActionExecutor());
	AddActionExecutor(new TrackOffActionExecutor());
	AddActionExecutor(new TrackModeActionExecutor());
	AddActionExecutor(new PauseActionExecutor());
	AddActionExecutor(new DebugActionExecutor());
	AddActionExecutor(new DebugGLActionExecutor());
	AddActionExecutor(new DebugGLErrorsActionExecutor());
	AddActionExecutor(new DebugColVolDrawerActionExecutor());
	AddActionExecutor(new DebugPathDrawerActionExecutor());
	AddActionExecutor(new DebugTraceRayDrawerActionExecutor());
	AddActionExecutor(new MuteActionExecutor());
	AddActionExecutor(new SoundActionExecutor());
	AddActionExecutor(new SoundChannelEnableActionExecutor());
	AddActionExecutor(new CreateVideoActionExecutor());
	AddActionExecutor(new DrawGrassActionExecutor());
	AddActionExecutor(new DrawTreesActionExecutor());
	AddActionExecutor(new NetPingActionExecutor());
	AddActionExecutor(new NetMsgSmoothingActionExecutor());
	AddActionExecutor(new SpeedControlActionExecutor());
	AddActionExecutor(new GameInfoActionExecutor());
	AddActionExecutor(new HideInterfaceActionExecutor());
	AddActionExecutor(new HardwareCursorActionExecutor());
	AddActionExecutor(new FullscreenActionExecutor());
	AddActionExecutor(new IncreaseViewRadiusActionExecutor());
	AddActionExecutor(new DecreaseViewRadiusActionExecutor());
	AddActionExecutor(new GroundDetailActionExecutor());
	AddActionExecutor(new MoreGrassActionExecutor());
	AddActionExecutor(new LessGrassActionExecutor());
	AddActionExecutor(new MoreTreesActionExecutor());
	AddActionExecutor(new LessTreesActionExecutor());
	AddActionExecutor(new SpeedUpActionExecutor());
	AddActionExecutor(new SlowDownActionExecutor());
	AddActionExecutor(new ControlUnitActionExecutor());
	AddActionExecutor(new ShowStandardActionExecutor());
	AddActionExecutor(new ShowElevationActionExecutor());
	AddActionExecutor(new ShowMetalMapActionExecutor());
	AddActionExecutor(new ShowPathTravActionExecutor());
	AddActionExecutor(new ShowPathHeatActionExecutor());
	AddActionExecutor(new ShowPathFlowActionExecutor());
	AddActionExecutor(new ShowPathCostActionExecutor());
	AddActionExecutor(new ToggleLOSActionExecutor());
	AddActionExecutor(new ToggleInfoActionExecutor());
	AddActionExecutor(new ShowPathTypeActionExecutor());
	AddActionExecutor(new ShareDialogActionExecutor());
	AddActionExecutor(new QuitMessageActionExecutor());
	AddActionExecutor(new QuitMenuActionExecutor());
	AddActionExecutor(new QuitActionExecutor());
	AddActionExecutor(new ReloadActionExecutor());
	AddActionExecutor(new IncreaseGUIOpacityActionExecutor());
	AddActionExecutor(new DecreaseGUIOpacityActionExecutor());
	AddActionExecutor(new ScreenShotActionExecutor());
	AddActionExecutor(new GrabInputActionExecutor());
	AddActionExecutor(new ClockActionExecutor());
	AddActionExecutor(new CrossActionExecutor());
	AddActionExecutor(new FPSActionExecutor());
	AddActionExecutor(new SpeedActionExecutor());
	AddActionExecutor(new TeamHighlightActionExecutor());
	AddActionExecutor(new InfoActionExecutor());
	AddActionExecutor(new CmdColorsActionExecutor());
	AddActionExecutor(new CtrlPanelActionExecutor());
	AddActionExecutor(new FontActionExecutor());
	AddActionExecutor(new VSyncActionExecutor());
	AddActionExecutor(new SafeGLActionExecutor());
	AddActionExecutor(new ResBarActionExecutor());
	AddActionExecutor(new ToolTipActionExecutor());
	AddActionExecutor(new ConsoleActionExecutor());
	AddActionExecutor(new EndGraphActionExecutor());
	AddActionExecutor(new FPSHudActionExecutor());
	AddActionExecutor(new DebugDrawAIActionExecutor());
	AddActionExecutor(new MapMarksActionExecutor());
	AddActionExecutor(new AllMapMarksActionExecutor());
	AddActionExecutor(new ClearMapMarksActionExecutor());
	AddActionExecutor(new NoLuaDrawActionExecutor());
	AddActionExecutor(new LuaUIActionExecutor());
	AddActionExecutor(new MiniMapActionExecutor());
	AddActionExecutor(new GroundDecalsActionExecutor());

	AddActionExecutor(new DistSortProjectilesActionExecutor());
	AddActionExecutor(new MaxParticlesActionExecutor());
	AddActionExecutor(new MaxNanoParticlesActionExecutor());

	AddActionExecutor(new GatherModeActionExecutor());
	AddActionExecutor(new PasteTextActionExecutor());
	AddActionExecutor(new BufferTextActionExecutor());
	AddActionExecutor(new InputTextGeoActionExecutor());
	AddActionExecutor(new DistIconActionExecutor());
	AddActionExecutor(new DistDrawActionExecutor());
	AddActionExecutor(new LODScaleActionExecutor());
	AddActionExecutor(new AirMeshActionExecutor());
	AddActionExecutor(new WireModelActionExecutor());
	AddActionExecutor(new WireMapActionExecutor());
	AddActionExecutor(new WireSkyActionExecutor());
	AddActionExecutor(new WireTreeActionExecutor());
	AddActionExecutor(new WireWaterActionExecutor());
	AddActionExecutor(new CrashActionExecutor());
	AddActionExecutor(new ExceptionActionExecutor());
	AddActionExecutor(new DivByZeroActionExecutor());
	AddActionExecutor(new GiveActionExecutor());
	AddActionExecutor(new DestroyActionExecutor());
	AddActionExecutor(new SendActionExecutor());
	AddActionExecutor(new SaveGameActionExecutor());
	AddActionExecutor(new DumpStateActionExecutor());
	AddActionExecutor(new SaveActionExecutor(true));
	AddActionExecutor(new SaveActionExecutor(false));
	AddActionExecutor(new ReloadGameActionExecutor());
	AddActionExecutor(new ReloadShadersActionExecutor());
	AddActionExecutor(new DebugInfoActionExecutor());

	// XXX are these redirects really required?
	AddActionExecutor(new RedirectToSyncedActionExecutor("ATM"));
#ifdef DEBUG
	AddActionExecutor(new RedirectToSyncedActionExecutor("Desync"));
#endif
	AddActionExecutor(new RedirectToSyncedActionExecutor("Resync"));
	if (modInfo.allowTake) {
		AddActionExecutor(new RedirectToSyncedActionExecutor("Take"));
	}
	AddActionExecutor(new RedirectToSyncedActionExecutor("LuaRules"));
	AddActionExecutor(new RedirectToSyncedActionExecutor("LuaGaia"));
	AddActionExecutor(new CommandListActionExecutor());
	AddActionExecutor(new CommandHelpActionExecutor());
}


UnsyncedGameCommands* UnsyncedGameCommands::singleton = nullptr;

void UnsyncedGameCommands::CreateInstance() {
	if (singleton == nullptr) {
		singleton = new UnsyncedGameCommands();
	} else {
		throw std::logic_error("UnsyncedGameCommands singleton is already initialized");
	}
}

void UnsyncedGameCommands::DestroyInstance() {
	if (singleton != nullptr) {
		spring::SafeDelete(singleton);
	} else {
		// this might happen during shutdown after an unclean init
		LOG_L(L_WARNING, "UnsyncedGameCommands singleton was not initialized or is already destroyed");
	}
}
