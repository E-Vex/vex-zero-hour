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

// AICoPilot.cpp
// The AI Co-Pilot: an AI teammate that plays alongside the human player.
// Author: E-Vex, October 2026
//
// See AICoPilot.h for the design. The co-pilot is an AIPlayer subclass
// attached to a human player (Player::attachAITeammate). It reuses the
// base class dozer-construction machinery (doBaseBuilding) to build and
// rebuild its opening sequence, and adds production, defense and economy
// upkeep on top of it. It deliberately never touches team-based attack
// scripts: attacking the enemy stays the human player's decision.

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"     // This must go first in EVERY cpp file in the GameEngine
#include "Common/GameMemory.h"
#include "Common/GlobalData.h"
#include "Common/PerfTimer.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/BuildAssistant.h"
#include "Common/ThingFactory.h"
#include "Common/ThingTemplate.h"
#include "Common/Xfer.h"
#include "GameClient/InGameUI.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/AICoPilot.h"
#include "GameLogic/SidesList.h"
#include "GameLogic/AI.h"
#include "GameLogic/AIPathfind.h"
#include "GameLogic/TerrainLogic.h"
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Module/BodyModule.h"
#include "GameLogic/Module/ProductionUpdate.h"
#include "GameLogic/ScriptEngine.h"
#include "GameLogic/Weapon.h"

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
AITeammatePlayer::AITeammatePlayer( Player *p ) :
	AIPlayer( p ),
	m_mode( COPILOT_MODE_FULL ),
	m_initialized( false ),
	m_onlineAnnounced( false ),
	m_openingStep( 0 ),
	m_openingAnnounced( false ),
	m_nextOpeningCheckFrame( 0 ),
	m_nextProductionCheckFrame( 0 ),
	m_nextDefenseCheckFrame( 0 ),
	m_nextEconomyCheckFrame( 0 ),
	m_lastPlayerNotifyFrame( 0 ),
	m_lastDefenseAttackerID( INVALID_ID )
{
	// TheSuperHackers @feature E-Vex 16/10/2026 AI Co-Pilot.
	// The AIPlayer constructor disables unit production (the skirmish AI enables it
	// via script actions). The co-pilot serves a human player: production must stay
	// enabled so the player's own queue keeps working.
	p->setCanBuildUnits( true );

	// The teammate attaches during game start (GameLogic::tryStartNewGame) or via the
	// F9 hotkey; both happen before/around newMap. Lazy-init in update() covers any
	// ordering case.
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
AITeammatePlayer::~AITeammatePlayer()
{
	// Nothing to clean up; the build list entries belong to the Player and are
	// cleaned up with it. Re-enable normal human production on the way out.
	if (m_player)
		m_player->setCanBuildUnits( true );
}

// ------------------------------------------------------------------------------------------------
/** Change the level of control delegated to the co-pilot. */
// ------------------------------------------------------------------------------------------------
void AITeammatePlayer::setMode( AICoPilotMode mode )
{
	if (mode == COPILOT_MODE_COUNT)
		return;

	if ((AICoPilotMode)m_mode == mode)
		return;

	m_mode = (Int)mode;

	if (m_player)
	{
		// Human production must never be gated by the co-pilot.
		m_player->setCanBuildUnits( true );
	}

	switch (mode)
	{
		case COPILOT_MODE_OFF:
			notifyPlayer( L"Co-Pilot: standing down. You have full control." );
			break;
		case COPILOT_MODE_OPENING:
			notifyPlayer( L"Co-Pilot: managing the opening build order and base rebuilds." );
			break;
		case COPILOT_MODE_FULL:
			notifyPlayer( L"Co-Pilot: full assistance - base, production and perimeter defense." );
			break;
		default:
			break;
	}
}

// ------------------------------------------------------------------------------------------------
/** OFF -> OPENING -> FULL -> OFF. Used by the F9 hotkey. */
// ------------------------------------------------------------------------------------------------
void AITeammatePlayer::cycleMode()
{
	AICoPilotMode next = (AICoPilotMode)(((Int)m_mode + 1) % COPILOT_MODE_COUNT);
	setMode( next );
}

// ------------------------------------------------------------------------------------------------
/** New map. Adopt the player's existing base instead of (re)building map leftovers. */
// ------------------------------------------------------------------------------------------------
void AITeammatePlayer::newMap()
{
	// TheSuperHackers @feature E-Vex 16/10/2026 AI Co-Pilot.
	// Deliberately NOT calling AIPlayer::newMap(): it instant-builds all entries of the
	// side's build list that are flagged "initially built", which would duplicate
	// structures the skirmish map already placed for the human. The co-pilot builds
	// with real dozers at real cost only (fair play).
	m_initialized = false;
	ensureInitialized();
}

// ------------------------------------------------------------------------------------------------
/** One-time adoption of the player's current base. Idempotent. */
// ------------------------------------------------------------------------------------------------
void AITeammatePlayer::ensureInitialized()
{
	if (m_initialized)
		return;

	m_initialized = true;
	m_nextOpeningCheckFrame = TheGameLogic->getFrame() + 2 * LOGICFRAMES_PER_SECOND;
	m_nextProductionCheckFrame = m_nextOpeningCheckFrame;
	m_nextDefenseCheckFrame = TheGameLogic->getFrame() + LOGICFRAMES_PER_SECOND;
	m_nextEconomyCheckFrame = m_nextOpeningCheckFrame;

	// Track every production building the player already owns, so findFactory() (and
	// therefore unit training) sees the human's real factories. This mirrors what
	// AIPlayer::newMap does for AI players.
	Object *obj;
	for ( obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject() )
	{
		if (obj->getControllingPlayer() != m_player)
			continue;
		if (obj->isEffectivelyDead())
			continue;
		ProductionUpdateInterface *pu = obj->getProductionUpdateInterface();
		if (!pu)
			continue;
		// Avoid duplicate tracking when newMap runs more than once.
		Bool alreadyTracked = false;
		for ( BuildListInfo *info = m_player->getBuildList(); info; info = info->getNext() )
		{
			if (info->getObjectID() == obj->getID())
			{
				alreadyTracked = true;
				break;
			}
		}
		if (!alreadyTracked)
			m_player->addToBuildList( obj );
	}

	// Compute the base center/radius from the structures the player actually owns.
	Coord3D center;
	center.zero();
	Real radius = 0;
	Int numStructures = 0;
	for ( obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject() )
	{
		if (obj->getControllingPlayer() != m_player)
			continue;
		if (!obj->isKindOf( KINDOF_STRUCTURE ))
			continue;
		if (obj->isEffectivelyDead())
			continue;
		if (obj->isKindOf( KINDOF_FS_FAKE ))
			continue;
		const Coord3D *pos = obj->getPosition();
		center.x += pos->x;
		center.y += pos->y;
		numStructures++;
	}
	if (numStructures > 0)
	{
		center.x /= numStructures;
		center.y /= numStructures;
		m_baseCenter = center;
		m_baseCenterSet = true;

		for ( obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject() )
		{
			if (obj->getControllingPlayer() != m_player)
				continue;
			if (!obj->isKindOf( KINDOF_STRUCTURE ))
				continue;
			if (obj->isEffectivelyDead() || obj->isKindOf( KINDOF_FS_FAKE ))
				continue;
			const Coord3D *pos = obj->getPosition();
			Real dx = pos->x - center.x;
			Real dy = pos->y - center.y;
			Real radSqr = dx * dx + dy * dy;
			if (radSqr > radius * radius)
				radius = sqrt( radSqr );
		}
		if (radius < 5 * PATHFIND_CELL_SIZE_F)
			radius = 5 * PATHFIND_CELL_SIZE_F;
		m_baseRadius = radius;
	}
}

// ------------------------------------------------------------------------------------------------
/** Main think loop. Ticked from Player::update() every logic frame. */
// ------------------------------------------------------------------------------------------------
void AITeammatePlayer::update()
{
	if ((AICoPilotMode)m_mode == COPILOT_MODE_OFF)
		return;

	ensureInitialized();

	if (!m_onlineAnnounced)
	{
		// Announce on the first live logic frame, when the in-game UI is ready.
		m_onlineAnnounced = true;
		notifyPlayer( L"Co-Pilot online: I'll manage the base while you fight. Press F9 to change my orders." );
	}

	// The opening sequence queues structures into the player's build list; the
	// inherited doBaseBuilding() orders our dozers to construct (and re-construct)
	// those entries at full cost, respecting the structure delay timers.
	updateOpeningSequence();
	doBaseBuilding();

	if ((AICoPilotMode)m_mode == COPILOT_MODE_FULL)
	{
		updateEconomy();
		updateProduction();
		updateBaseDefense();
	}
}

// ------------------------------------------------------------------------------------------------
/** The opening build sequence: supply -> power -> barracks -> 2nd supply -> factory. */
// ------------------------------------------------------------------------------------------------
void AITeammatePlayer::updateOpeningSequence()
{
	if (m_openingAnnounced)
		return;

	UnsignedInt frame = TheGameLogic->getFrame();
	if (frame < m_nextOpeningCheckFrame)
		return;
	m_nextOpeningCheckFrame = frame + COPILOT_SECONDS_BETWEEN_CHECKS * LOGICFRAMES_PER_SECOND;

	// Steps advance only when the previous goal is standing. Destroyed buildings are
	// handled by the build list rebuild machinery (UNLIMITED_REBUILDS), not by
	// re-running steps.
	enum
	{
		STEP_SUPPLY_ONE = 0,
		STEP_POWER_ONE,
		STEP_BARRACKS,
		STEP_SUPPLY_TWO,
		STEP_WAR_FACTORY,
		STEP_DONE
	};

	while (m_openingStep < STEP_DONE)
	{
		switch (m_openingStep)
		{
			case STEP_SUPPLY_ONE:
			case STEP_SUPPLY_TWO:
			{
				Int supplyCount = countPlayerStructuresOfKind( KINDOF_FS_SUPPLY_CENTER );
				Int needed = (m_openingStep == STEP_SUPPLY_ONE) ? 1 : 2;
				if (supplyCount >= needed)
				{
					m_openingStep++;
					continue;
				}
				// Only queue a second supply if another reachable supply source exists.
				if (m_openingStep == STEP_SUPPLY_TWO && findNearestSupplyWarehouse() == nullptr)
				{
					m_openingStep++;
					continue;
				}
				if (queueOpeningStructure( KINDOF_FS_SUPPLY_CENTER, findNearestSupplyWarehouse() ))
				{
					DEBUG_LOG(( "AI Co-Pilot: queued supply center (step %d)\n", m_openingStep ));
				}
				return;
			}

			case STEP_POWER_ONE:
			{
				// GLA has no power plants.
				if (m_player->getSide() == "GLA")
				{
					m_openingStep++;
					continue;
				}
				// The opening always adds one power plant: the factory we are
				// about to build will need the headroom, even if the command
				// center currently keeps us in the green.
				if (countPlayerStructuresOfKind( KINDOF_FS_POWER ) > 0)
				{
					m_openingStep++;
					continue;
				}
				if (queueOpeningStructure( KINDOF_FS_POWER, nullptr ))
				{
					DEBUG_LOG(( "AI Co-Pilot: queued power plant\n" ));
				}
				return;
			}

			case STEP_BARRACKS:
			{
				if (countPlayerStructuresOfKind( KINDOF_FS_BARRACKS ) > 0)
				{
					m_openingStep++;
					continue;
				}
				if (queueOpeningStructure( KINDOF_FS_BARRACKS, nullptr ))
				{
					DEBUG_LOG(( "AI Co-Pilot: queued barracks\n" ));
				}
				return;
			}

			case STEP_WAR_FACTORY:
			{
				if (countPlayerStructuresOfKind( KINDOF_FS_WARFACTORY ) > 0)
				{
					m_openingStep++;
					continue;
				}
				if (queueOpeningStructure( KINDOF_FS_WARFACTORY, nullptr ))
				{
					DEBUG_LOG(( "AI Co-Pilot: queued war factory\n" ));
				}
				return;
			}

			default:
				m_openingStep = STEP_DONE;
				break;
		}
	}

	if (!m_openingAnnounced)
	{
		m_openingAnnounced = true;
		if ((AICoPilotMode)m_mode == COPILOT_MODE_FULL)
			notifyPlayer( L"Co-Pilot: opening complete. Managing production, rebuilds and defense - the attacks are yours." );
		else
			notifyPlayer( L"Co-Pilot: opening complete. I'll keep the base in shape." );
	}
}

// ------------------------------------------------------------------------------------------------
/** Keep the player's factories busy with a cheap, balanced unit mix. */
// ------------------------------------------------------------------------------------------------
void AITeammatePlayer::updateProduction()
{
	UnsignedInt frame = TheGameLogic->getFrame();
	if (frame < m_nextProductionCheckFrame)
		return;
	m_nextProductionCheckFrame = frame + COPILOT_SECONDS_BETWEEN_CHECKS * LOGICFRAMES_PER_SECOND;

	if (!canAffordReserve())
		return;

	// One infantry and one vehicle per cycle at idle factories. findFactory() only
	// returns factories that can actually produce the requested thing, and
	// queueCreateUnit() charges the player full price.
	const ThingTemplate *unit = findCheapestBuildableUnit( KINDOF_INFANTRY );
	if (unit)
	{
		Object *factory = findFactory( unit, false );
		if (factory)
		{
			ProductionUpdateInterface *pu = factory->getProductionUpdateInterface();
			if (pu)
				pu->queueCreateUnit( unit, pu->requestUniqueUnitID() );
		}
	}

	unit = findCheapestBuildableUnit( KINDOF_VEHICLE );
	if (unit && !unit->isKindOf( KINDOF_DOZER ) && !unit->isKindOf( KINDOF_HARVESTER ))
	{
		Object *factory = findFactory( unit, false );
		if (factory)
		{
			ProductionUpdateInterface *pu = factory->getProductionUpdateInterface();
			if (pu)
				pu->queueCreateUnit( unit, pu->requestUniqueUnitID() );
		}
	}
}

// ------------------------------------------------------------------------------------------------
/** Dozer and harvester upkeep so the player never has to micro the economy. */
// ------------------------------------------------------------------------------------------------
void AITeammatePlayer::updateEconomy()
{
	UnsignedInt frame = TheGameLogic->getFrame();
	if (frame < m_nextEconomyCheckFrame)
		return;
	m_nextEconomyCheckFrame = frame + COPILOT_SECONDS_BETWEEN_CHECKS * LOGICFRAMES_PER_SECOND;

	if (!canAffordReserve())
		return;

	// Count dozers and harvesters that are alive.
	Int dozerCount = 0;
	Int harvesterCount = 0;
	Object *obj;
	for ( obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject() )
	{
		if (obj->getControllingPlayer() != m_player)
			continue;
		if (obj->isEffectivelyDead())
			continue;
		if (obj->isKindOf( KINDOF_DOZER ))
			dozerCount++;
		else if (obj->isKindOf( KINDOF_HARVESTER ))
			harvesterCount++;
	}

	// If the player lost every dozer, the co-pilot's whole build plan stalls. Get a
	// replacement out as a priority.
	if (dozerCount == 0)
	{
		const ThingTemplate *dozer = findCheapestBuildableUnit( KINDOF_DOZER );
		if (dozer && !isUnitTypeInProductionQueues( dozer ))
		{
			Object *factory = findFactory( dozer, true );
			if (factory)
			{
				ProductionUpdateInterface *pu = factory->getProductionUpdateInterface();
				if (pu && pu->queueCreateUnit( dozer, pu->requestUniqueUnitID() ))
				{
					if (frame - m_lastPlayerNotifyFrame > 10 * LOGICFRAMES_PER_SECOND)
					{
						notifyPlayer( L"Co-Pilot: our dozer was lost. Building a replacement." );
						m_lastPlayerNotifyFrame = frame;
					}
				}
			}
		}
		return;
	}

	// Keep the harvesters stocked relative to the number of supply centers. One
	// replacement at a time; more would eat the player's money.
	Int supplyCenters = countPlayerStructuresOfKind( KINDOF_FS_SUPPLY_CENTER );
	Int desiredGatherers = supplyCenters * COPILOT_DESIRED_GATHERERS_PER_CENTER;
	if (harvesterCount < desiredGatherers)
	{
		const ThingTemplate *harvester = findCheapestBuildableUnit( KINDOF_HARVESTER );
		if (harvester && !isUnitTypeInProductionQueues( harvester ))
		{
			Object *factory = findFactory( harvester, false );
			if (factory)
			{
				ProductionUpdateInterface *pu = factory->getProductionUpdateInterface();
				if (pu)
					pu->queueCreateUnit( harvester, pu->requestUniqueUnitID() );
			}
		}
	}
}

// ------------------------------------------------------------------------------------------------
/** Defend the base perimeter. Strict fair play: only reacts to damage our own
 *  units and structures actually received; never reveals hidden enemies. */
// ------------------------------------------------------------------------------------------------
void AITeammatePlayer::updateBaseDefense()
{
	UnsignedInt frame = TheGameLogic->getFrame();
	if (frame < m_nextDefenseCheckFrame)
		return;
	m_nextDefenseCheckFrame = frame + COPILOT_DEFENSE_SCAN_SECONDS * LOGICFRAMES_PER_SECOND;

	// Find the most recent attack against one of our structures.
	Object *attacker = nullptr;
	Coord3D attackPos;
	attackPos.zero();
	Object *obj;
	for ( obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject() )
	{
		if (obj->getControllingPlayer() != m_player)
			continue;
		if (!obj->isKindOf( KINDOF_STRUCTURE ))
			continue;
		if (obj->isEffectivelyDead())
			continue;

		BodyModuleInterface *body = obj->getBodyModule();
		if (!body)
			continue;

		UnsignedInt damageTimestamp = body->getLastDamageTimestamp();
		if (damageTimestamp == 0)
			continue;
		if (frame - damageTimestamp > COPILOT_DAMAGE_RECENT_SECONDS * LOGICFRAMES_PER_SECOND)
			continue;

		ObjectID attackerID = body->getClearableLastAttacker();
		Object *candidate = TheGameLogic->findObjectByID( attackerID );
		if (candidate == nullptr || candidate->isEffectivelyDead())
			continue;
		if (m_player->getRelationship( candidate->getTeam() ) != ENEMIES)
			continue;

		attacker = candidate;
		attackPos = *obj->getPosition();
		break;
	}

	if (attacker == nullptr)
		return;

	Bool newWave = (attacker->getID() != m_lastDefenseAttackerID);
	Bool cooldownOver = (frame - m_lastPlayerNotifyFrame) > COPILOT_DEFENSE_NOTIFY_COOLDOWN * LOGICFRAMES_PER_SECOND;
	if (newWave || cooldownOver)
	{
		// TheSuperHackers @feature E-Vex 16/10/2026 AI Co-Pilot.
		// Notify the human the way a teammate would: we handle it, but ask for help
		// if it gets serious. Severity is measured with our own losses only.
		notifyPlayer( L"Co-Pilot: we're under attack near our base. Sending defenders - I may need your help." );
		m_lastPlayerNotifyFrame = frame;
		m_lastDefenseAttackerID = attacker->getID();
	}

	orderUnitsToDefend( attacker, attackPos );
}

// ------------------------------------------------------------------------------------------------
/** Redirect nearby idle units to fight an attacker. Never touches units the player
 *  is actively commanding (isIdle() check). */
// ------------------------------------------------------------------------------------------------
void AITeammatePlayer::orderUnitsToDefend( Object *attacker, const Coord3D &position )
{
	Real respondRadius = m_baseRadius * 2.0f;
	if (respondRadius < 60.0f)
		respondRadius = 60.0f;

	Int sent = 0;
	Object *obj;
	for ( obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject() )
	{
		if (sent >= COPILOT_MAX_DEFENDERS)
			break;
		if (obj->getControllingPlayer() != m_player)
			continue;
		if (obj->isEffectivelyDead())
			continue;
		if (!obj->isKindOf( KINDOF_CAN_ATTACK ))
			continue;
		// Never micro the economy or the builders.
		if (obj->isKindOf( KINDOF_DOZER ) || obj->isKindOf( KINDOF_HARVESTER ))
			continue;
		// For now the co-pilot only commands ground units; aircraft handling can come later.
		if (obj->isKindOf( KINDOF_AIRCRAFT ))
			continue;

		AIUpdateInterface *ai = obj->getAI();
		if (!ai || !ai->isIdle())
			continue;

		const Coord3D *unitPos = obj->getPosition();
		Real dx = unitPos->x - position.x;
		Real dy = unitPos->y - position.y;
		if (dx * dx + dy * dy > respondRadius * respondRadius)
			continue;

		ai->aiAttackObject( attacker, NO_MAX_SHOTS_LIMIT, CMD_FROM_AI );
		sent++;
	}

	if (sent < COPILOT_MIN_DEFENSE_DEFENDERS)
	{
		// Nobody idle close enough: pull idle units from a wider ring and park them
		// on guard at the hot spot so the next wave meets resistance. The radius is
		// still limited so we never recall a staging army from across the map.
		Real recallRadius = respondRadius * 2.0f;
		for ( obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject() )
		{
			if (sent >= 2)
				break;
			if (obj->getControllingPlayer() != m_player)
				continue;
			if (obj->isEffectivelyDead())
				continue;
			if (!obj->isKindOf( KINDOF_CAN_ATTACK ))
				continue;
			if (obj->isKindOf( KINDOF_DOZER ) || obj->isKindOf( KINDOF_HARVESTER ) || obj->isKindOf( KINDOF_AIRCRAFT ))
				continue;

			AIUpdateInterface *ai = obj->getAI();
			if (!ai || !ai->isIdle())
				continue;

			const Coord3D *unitPos = obj->getPosition();
			Real dx = unitPos->x - position.x;
			Real dy = unitPos->y - position.y;
			if (dx * dx + dy * dy > recallRadius * recallRadius)
				continue;

			Coord3D guardPos = position;
			ai->aiGuardPosition( &guardPos, GUARDMODE_NORMAL, CMD_FROM_AI );
			sent++;
		}
	}
}

// ------------------------------------------------------------------------------------------------
/** Track buildings the human completes so training sees the real factories. */
// ------------------------------------------------------------------------------------------------
void AITeammatePlayer::onStructureProduced( Object *factory, Object *structure )
{
	// TheSuperHackers @feature E-Vex 16/10/2026 AI Co-Pilot.
	// Don't call AIPlayer::onStructureProduced(): it matches the new structure against
	// build list entries and runs script attachments meant for AI sides. We only need
	// to track the new factory for production, with a rebuild count of zero: buildings
	// the player placed are the player's decision, the co-pilot won't auto-rebuild them.
	if (structure == nullptr || structure->getControllingPlayer() != m_player)
		return;

	if (structure->getProductionUpdateInterface() != nullptr)
	{
		Bool alreadyTracked = false;
		for ( BuildListInfo *info = m_player->getBuildList(); info; info = info->getNext() )
		{
			if (info->getObjectID() == structure->getID())
			{
				alreadyTracked = true;
				break;
			}
		}
		if (!alreadyTracked)
			m_player->addToBuildList( structure );
	}
}

// ------------------------------------------------------------------------------------------------
/** A unit emerged from one of our factories. */
// ------------------------------------------------------------------------------------------------
void AITeammatePlayer::onUnitProduced( Object *factory, Object *unit )
{
	// Nothing to do yet; the unit is left completely under the human's control.
}

// ------------------------------------------------------------------------------------------------
/** Cheapest structure of the given kind this player's faction can currently build. */
// ------------------------------------------------------------------------------------------------
const ThingTemplate *AITeammatePlayer::findCheapestBuildableStructure( KindOfType structureKind ) const
{
	const ThingTemplate *best = nullptr;
	Int bestCost = 0;

	const ThingTemplate *t;
	for ( t = TheThingFactory->firstTemplate(); t; t = t->friend_getNextTemplate() )
	{
		if (!t->isKindOf( structureKind ))
			continue;
		if (!t->isKindOf( KINDOF_STRUCTURE ))
			continue;
		if (t->isKindOf( KINDOF_FS_FAKE ))
			continue;
		if (!m_player->canBuild( t ))
			continue;
		Int cost = t->calcCostToBuild( m_player );
		if (best == nullptr || cost < bestCost)
		{
			best = t;
			bestCost = cost;
		}
	}

	return best;
}

// ------------------------------------------------------------------------------------------------
/** Cheapest buildable unit of the given kind (skips heroes - those are the player's call). */
// ------------------------------------------------------------------------------------------------
const ThingTemplate *AITeammatePlayer::findCheapestBuildableUnit( KindOfType unitKind ) const
{
	const ThingTemplate *best = nullptr;
	Int bestCost = 0;

	const ThingTemplate *t;
	for ( t = TheThingFactory->firstTemplate(); t; t = t->friend_getNextTemplate() )
	{
		if (!t->isKindOf( unitKind ))
			continue;
		if (t->isKindOf( KINDOF_HERO ))
			continue;
		if (t->isKindOf( KINDOF_STRUCTURE ))
			continue;
		if (!m_player->canBuild( t ))
			continue;
		Int cost = t->calcCostToBuild( m_player );
		if (best == nullptr || cost < bestCost)
		{
			best = t;
			bestCost = cost;
		}
	}

	return best;
}

// ------------------------------------------------------------------------------------------------
/** Count standing structures of a kind owned by our player. */
// ------------------------------------------------------------------------------------------------
Int AITeammatePlayer::countPlayerStructuresOfKind( KindOfType structureKind ) const
{
	Int count = 0;
	Object *obj;
	for ( obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject() )
	{
		if (obj->getControllingPlayer() != m_player)
			continue;
		if (!obj->isKindOf( structureKind ))
			continue;
		if (obj->isEffectivelyDead())
			continue;
		count++;
	}
	return count;
}

// ------------------------------------------------------------------------------------------------
/** Is a unit of this type already waiting in one of our factories? */
// ------------------------------------------------------------------------------------------------
Bool AITeammatePlayer::isUnitTypeInProductionQueues( const ThingTemplate *thing ) const
{
	if (thing == nullptr)
		return false;

	for ( BuildListInfo *info = m_player->getBuildList(); info; info = info->getNext() )
	{
		Object *factory = TheGameLogic->findObjectByID( info->getObjectID() );
		if (factory == nullptr)
			continue;
		ProductionUpdateInterface *pu = factory->getProductionUpdateInterface();
		if (pu == nullptr)
			continue;
		if (pu->countUnitTypeInQueue( thing ) > 0)
			return true;
	}
	return false;
}

// ------------------------------------------------------------------------------------------------
/** Is a template of this kind already represented in the player's build list? */
// ------------------------------------------------------------------------------------------------
Bool AITeammatePlayer::isTemplateInBuildList( const ThingTemplate *thing ) const
{
	if (thing == nullptr)
		return false;

	for ( BuildListInfo *info = m_player->getBuildList(); info; info = info->getNext() )
	{
		if (info->getTemplateName().isEmpty())
			continue;
		const ThingTemplate *listed = TheThingFactory->findTemplate( info->getTemplateName() );
		if (listed && listed->isEquivalentTo( thing ))
			return true;
	}
	return false;
}

// ------------------------------------------------------------------------------------------------
/** Add one opening structure to the player's build list. Unlimited rebuilds. */
// ------------------------------------------------------------------------------------------------
Bool AITeammatePlayer::queueOpeningStructure( KindOfType structureKind, Object *nearObject )
{
	const ThingTemplate *thing = findCheapestBuildableStructure( structureKind );
	if (thing == nullptr)
		return false;

	if (isTemplateInBuildList( thing ))
		return false;

	Coord3D location;
	location.zero();

	if (nearObject != nullptr)
	{
		// Supply centers go next to the supply source, nudged toward our base.
		location = *nearObject->getPosition();
		if (m_baseCenterSet)
		{
			Coord2D offset;
			offset.x = m_baseCenter.x - location.x;
			offset.y = m_baseCenter.y - location.y;
			if (offset.x != 0.0f || offset.y != 0.0f)
				offset.normalize();
			Real keepAway = 3 * PATHFIND_CELL_SIZE_F;
			location.x -= offset.x * keepAway;
			location.y -= offset.y * keepAway;
		}
	}
	else
	{
		// Everything else clusters around the base center. Without any owned
		// structure we have no anchor; the step retries once the player has one.
		if (!m_baseCenterSet)
			return false;

		Coord3D base = m_baseCenter;
		if (!calcClosestConstructionZoneLocation( thing, &base ))
		{
			// couldn't find a construction zone; the dozer placement pass will wiggle it.
			base = m_baseCenter;
		}
		location = base;
	}

	location.z = TheTerrainLogic->getGroundHeight( location.x, location.y );

	m_player->addAITeammateBuildListEntry( thing->getName(), location, thing->getPlacementViewAngle(), TRUE );
	return true;
}

// ------------------------------------------------------------------------------------------------
/** Nearest neutral supply warehouse (the piles harvesters dock at). Fair play: this
 *  is the same information the game shows every player on the minimap. */
// ------------------------------------------------------------------------------------------------
Object *AITeammatePlayer::findNearestSupplyWarehouse() const
{
	// Reuse the base class helper that finds the best supply center to build near.
	// It is non-const in the base class; the scan itself only reads world state.
	return const_cast<AITeammatePlayer *>( this )->findSupplyCenter( 0 );
}

// ------------------------------------------------------------------------------------------------
/** Send a message to the player, with a small spam guard. */
// ------------------------------------------------------------------------------------------------
void AITeammatePlayer::notifyPlayer( const WideChar *text )
{
	if (text == nullptr)
		return;

	if (TheInGameUI)
		TheInGameUI->message( UnicodeString( text ) );
}

// ------------------------------------------------------------------------------------------------
/** The co-pilot never spends the last of the player's money. */
// ------------------------------------------------------------------------------------------------
Bool AITeammatePlayer::canAffordReserve() const
{
	return m_player->getMoney()->countMoney() > COPILOT_MONEY_RESERVE;
}

// ------------------------------------------------------------------------------------------------
/** snapshot: crc */
// ------------------------------------------------------------------------------------------------
void AITeammatePlayer::crc( Xfer *xfer )
{
	AIPlayer::crc( xfer );
}

// ------------------------------------------------------------------------------------------------
/** snapshot: save/load the co-pilot state so save games keep the teammate. */
// ------------------------------------------------------------------------------------------------
void AITeammatePlayer::xfer( Xfer *xfer )
{
	AIPlayer::xfer( xfer );

	// version
	XferVersion currentVersion = 1;
	XferVersion version = currentVersion;
	xfer->xferVersion( &version, currentVersion );

	xfer->xferInt( &m_mode );
	xfer->xferBool( &m_initialized );
	xfer->xferBool( &m_onlineAnnounced );
	xfer->xferInt( &m_openingStep );
	xfer->xferBool( &m_openingAnnounced );
	xfer->xferUnsignedInt( &m_nextOpeningCheckFrame );
	xfer->xferUnsignedInt( &m_nextProductionCheckFrame );
	xfer->xferUnsignedInt( &m_nextDefenseCheckFrame );
	xfer->xferUnsignedInt( &m_nextEconomyCheckFrame );
	xfer->xferUnsignedInt( &m_lastPlayerNotifyFrame );
	xfer->xferObjectID( &m_lastDefenseAttackerID );
}

// ------------------------------------------------------------------------------------------------
/** snapshot: post-load fixup */
// ------------------------------------------------------------------------------------------------
void AITeammatePlayer::loadPostProcess()
{
	AIPlayer::loadPostProcess();

	// Human production must stay enabled after a load.
	if (m_player)
		m_player->setCanBuildUnits( true );
}
