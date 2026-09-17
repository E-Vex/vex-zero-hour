/*
 * AI Player Control System
 * 
 * This module enables the internal AI system to control what would normally
 * be a human-controlled player. This allows the game's existing AI systems
 * to operate as a normal playable participant.
 * 
 * Usage:
 * - Call enableAIControl(playerIndex, true) to give control to AI
 * - Call enableAIControl(playerIndex, false) to return control to human
 * 
 * The AI will handle:
 * - Base building and structure placement
 * - Unit production and team management
 * - Resource gathering and economy
 * - Combat decisions and attacks
 * - Upgrade research
 * - Special power usage
 */

#pragma once

#include "Common/GameCommon.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "GameLogic/AIPlayer.h"
#include "GameLogic/AISkirmishPlayer.h"

/**
 * Enable or disable AI control for a specific player.
 * 
 * @param playerIndex The index of the player to control (0-based)
 * @param enableAI True to enable AI control, false to return to human control
 * @param useSkirmishAI True to use the advanced skirmish AI, false for basic AI
 */
void enableAIControl(PlayerIndex playerIndex, Bool enableAI, Bool useSkirmishAI = true);

/**
 * Check if a player is currently under AI control.
 * 
 * @param playerIndex The index of the player to check
 * @return True if AI is controlling this player
 */
Bool isAIControlled(PlayerIndex playerIndex);

/**
 * Get the AI player instance for a specific player.
 * 
 * @param playerIndex The index of the player
 * @return Pointer to the AIPlayer instance, or nullptr if not AI-controlled
 */
AIPlayer* getAIForPlayer(PlayerIndex playerIndex);

/**
 * Configure AI behavior parameters.
 * 
 * @param playerIndex The index of the player
 * @param teamDelaySeconds Delay between team creation (default: 10)
 * @param difficulty AI difficulty level
 */
void configureAI(PlayerIndex playerIndex, Int teamDelaySeconds, GameDifficulty difficulty);

/**
 * Force AI to build a specific building immediately.
 * 
 * @param playerIndex The index of the player
 * @param buildingName Internal name of the building to construct
 */
void aiBuildBuilding(PlayerIndex playerIndex, const char* buildingName);

/**
 * Force AI to build a specific team immediately.
 * 
 * @param playerIndex The index of the player
 * @param teamName Internal name of the team prototype to build
 */
void aiBuildTeam(PlayerIndex playerIndex, const char* teamName);

/**
 * Emergency shutdown - disable all AI control and return to manual control.
 * Useful for debugging or if AI gets stuck.
 */
void emergencyDisableAIControl();
