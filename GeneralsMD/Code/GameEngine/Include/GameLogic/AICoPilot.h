/*
**      Command & Conquer Generals Zero Hour(tm)
**      Copyright 2025 Electronic Arts Inc.
**
**      This program is free software: you can redistribute it and/or modify
**      it under the terms of the GNU General Public License as published by
**      the Free Software Foundation, either version 3 of the License, or
**      (at your option) any later version.
**
**      This program is distributed in the hope that it will be useful,
**      but WITHOUT ANY WARRANTY; without even the implied warranty of
**      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**      GNU General Public License for more details.
**
**      You should have received a copy of the GNU General Public License
**      along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

////////////////////////////////////////////////////////////////////////////////
//                                                                                                              //
//  (c) 2001-2003 Electronic Arts Inc.                                                                          //
//                                                                                                              //
////////////////////////////////////////////////////////////////////////////////

// AICoPilot.h
// The AI Co-Pilot: an AI teammate that plays alongside the human player.
// Author: E-Vex, October 2026
//
// The AITeammatePlayer is attached to a PLAYER_HUMAN player (see
// Player::attachAITeammate). Because Player::update() ticks m_ai whenever
// it is non-null, the co-pilot "thinks" every logic frame without changing
// the player's type: the human keeps full manual control of the UI, the
// build menus and the army. The co-pilot only manages what the player
// delegated to it:
//
//   COPILOT_MODE_OFF     - detached, does nothing.
//   COPILOT_MODE_OPENING - runs the opening build sequence (supply, power,
//                          barracks, factory) and keeps those structures
//                          rebuilt when destroyed.
//   COPILOT_MODE_FULL    - opening + continuous unit production + base
//                          perimeter defense + economy upkeep.
//
// Fair play: the co-pilot only reacts to information the player already has
// (own money, power, queues, and damage dealt to own units/structures). It
// never scans hidden enemy positions through the fog of war. All buildings
// are placed with the player's real dozers at full cost and real build time.

#pragma once

#include "Common/GameMemory.h"
#include "Common/KindOf.h"
#include "Common/Snapshot.h"
#include "GameLogic/AIPlayer.h"

//-------------------------------------------------------------------------------------------------
/// Level of control the human player has delegated to the AI Co-Pilot.
//-------------------------------------------------------------------------------------------------
enum AICoPilotMode CPP_11(: Int)
{
	COPILOT_MODE_OFF = 0,      ///< Co-pilot is disabled.
	COPILOT_MODE_OPENING,      ///< Manages the opening build order and rebuilds.
	COPILOT_MODE_FULL,         ///< Opening + production + base defense.

	COPILOT_MODE_COUNT
};

//-------------------------------------------------------------------------------------------------
/**
 * The AI teammate. An AIPlayer subclass that works FOR a human player
 * instead of against them. See file comment for the design.
 */
//-------------------------------------------------------------------------------------------------
class AITeammatePlayer : public AIPlayer
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE( AITeammatePlayer, "AITeammatePlayer" )

public:

	AITeammatePlayer( Player *p );                                                        ///< constructor
	// (destructor is declared by the memory-pool glue, as in AIPlayer)

public: // AIPlayer interface methods.

	virtual void update() override;                                                       ///< simulates the behavior of a player
	virtual void newMap() override;                                                       ///< new map loaded call
	virtual void onStructureProduced( Object *factory, Object *structure ) override;      ///< human finished a building
	virtual void onUnitProduced( Object *factory, Object *unit ) override;                ///< a unit came out of a factory
	virtual Bool isSkirmishAI() override { return false; }                                ///< we are not the skirmish AI

public: // Co-Pilot specific interface.

	void setMode( AICoPilotMode mode );                                                   ///< change level of control
	AICoPilotMode getMode() const { return (AICoPilotMode)m_mode; }
	void cycleMode();                                                                     ///< OFF -> OPENING -> FULL -> OFF

	Bool isInitialized() const { return m_initialized; }
	void ensureInitialized();                                                             ///< lazy init when attached after newMap

