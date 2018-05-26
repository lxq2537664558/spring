/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef SKIRMISH_AI_HANDLER_H
#define SKIRMISH_AI_HANDLER_H

#include <array>

#include "ExternalAI/SkirmishAIData.h"
#include "ExternalAI/SkirmishAIKey.h"
#include "Sim/Misc/GlobalConstants.h"
#include "System/creg/creg_cond.h"
#include "System/UnorderedMap.hpp"
#include "System/UnorderedSet.hpp"


class CGameSetup;

/**
 * Handles all Skirmish AI instance relevant data, which includes,
 * but is not limited to all sync relevant Skirmish AI stuff.
 */
class CSkirmishAIHandler
{
	CR_DECLARE_STRUCT(CSkirmishAIHandler)

public:
	void ResetState();
	void LoadFromSetup(const CGameSetup& setup);

	/**
	 * Will be called when the Mods archives were loaded into the VFS,
	 * and we received our player number (gu->myPlayerNum is set).
	 * This loads the list of Lua AIs from the mod archives LuaAI.lua.
	 */
	void LoadPreGame();

	/**
	 * @param skirmishAIId index to check
	 * @return true if is active Skirmish AI
	 *
	 * Returns true if data for a Skirmish AI with the specified id is stored.
	 */
	bool IsActiveSkirmishAI(const size_t skirmishAIId) const;

	/**
	 * @brief SkirmishAIData
	 * @param skirmishAIId index to fetch
	 * @return SkirmishAIData pointer
	 *
	 * Accesses the data of a Skirmish AI instance at a given index.
	 */
	SkirmishAIData* GetSkirmishAI(const size_t skirmishAIId);

	/**
	 * @brief SkirmishAIData
	 * @param name name of the Skirmish AI instance
	 * @return its index or -1 if not found
	 *
	 * Search a Skirmish AI instance by name.
	 */
	size_t GetSkirmishAI(const std::string& name) const;

	/**
	 * @brief Skirmish AIs controlling a team
	 *
	 * Will change during runtime (Connection lost, died, killed, created, ...).
	 */
	std::vector<uint8_t> GetSkirmishAIsInTeam(const int teamId, const int hostPlayerId = -1) const;

	/**
	 * @brief Skirmish AIs hosted by a player
	 *
	 * Will change during runtime (Connection lost, died, killed, created, ...).
	 */
	std::vector<uint8_t> GetSkirmishAIsByPlayer(const int playerId);

	/**
	 * @brief All active Skirmish AIs
	 *
	 * Will change during runtime (Connection lost, died, killed, created, ...).
	 */
	const spring::unordered_map<uint8_t, SkirmishAIData>& GetAllSkirmishAIs() const { return skirmishAIDataMap; }


	/**
	 * @brief Adds a Skirmish AI
	 * @param data contans the details for the Skirmish AI to add
	 * @param skirmishAIId id of the Skirmish AI, generated by
	 *                     and received from the server
	 */
	bool AddSkirmishAI(const SkirmishAIData& data, const size_t skirmishAIId);

	/**
	 * @brief Removes a Skirmish AI
	 * @param skirmishAIId id of the Skirmish AI to be removed
	 * @return true if the Skirmish AI was removed
	 */
	bool RemoveSkirmishAI(const size_t skirmishAIId);

	bool HasSkirmishAIsInTeam(const int teamId, const int hostPlayerId = -1) const {
		return (GetSkirmishAIsInTeam(teamId, hostPlayerId) != std::vector<uint8_t>{});
	}

	size_t GetNumSkirmishAIs() const { return numSkirmishAIs; }
	// size_t GetNumSkirmishAIsInTeam(const int teamId, const int hostPlayerId = -1) const { ... }


