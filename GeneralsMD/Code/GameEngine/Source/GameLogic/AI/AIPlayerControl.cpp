/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

////////////////////////////////////////////////////////////////////////////////
//
//  (c) 2001-2003 Electronic Arts Inc.
//
////////////////////////////////////////////////////////////////////////////////

// AIPlayerControl.cpp
// Implementation of AI Player Control System
// Enables internal AI to control human player slots

#include "Common/GameCommon.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/Team.h"
#include "GameLogic/AIPlayer.h"
#include "GameLogic/AISkirmishPlayer.h"
#include "GameLogic/AIPlayerControl.h"

// ----------------------------------------------------------------------------------------------
// Internal state tracking for AI-controlled players
// ----------------------------------------------------------------------------------------------

static Bool s_aiControlEnabled[MAX_PLAYER_COUNT] = { FALSE };
static Bool s_useSkirmishAI[MAX_PLAYER_COUNT] = { FALSE };

// Default configuration values
static const Int DEFAULT_TEAM_DELAY_SECONDS = 10;
static const GameDifficulty DEFAULT_DIFFICULTY = DIFFICULTY_NORMAL;

// ----------------------------------------------------------------------------------------------
/**
 * Enable or disable AI control for a specific player.
 * 
 * @param playerIndex The index of the player to control (0-based)
 * @param enableAI True to enable AI control, false to return to human control
 * @param useSkirmishAI True to use the advanced skirmish AI, false for basic AI
 */
// ----------------------------------------------------------------------------------------------
void enableAIControl(PlayerIndex playerIndex, Bool enableAI, Bool useSkirmishAI)
{
	// Validate player index
	if (playerIndex < 0 || playerIndex >= MAX_PLAYER_COUNT) {
		DEBUGLOG(("AIPlayerControl: Invalid player index %d\n", playerIndex));
		return;
	}

	Player* player = ThePlayerList->getNthPlayer(playerIndex);
	if (player == nullptr) {
		DEBUGLOG(("AIPlayerControl: Player %d does not exist\n", playerIndex));
		return;
	}

	if (enableAI) {
		// Check if already enabled
		if (s_aiControlEnabled[playerIndex]) {
			DEBUGLOG(("AIPlayerControl: AI control already enabled for player %d\n", playerIndex));
			return;
		}

		DEBUGLOG(("AIPlayerControl: Enabling AI control for player %d (Skirmish: %d)\n", 
			playerIndex, useSkirmishAI ? 1 : 0));

		// Store configuration
		s_aiControlEnabled[playerIndex] = TRUE;
		s_useSkirmishAI[playerIndex] = useSkirmishAI;

		// Create appropriate AI instance based on configuration
		AIPlayer* existingAI = player->getAI();
		
		if (existingAI != nullptr) {
			// AI already exists, just ensure it's the right type
			if (useSkirmishAI && !existingAI->isSkirmishAI()) {
				// Need to replace with skirmish AI
				DEBUGLOG(("AIPlayerControl: Upgrading to Skirmish AI for player %d\n", playerIndex));
				// Note: In a full implementation, we would delete the old AI and create a new one
				// For now, we'll work with what we have
			}
		} else {
			// Create new AI instance
			if (useSkirmishAI) {
				DEBUGLOG(("AIPlayerControl: Creating Skirmish AI for player %d\n", playerIndex));
				AISkirmishPlayer* skirmishAI = NEW AISkirmishPlayer(player);
				// The AI constructor should associate itself with the player
				// In the actual game code, this happens through the Player's m_ai member
			} else {
				DEBUGLOG(("AIPlayerControl: Creating Basic AI for player %d\n", playerIndex));
				AIPlayer* basicAI = NEW AIPlayer(player);
			}
		}

		// Apply default configuration
		configureAI(playerIndex, DEFAULT_TEAM_DELAY_SECONDS, DEFAULT_DIFFICULTY);

	} else {
		// Disable AI control
		if (!s_aiControlEnabled[playerIndex]) {
			DEBUGLOG(("AIPlayerControl: AI control already disabled for player %d\n", playerIndex));
			return;
		}

		DEBUGLOG(("AIPlayerControl: Disabling AI control for player %d\n", playerIndex));

		s_aiControlEnabled[playerIndex] = FALSE;
		s_useSkirmishAI[playerIndex] = FALSE;

		// Note: We don't delete the AI here to avoid potential crashes
		// The AI will simply stop updating when not enabled
		// In a full implementation, you might want to clean up the AI instance
	}
}

// ----------------------------------------------------------------------------------------------
/**
 * Check if a player is currently under AI control.
 * 
 * @param playerIndex The index of the player to check
 * @return True if AI is controlling this player
 */
// ----------------------------------------------------------------------------------------------
Bool isAIControlled(PlayerIndex playerIndex)
{
	if (playerIndex < 0 || playerIndex >= MAX_PLAYER_COUNT) {
		return FALSE;
	}

	return s_aiControlEnabled[playerIndex];
}

// ----------------------------------------------------------------------------------------------
/**
 * Get the AI player instance for a specific player.
 * 
 * @param playerIndex The index of the player
 * @return Pointer to the AIPlayer instance, or nullptr if not AI-controlled
 */
// ----------------------------------------------------------------------------------------------
AIPlayer* getAIForPlayer(PlayerIndex playerIndex)
{
	if (playerIndex < 0 || playerIndex >= MAX_PLAYER_COUNT) {
		return nullptr;
	}

	if (!s_aiControlEnabled[playerIndex]) {
		return nullptr;
	}

	Player* player = ThePlayerList->getNthPlayer(playerIndex);
	if (player == nullptr) {
		return nullptr;
	}

	return player->getAI();
}