protected:

	// snapshot methods
	virtual void crc( Xfer *xfer ) override;
	virtual void xfer( Xfer *xfer ) override;
	virtual void loadPostProcess() override;

protected: // behavior

	void updateOpeningSequence();                                                         ///< advance the opening build order state machine
	void updateProduction();                                                              ///< keep factories busy (FULL mode only)
	void updateBaseDefense();                                                             ///< defend the base perimeter (FULL mode only)
	void updateEconomy();                                                                 ///< dozer/harvester upkeep

protected: // helpers

	const ThingTemplate *findCheapestBuildableStructure( KindOfType structureKind ) const;///< buildable structure template for our faction
	const ThingTemplate *findCheapestBuildableUnit( KindOfType unitKind ) const;          ///< buildable unit template for our faction
	Int countPlayerStructuresOfKind( KindOfType structureKind ) const;                    ///< how many of this structure kind do we own
	Bool isTemplateInBuildList( const ThingTemplate *thing ) const;                       ///< is this already queued/built via build list
	Bool isUnitTypeInProductionQueues( const ThingTemplate *thing ) const;                ///< is a unit of this type waiting in a factory
	Bool queueOpeningStructure( KindOfType structureKind, Object *nearObject );           ///< add an opening structure to the build list
	Object *findNearestSupplyWarehouse() const;                                           ///< nearest supply source (neutral warehouse)
	void orderUnitsToDefend( Object *attacker, const Coord3D &position );                 ///< send nearby idle units to defend
	void notifyPlayer( const WideChar *text );                                            ///< in-game co-pilot message
	Bool canAffordReserve() const;                                                        ///< respect the money reserve for the player

protected:

	Int             m_mode;                                                               ///< AICoPilotMode
	Bool            m_initialized;                                                        ///< newMap / lazy init ran
	Bool            m_onlineAnnounced;                                                   ///< "co-pilot online" message sent
	Int             m_openingStep;                                                        ///< current step of the opening sequence
	Bool            m_openingAnnounced;                                                   ///< "opening complete" message sent

	UnsignedInt     m_nextOpeningCheckFrame;                                              ///< throttle for the opening state machine
	UnsignedInt     m_nextProductionCheckFrame;                                           ///< throttle for production
	UnsignedInt     m_nextDefenseCheckFrame;                                              ///< throttle for defense scan
	UnsignedInt     m_nextEconomyCheckFrame;                                              ///< throttle for economy upkeep
	UnsignedInt     m_lastPlayerNotifyFrame;                                              ///< don't spam the player with messages
	ObjectID        m_lastDefenseAttackerID;                                              ///< attacker of the last defense wave

	// TheSuperHackers @feature E-Vex 16/10/2026 AI Co-Pilot tuning values. These are
	// deliberately conservative so the co-pilot helps the player without starving them.
	enum
	{
		COPILOT_SECONDS_BETWEEN_CHECKS      = 2,     ///< how often opening/production logic runs
		COPILOT_DEFENSE_SCAN_SECONDS        = 1,     ///< how often we look for attacks on our base
		COPILOT_DAMAGE_RECENT_SECONDS       = 2,     ///< damage newer than this counts as "under attack"
		COPILOT_DEFENSE_NOTIFY_COOLDOWN     = 20,    ///< seconds between "under attack" messages
		COPILOT_MAX_DEFENDERS               = 6,     ///< how many units we redirect at once
		COPILOT_MONEY_RESERVE               = 300,   ///< money we leave for the human player
		COPILOT_DESIRED_GATHERERS_PER_CENTER= 3,     ///< harvesters per supply center
		COPILOT_MIN_DEFENSE_DEFENDERS       = 1,     ///< only respond if we can send at least 1 unit
	};
};