	/**
	 * Starts the initialization process of a locally running Skirmish AI,
	 * which was defined in the start script.
	 * Do NOT use for creating AIs not defined in the start script,
	 * as it will cuase desyncs.
	 * Stores detailed info locally real creation happens right here.
	 * @param skirmishAIId Skirmish AI index
	 * @see EngineOutHandler::CreateSkirmishAI()
	 */
	void CreateLocalSkirmishAI(const size_t skirmishAIId);
	/**
	 * Starts the synced initialization process of a locally running Skirmish AI.
	 * Stores detailed info locally and sends synced stuff in a message
	 * to the server, and real creation will happen when receiving the answer.
	 * @param aiData detailed info of the AI to create
	 * @see EngineOutHandler::CreateSkirmishAI()
	 */
	void CreateLocalSkirmishAI(const SkirmishAIData& aiData);
	/**
	 * Returns detailed (including unsynced) data for a Skirmish AI to be
	 * running on the local machine.
	 * @param teamId index of the team the AI shall be created for.
	 *               only one AI per player can be in creation
	 * @return detailed (including unsynced) data for a Skirmish AI
	 * @see CreateLocalSkirmishAI()
	 */
	const SkirmishAIData* GetLocalSkirmishAIInCreation(const int teamId) const;

	/**
	 * This may only be called for local AIs.
	 * Sends a message to the server, while real destruction will happen
	 * when receiving the answer.
	 * @param skirmishAIId index of the AI to destroy
	 * @param reason for a list of values, see SReleaseEvent in ExternalAI/Interface/AISEvents.h
	 * @see EngineOutHandler::DestroySkirmishAI()
	 */
	void SetLocalKillFlag(const size_t skirmishAIId, const int reason);

	/**
	 * Returns a value explaining why a Skirmish AI is dieing, or a value < 0
	 * if it is not.
	 * @param skirmishAIId index of the AI in question
	 * @return for a list of values, see SReleaseEvent in ExternalAI/Interface/AISEvents.h
	 * @see IsSkirmishAIDieing()
	 */
	int GetLocalKillFlag(const size_t skirmishAIId) const { return aiKillFlags[skirmishAIId]; }

	/**
	 * Reports true even before the DIEING state was received
	 * from the server, but only for local AIs.
	 * @param skirmishAIId index of the AI in question
	 */
	bool HasLocalKillFlag(const size_t skirmishAIId) const { return (GetLocalKillFlag(skirmishAIId) != -1); }
	bool IsLocalSkirmishAI(const size_t skirmishAIId) const;

	/**
	 * Returns the library key for a local Skirmish AI, or NULL.
	 */
	const SkirmishAIKey* GetLocalSkirmishAILibraryKey(const size_t skirmishAIId);

	const spring::unordered_set<std::string>& GetLuaAIImplShortNames() const { return luaAIShortNames; }

	uint8_t GetCurrentAIID() { return currentAIId; }
	void SetCurrentAIID(uint8_t id) { currentAIId = id; }

private:
	static bool IsLocalSkirmishAI(const SkirmishAIData& aiData);
	static bool IsValidSkirmishAI(const SkirmishAIData& aiData) { return (!aiData.shortName.empty()); }

	bool IsLuaAI(const SkirmishAIData& aiData) const { return (luaAIShortNames.find(aiData.shortName) != luaAIShortNames.end()); }

	void CompleteWithDefaultOptionValues(const size_t skirmishAIId);
	void CompleteSkirmishAI(const size_t skirmishAIId);


	/// id -> AI instance
	std::array<SkirmishAIData, MAX_AIS> aiInstanceData;

	/// id -> AI instance library key
	std::array<SkirmishAIKey, MAX_AIS> aiLibraryKeys;

	/// temporarily stores detailed info of local Skirmish AIs waiting for initialization
	std::array<SkirmishAIData, MAX_AIS> localTeamAIs;

	/// temporarily stores reason for killing a Skirmish AI
	std::array<int, MAX_AIS> aiKillFlags;

	spring::unordered_map<uint8_t, SkirmishAIData> skirmishAIDataMap;
	spring::unordered_set<std::string> luaAIShortNames;

	// the current local AI ID that is executing, MAX_AIS if none (e.g. LuaUI)
	uint8_t currentAIId = MAX_AIS;
	uint8_t numSkirmishAIs = 0;

	bool gameInitialized = false;
};

extern CSkirmishAIHandler skirmishAIHandler;

#endif // SKIRMISH_AI_HANDLER_H