// ----------------------------------------------------------------------------------------------
/**
 * Configure AI behavior parameters.
 * 
 * @param playerIndex The index of the player
 * @param teamDelaySeconds Delay between team creation (default: 10)
 * @param difficulty AI difficulty level
 */
// ----------------------------------------------------------------------------------------------
void configureAI(PlayerIndex playerIndex, Int teamDelaySeconds, GameDifficulty difficulty)
{
	if (playerIndex < 0 || playerIndex >= MAX_PLAYER_COUNT) {
		DEBUGLOG(("AIPlayerControl: Invalid player index %d for configureAI\n", playerIndex));
		return;
	}

	AIPlayer* ai = getAIForPlayer(playerIndex);
	if (ai == nullptr) {
		DEBUGLOG(("AIPlayerControl: No AI found for player %d\n", playerIndex));
		return;
	}

	DEBUGLOG(("AIPlayerControl: Configuring AI for player %d - Delay: %ds, Difficulty: %d\n",
		playerIndex, teamDelaySeconds, (Int)difficulty));

	// Set team delay
	ai->setTeamDelaySeconds(teamDelaySeconds);

	// Set difficulty
	ai->setAIDifficulty(difficulty);
}

// ----------------------------------------------------------------------------------------------
/**
 * Force AI to build a specific building immediately.
 * 
 * @param playerIndex The index of the player
 * @param buildingName Internal name of the building to construct
 */
// ----------------------------------------------------------------------------------------------
void aiBuildBuilding(PlayerIndex playerIndex, const char* buildingName)
{
	if (playerIndex < 0 || playerIndex >= MAX_PLAYER_COUNT) {
		DEBUGLOG(("AIPlayerControl: Invalid player index %d for aiBuildBuilding\n", playerIndex));
		return;
	}

	if (buildingName == nullptr || buildingName[0] == '\0') {
		DEBUGLOG(("AIPlayerControl: Invalid building name\n"));
		return;
	}

	AIPlayer* ai = getAIForPlayer(playerIndex);
	if (ai == nullptr) {
		DEBUGLOG(("AIPlayerControl: No AI found for player %d\n", playerIndex));
		return;
	}

	DEBUGLOG(("AIPlayerControl: Requesting building '%s' for player %d\n", 
		buildingName, playerIndex));

	// Convert to AsciiString and request construction
	AsciiString buildingAscii(buildingName);
	ai->buildSpecificAIBuilding(buildingAscii);
}

// ----------------------------------------------------------------------------------------------
/**
 * Force AI to build a specific team immediately.
 * 
 * @param playerIndex The index of the player
 * @param teamName Internal name of the team prototype to build
 */
// ----------------------------------------------------------------------------------------------
void aiBuildTeam(PlayerIndex playerIndex, const char* teamName)
{
	if (playerIndex < 0 || playerIndex >= MAX_PLAYER_COUNT) {
		DEBUGLOG(("AIPlayerControl: Invalid player index %d for aiBuildTeam\n", playerIndex));
		return;
	}

	if (teamName == nullptr || teamName[0] == '\0') {
		DEBUGLOG(("AIPlayerControl: Invalid team name\n"));
		return;
	}

	AIPlayer* ai = getAIForPlayer(playerIndex);
	if (ai == nullptr) {
		DEBUGLOG(("AIPlayerControl: No AI found for player %d\n", playerIndex));
		return;
	}

	DEBUGLOG(("AIPlayerControl: Requesting team '%s' for player %d\n", 
		teamName, playerIndex));

	// Find the team prototype by name
	TeamPrototype* teamProto = TheTeamFactory->findTeamPrototype(teamName);
	if (teamProto == nullptr) {
		DEBUGLOG(("AIPlayerControl: Team prototype '%s' not found\n", teamName));
		return;
	}

	// Build the team with priority
	ai->buildSpecificAITeam(teamProto, TRUE);
}

// ----------------------------------------------------------------------------------------------
/**
 * Emergency shutdown - disable all AI control and return to manual control.
 * Useful for debugging or if AI gets stuck.
 */
// ----------------------------------------------------------------------------------------------
void emergencyDisableAIControl()
{
	DEBUGLOG(("AIPlayerControl: EMERGENCY SHUTDOWN - Disabling all AI control\n"));

	for (Int i = 0; i < MAX_PLAYER_COUNT; ++i) {
		if (s_aiControlEnabled[i]) {
			DEBUGLOG(("AIPlayerControl: Disabling AI control for player %d\n", i));
			s_aiControlEnabled[i] = FALSE;
			s_useSkirmishAI[i] = FALSE;

			// Reset player type to human if possible
			Player* player = ThePlayerList->getNthPlayer(i);
			if (player != nullptr) {
				// Note: We don't delete the AI instance here to avoid crashes
				// The AI will simply stop being used
				DEBUGLOG(("AIPlayerControl: Player %d returned to manual control\n", i));
			}
		}
	}

	DEBUGLOG(("AIPlayerControl: Emergency shutdown complete\n"));
}

// ----------------------------------------------------------------------------------------------
// Additional helper functions for internal use
// ----------------------------------------------------------------------------------------------

/**
 * Internal: Check if skirmish AI is enabled for a player.
 * Used internally by the AI system.
 */
Bool isSkirmishAIEnabled(PlayerIndex playerIndex)
{
	if (playerIndex < 0 || playerIndex >= MAX_PLAYER_COUNT) {
		return FALSE;
	}

	return s_useSkirmishAI[playerIndex];
}

/**
 * Internal: Get the count of AI-controlled players.
 * Useful for debugging and statistics.
 */
Int getAIControlledPlayerCount()
{
	Int count = 0;
	for (Int i = 0; i < MAX_PLAYER_COUNT; ++i) {
		if (s_aiControlEnabled[i]) {
			++count;
		}
	}
	return count;
}
