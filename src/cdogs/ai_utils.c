/*
	C-Dogs SDL
	A port of the legendary (and fun) action/arcade cdogs.
	Copyright (C) 1995 Ronny Wester
	Copyright (C) 2003 Jeremy Chin
	Copyright (C) 2003-2007 Lucas Martin-King

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

	This file incorporates work covered by the following copyright and
	permission notice:

	Copyright (c) 2013-2015, 2017, 2021 Cong Xu
	All rights reserved.

	Redistribution and use in source and binary forms, with or without
	modification, are permitted provided that the following conditions are met:

	Redistributions of source code must retain the above copyright notice, this
	list of conditions and the following disclaimer.
	Redistributions in binary form must reproduce the above copyright notice,
	this list of conditions and the following disclaimer in the documentation
	and/or other materials provided with the distribution.

	THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
	AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
	IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
	ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
	LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
	CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
	SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
	INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
	CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
	ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
	POSSIBILITY OF SUCH DAMAGE.
*/
#include "ai_utils.h"

#include <assert.h>

#include "algorithms.h"
#include "collision/collision.h"
#include "gamedata.h"
#include "map.h"
#include "objs.h"
#include "path_cache.h"
#include "weapon.h"

TActor *AIGetClosestPlayer(const struct vec2 pos)
{
	float minDistance2 = -1;
	TActor *closestPlayer = NULL;
	CA_FOREACH(const PlayerData, pd, gPlayerDatas)
	if (!IsPlayerAlive(pd))
	{
		continue;
	}
	TActor *p = ActorGetByUID(pd->ActorUID);
	const float distance2 = svec2_distance_squared(pos, p->Pos);
	if (!closestPlayer || distance2 < minDistance2)
	{
		closestPlayer = p;
		minDistance2 = distance2;
	}
	CA_FOREACH_END()
	return closestPlayer;
}

static TActor *AIGetClosestActor(
	const struct vec2 fromPos, const TActor *from,
	bool (*compFunc)(const TActor *, const TActor *))
{
	// Search all the actors and find the closest one that
	// satisfies the condition
	TActor *closest = NULL;
	float minDistance2 = -1;
	CA_FOREACH(TActor, a, gActors)
	if (!a->isInUse || a->dead)
	{
		continue;
	}
	// Never target invulnerables or civilians
	if (a->flags & (FLAGS_INVULNERABLE | FLAGS_PENALTY))
	{
		continue;
	}
	if (compFunc(a, from))
	{
		const float distance2 = svec2_distance_squared(fromPos, a->Pos);
		if (!closest || distance2 < minDistance2)
		{
			minDistance2 = distance2;
			closest = a;
		}
	}
	CA_FOREACH_END()
	return closest;
}

static bool IsGood(const TActor *a, const TActor *b)
{
	UNUSED(b);
	return a->PlayerUID >= 0 || (a->flags & FLAGS_GOOD_GUY);
}
static bool IsBad(const TActor *a, const TActor *b)
{
	return !IsGood(a, b);
}
static bool IsDifferent(const TActor *a, const TActor *b)
{
	return a != b;
}
const TActor *AIGetClosestEnemy(
	const struct vec2 from, const TActor *a, const int flags)
{
	if (IsPVP(gCampaign.Entry.Mode))
	{
		// free for all; look for anybody else
		return AIGetClosestActor(from, a, IsDifferent);
	}
	else if ((!a || a->PlayerUID < 0) && !(flags & FLAGS_GOOD_GUY))
	{
		// we are bad; look for good guys
		return AIGetClosestActor(from, a, IsGood);
	}
	else
	{
		// we are good; look for bad guys
		return AIGetClosestActor(from, a, IsBad);
	}
}

static bool IsGoodAndVisible(const TActor *a, const TActor *b)
{
	return IsGood(a, b) && (a->flags & FLAGS_VISIBLE);
}
static bool IsBadAndVisible(const TActor *a, const TActor *b)
{
	return IsBad(a, b) && (a->flags & FLAGS_VISIBLE);
}
const TActor *AIGetClosestVisibleEnemy(const TActor *from, const bool isPlayer)
{
	if (IsPVP(gCampaign.Entry.Mode))
	{
		// free for all; look for anybody
		return AIGetClosestActor(from->Pos, from, IsDifferent);
	}
	else if (!isPlayer && !(from->flags & FLAGS_GOOD_GUY))
	{
		// we are bad; look for good guys
		return AIGetClosestActor(from->Pos, from, IsGoodAndVisible);
	}
	else
	{
		// we are good; look for bad guys
		return AIGetClosestActor(from->Pos, from, IsBadAndVisible);
	}
}

struct vec2 AIGetClosestPlayerPos(const struct vec2 pos)
{
	TActor *closestPlayer = AIGetClosestPlayer(pos);
	if (closestPlayer)
	{
		return closestPlayer->Pos;
	}
	else
	{
		return pos;
	}
}

int AIReverseDirection(int cmd)
{
	if (cmd & (CMD_LEFT | CMD_RIGHT))
	{
		cmd ^= CMD_LEFT | CMD_RIGHT;
	}
	if (cmd & (CMD_UP | CMD_DOWN))
	{
		cmd ^= CMD_UP | CMD_DOWN;
	}
	return cmd;
}

typedef bool (*IsBlockedFunc)(void *, const struct vec2i);
static bool AIHasClearLine(
	struct vec2i from, struct vec2i to, IsBlockedFunc isBlockedFunc);
static bool IsTileNoWalk(void *data, const struct vec2i pos);
static bool IsTileNoWalkAroundObjects(void *data, const struct vec2i pos);
bool AIHasClearPath(
	const struct vec2 from, const struct vec2 to, const bool ignoreObjects)
{
	IsBlockedFunc f = ignoreObjects ? IsTileNoWalk : IsTileNoWalkAroundObjects;
	return AIHasClearLine(Vec2ToTile(from), Vec2ToTile(to), f);
}
static bool AIHasClearLine(
	struct vec2i from, struct vec2i to, IsBlockedFunc isBlockedFunc)
{
	// Find all tiles that overlap with the line (from, to)
	HasClearLineData data;
	data.IsBlocked = isBlockedFunc;
	data.data = &gMap;

	return HasClearLineJMRaytrace(from, to, &data);
}
static bool IsTileWalkableOrOpenable(Map *map, struct vec2i pos);
bool IsTileWalkable(Map *map, const struct vec2i pos)
{
	if (!IsTileWalkableOrOpenable(map, pos))
	{
		return false;
	}
	// Check if tile has a dangerous (explosive) item on it
	// For AI, we don't want to shoot it, so just walk around
	Tile *t = MapGetTile(map, pos);
	CA_FOREACH(ThingId, tid, t->things)
	// Only look for explosive objects
	if (tid->Kind != KIND_OBJECT)
	{
		continue;
	}
	const TObject *o = CArrayGet(&gObjs, tid->Id);
	if (ObjIsDangerous(o))
	{
		return false;
	}
	CA_FOREACH_END()
	return true;
}
static bool IsTileNoWalk(void *data, const struct vec2i pos)
{
	return !IsTileWalkable(data, pos);
}
bool IsTileWalkableAroundObjects(Map *map, const struct vec2i pos)
{
	if (!IsTileWalkableOrOpenable(map, pos))
	{
		return false;
	}
	// Check if tile has any item on it
	Tile *t = MapGetTile(map, pos);
	CA_FOREACH(ThingId, tid, t->things)
	if (tid->Kind == KIND_OBJECT)
	{
		// Check that the object has hitbox - i.e. health > 0
		const TObject *o = CArrayGet(&gObjs, tid->Id);
		if (o->Health > 0)
		{
			return false;
		}
	}
	else if (tid->Kind == KIND_CHARACTER)
	{
		switch (gCollisionSystem.allyCollision)
		{
		case ALLYCOLLISION_NORMAL:
			return false;
		case ALLYCOLLISION_REPEL:
			// TODO: implement
			// Need to know collision team of player
			// to know if collision will result in repelling
			break;
		case ALLYCOLLISION_NONE:
			continue;
		default:
			CASSERT(false, "unknown collision type");
			break;
		}
	}
	CA_FOREACH_END()
	return true;
}
static bool IsTileNoWalkAroundObjects(void *data, const struct vec2i pos)
{
	return !IsTileWalkableAroundObjects(data, pos);
}
static bool IsTileWalkableOrOpenable(Map *map, struct vec2i pos)
{
	const Tile *tile = MapGetTile(map, pos);
	if (tile == NULL)
	{
		return false;
	}
	if (TileCanWalk(tile))
	{
		return true;
	}
	if (tile->Class->Type == TILE_CLASS_DOOR)
	{
		// A door; check if we can open it
		int keycard = MapGetDoorKeycardFlag(map, pos);
		if (!keycard)
		{
			// Unlocked door
			return true;
		}
		return !!(keycard & gMission.KeyFlags);
	}
	// Otherwise, we cannot walk over this tile
	return false;
}

static bool IsPosNoSee(void *data, struct vec2i pos)
{
	return TileIsOpaque(MapGetTile(data, Vec2iToTile(pos)));
}
bool AIHasClearView(
	const TActor *a, const struct vec2 to, const int sightRange)
{
	if (svec2_distance_squared(a->Pos, to) > SQUARED(sightRange))
	{
		return false;
	}
	return AIHasClearLine(
		svec2i_assign_vec2(a->Pos), svec2i_assign_vec2(to), IsPosNoSee);
}
bool AICanSee(const TActor *a, const struct vec2 target, const direction_e d)
{
	const int sightRange =
		ConfigGetInt(&gConfig, "Game.SightRange") * TILE_WIDTH;
	if ((a->flags & FLAGS_ALL_SEEING) || AIIsFacing(a, target, d))
	{
		return AIHasClearView(a, target, sightRange * 2 / 3);
	}
	else if (
		AIIsFacing(a, target, DirectionRotate(d, 1)) ||
		AIIsFacing(a, target, DirectionRotate(d, -1)))
	{
		const int peripheralRange = sightRange / 3;
		return AIHasClearView(a, target, peripheralRange);
	}
	return false;
}

static bool IsPosShootable(void *data, const struct vec2i pos)
{
	return TileIsShootable(MapGetTile(data, Vec2iToTile(pos)));
}
bool AIHasClearShot(const struct vec2 from, const struct vec2 to)
{
	// Perform 4 line tests - above, below, left and right
	// This is to account for possible positions for the muzzle
	struct vec2 fromOffset = from;

	const int pad = 2;
	fromOffset.x = from.x - (ACTOR_W + pad) / 2;
	if (Vec2ToTile(fromOffset).x >= 0 &&
		!AIHasClearLine(
			svec2i_assign_vec2(fromOffset), svec2i_assign_vec2(to),
			IsPosShootable))
	{
		return false;
	}
	fromOffset.x = from.x + (ACTOR_W + pad) / 2;
	if (Vec2ToTile(fromOffset).x < gMap.Size.x &&
		!AIHasClearLine(
			svec2i_assign_vec2(fromOffset), svec2i_assign_vec2(to),
			IsPosShootable))
	{
		return false;
	}
	fromOffset.x = from.x;
	fromOffset.y = from.y - (ACTOR_H + pad) / 2;
	if (Vec2ToTile(fromOffset).y >= 0 &&
		!AIHasClearLine(
			svec2i_assign_vec2(fromOffset), svec2i_assign_vec2(to),
			IsPosShootable))
	{
		return false;
	}
	fromOffset.y = from.y + (ACTOR_H + pad) / 2;
	if (Vec2ToTile(fromOffset).y < gMap.Size.y &&
		!AIHasClearLine(
			svec2i_assign_vec2(fromOffset), svec2i_assign_vec2(to),
			IsPosShootable))
	{
		return false;
	}
	return true;
}

TObject *AIGetObjectRunningInto(TActor *a, int cmd)
{
	// Check the position just in front of the character;
	// check if there's a (non-dangerous) object in front of it
	struct vec2 frontPos = a->Pos;
	Thing *item;
	if (cmd & CMD_LEFT)
	{
		frontPos.x--;
	}
	else if (cmd & CMD_RIGHT)
	{
		frontPos.x++;
	}
	if (cmd & CMD_UP)
	{
		frontPos.y--;
	}
	else if (cmd & CMD_DOWN)
	{
		frontPos.y++;
	}
	const CollisionParams params = {
		THING_IMPASSABLE, CalcCollisionTeam(true, a),
		IsPVP(gCampaign.Entry.Mode), false};
	item = OverlapGetFirstItem(
		&a->thing, frontPos, a->thing.size, a->thing.Vel, params);
	if (!item || item->kind != KIND_OBJECT)
	{
		return NULL;
	}
	return CArrayGet(&gObjs, item->id);
}

bool AIIsFacing(const TActor *a, const struct vec2 target, const direction_e d)
{
	const bool isUpperOrLowerOctants =
		fabsf(a->Pos.x - target.x) < fabsf(a->Pos.y - target.y);
	const bool isRight = a->Pos.x < target.x;
	const bool isAbove = a->Pos.y > target.y;
	switch (d)
	{
	case DIRECTION_UP:
		return isAbove && isUpperOrLowerOctants;
	case DIRECTION_UPLEFT:
		return !isRight && isAbove;
	case DIRECTION_LEFT:
		return !isRight && !isUpperOrLowerOctants;
	case DIRECTION_DOWNLEFT:
		return !isRight && !isAbove;
	case DIRECTION_DOWN:
		return !isAbove && isUpperOrLowerOctants;
	case DIRECTION_DOWNRIGHT:
		return isRight && !isAbove;
	case DIRECTION_RIGHT:
		return isRight && !isUpperOrLowerOctants;
	case DIRECTION_UPRIGHT:
		return isRight && isAbove;
	default:
		CASSERT(false, "invalid direction");
		break;
	}
	return false;
}

typedef struct
{
	int ActorUID;
	CollisionTeam CT;
	bool HasFriendly;
} FindFriendliesInTileData;
static bool FindFriendliesInTile(void *data, const struct vec2i tile);
// Whether there are friendlies in the direct line of the gun's range
static bool AIHasFriendliesInLine(const TActor *a, const direction_e dir)
{
	const struct vec2i tileStart = Vec2ToTile(a->Pos);
	const struct vec2 d = Vec2FromRadians(dir2radians[dir]);
	const WeaponClass *wc = ACTOR_GET_WEAPON(a)->Gun;
	const float gunRange = WeaponClassGetRange(wc);
	const struct vec2 dv = svec2_scale(d, gunRange);
	const struct vec2 posEnd = svec2_add(a->Pos, dv);
	const struct vec2i tileEnd = Vec2ToTile(posEnd);

	HasClearLineData data;
	data.IsBlocked = FindFriendliesInTile;
	FindFriendliesInTileData tData;
	tData.ActorUID = a->uid;
	tData.CT = CalcCollisionTeam(true, a);
	tData.CT = CalcCollisionTeam(true, a);
	tData.HasFriendly = false;
	data.data = &tData;
	HasClearLineJMRaytrace(tileStart, tileEnd, &data);
	return tData.HasFriendly;
}
static bool FindFriendliesInTile(void *data, const struct vec2i tile)
{
	const Tile *t = MapGetTile(&gMap, tile);
	if (t == NULL)
		return true;
	FindFriendliesInTileData *tData = data;
	CA_FOREACH(const ThingId, tid, t->things)
	if (tid->Kind != KIND_CHARACTER)
		continue;
	const TActor *other = CArrayGet(&gActors, tid->Id);
	// Don't worry about self
	if (tData->ActorUID == other->uid)
		continue;
	// Never shoot prisoners/victims... it's not nice ;)
	if (other->flags & (FLAGS_PRISONER | FLAGS_VICTIM))
	{
		tData->HasFriendly = true;
		return true;
	}
	const CollisionTeam ctOther = CalcCollisionTeam(true, other);
	if (tData->CT != COLLISIONTEAM_NONE && ctOther != COLLISIONTEAM_NONE &&
		tData->CT == ctOther)
	{
		tData->HasFriendly = true;
		return true;
	}
	// If it's an enemy, do shoot!
	return true;
	CA_FOREACH_END()
	return false;
}

// Use pathfinding to check that there is a path between
// source and destination tiles
bool AIHasPath(
	const struct vec2 from, const struct vec2 to, const bool ignoreObjects)
{
	// Quick first test: check there is a clear path
	if (AIHasClearPath(from, to, ignoreObjects))
	{
		return true;
	}
	// Pathfind
	const struct vec2i fromTile = Vec2ToTile(from);
	const struct vec2i toTile = MapSearchTileAround(
		&gMap, Vec2ToTile(to),
		ignoreObjects ? IsTileWalkable : IsTileWalkableAroundObjects);
	CachedPath path =
		PathCacheCreate(&gPathCache, fromTile, toTile, ignoreObjects, true);
	const size_t pathCount = ASPathGetCount(path.Path);
	CachedPathDestroy(&path);
	return pathCount >= 1;
}

int AIGotoDirect(const struct vec2 a, const struct vec2 p)
{
	int cmd = 0;

	if (a.x + 1 < p.x)
		cmd |= CMD_RIGHT;
	else if (a.x > p.x + 1)
		cmd |= CMD_LEFT;

	if (a.y + 1 < p.y)
		cmd |= CMD_DOWN;
	else if (a.y > p.y + 1)
		cmd |= CMD_UP;

	return cmd;
}

// Follow the current A* path
static int AStarFollow(
	AIGotoContext *c, const struct vec2i currentTile, const Thing *i,
	const struct vec2 a)
{
	struct vec2i *pathTile = ASPathGetNode(c->Path.Path, c->PathIndex);
	c->IsFollowing = 1;
	// Check if we need to follow the next step in the path
	// Note: need to make sure the actor is fully within the current tile
	// otherwise it may get stuck at corners
	if (svec2i_is_equal(currentTile, *pathTile) &&
		IsThingInsideTile(i, currentTile))
	{
		c->PathIndex++;
		pathTile = ASPathGetNode(c->Path.Path, c->PathIndex);
	}
	// Go directly to the center of the next tile
	return AIGotoDirect(a, Vec2CenterOfTile(*pathTile));
}
// Check that we are still close to the start of the A* path,
// and the end of the path is close to our goal
static int AStarCloseToPath(
	AIGotoContext *c, struct vec2i currentTile, struct vec2i goalTile)
{
	struct vec2i *pathTile;
	struct vec2i *pathEnd;
	if (!c || c->PathIndex >=
				  (int)ASPathGetCount(c->Path.Path) - 1) // at end of path
	{
		return 0;
	}
	// Check if we're too far from the current start of the path
	pathTile = ASPathGetNode(c->Path.Path, c->PathIndex);
	if ((int)svec2i_distance_squared(
			currentTile, svec2i(pathTile->x, pathTile->y)) > 4)
	{
		return 0;
	}
	// Check if we're too far from the end of the path
	pathEnd = ASPathGetNode(c->Path.Path, ASPathGetCount(c->Path.Path) - 1);
	if ((int)svec2i_distance_squared(
			goalTile, svec2i(pathEnd->x, pathEnd->y)) > 0)
	{
		return 0;
	}
	return 1;
}
int AIGoto(const TActor *actor, const struct vec2 p, const bool ignoreObjects)
{
	const struct vec2i currentTile = Vec2ToTile(actor->Pos);
	const struct vec2i goalTile = Vec2ToTile(p);
	AIGotoContext *c = &actor->aiContext->Goto;

	CASSERT(c != NULL, "no AI context");

	// If we are already there, go directly to the goal
	// This can happen if AI is trying to track the player,
	// but the player has died, for example.
	if (svec2i_is_equal(currentTile, goalTile))
	{
		return AIGotoDirect(actor->Pos, p);
	}

	// If we are currently following an A* path,
	// and it is still valid, keep following it until
	// we have reached a new tile
	if (c && c->IsFollowing && AStarCloseToPath(c, currentTile, goalTile))
	{
		return AStarFollow(c, currentTile, &actor->thing, actor->Pos);
	}
	else if (AIHasClearPath(actor->Pos, p, ignoreObjects))
	{
		// Simple case: if there's a clear line between AI and target,
		// walk straight towards it
		return AIGotoDirect(actor->Pos, p);
	}
	else
	{
		// We need to recalculate A*

		// First, if the goal tile is blocked itself,
		// find a nearby tile that can be walked to
		c->Goal = MapSearchTileAround(
			&gMap, goalTile,
			ignoreObjects ? IsTileWalkable : IsTileWalkableAroundObjects);

		c->PathIndex = 1; // start navigating to the next path node
		CachedPathDestroy(&c->Path);
		c->Path = PathCacheCreate(
			&gPathCache, currentTile, c->Goal, ignoreObjects, true);

		// In case we can't calculate A* for some reason,
		// try simple navigation again
		if (ASPathGetCount(c->Path.Path) <= 1)
		{
			return AIGotoDirect(actor->Pos, p);
		}

		return AStarFollow(c, currentTile, &actor->thing, actor->Pos);
	}
}

// Hunt moves an Actor towards a target, using the most efficient direction.
// That is, given the following octant:
//            x  A      xxxx
//          x       xxxx
//        x     xxxx
//      x   xxxx      B
//    x  xxx
//  xxxxxxxxxxxxxxxxxxxxxxx
// Those in slice A will move down-left and those in slice B will move left.
int AIHunt(const TActor *actor, const struct vec2 targetPos)
{
	const struct vec2 pos =
		svec2_add(actor->Pos, ActorGetAverageWeaponMuzzleOffset(actor));
	const float dx = fabsf(targetPos.x - pos.x);
	const float dy = fabsf(targetPos.y - pos.y);

	int cmd = 0;
	if (2 * dx > dy)
	{
		if (pos.x < targetPos.x)
			cmd |= CMD_RIGHT;
		else if (pos.x > targetPos.x)
			cmd |= CMD_LEFT;
	}
	if (2 * dy > dx)
	{
		if (pos.y < targetPos.y)
			cmd |= CMD_DOWN;
		else if (pos.y > targetPos.y)
			cmd |= CMD_UP;
	}
	// If it's a coward, reverse directions...
	if (actor->flags & FLAGS_RUNS_AWAY)
	{
		cmd = AIReverseDirection(cmd);
	}

	return cmd;
}
int AIHuntClosest(TActor *actor)
{
	struct vec2 targetPos = actor->Pos;
	if (!(actor->PlayerUID >= 0 || (actor->flags & FLAGS_GOOD_GUY)))
	{
		targetPos = AIGetClosestPlayerPos(actor->Pos);
	}

	if (actor->flags & FLAGS_VISIBLE)
	{
		const TActor *a =
			AIGetClosestVisibleEnemy(actor, actor->PlayerUID >= 0);
		if (a)
		{
			targetPos = a->Pos;
		}
	}
	return AIHunt(actor, targetPos);
}

// Smarter attack routine:
// - Move/position towards target, keeping ideal distance from it
// - Fire if
//   - has clear view to target, and
//   - no friendlies in the way
int AIAttack(const TActor *a, const struct vec2 targetPos)
{
	// Move to the ideal distance for the weapon
	int cmd = 0;
	const Weapon *w = ACTOR_GET_WEAPON(a);
	const WeaponClass *wc = w->Gun;
	const float gunRange = WeaponClassGetRange(wc);
	const float distanceSquared = svec2_distance_squared(a->Pos, targetPos);
	const bool canFire =
		WeaponClassCanShoot(wc) && WeaponGetUnlockedBarrel(w) >= 0;
	if ((double)distanceSquared <
			SQUARED(gunRange * 3) * a->aiContext->GunRangeScalar &&
		!canFire)
	{
		// Move away from the enemy because we're too close
		// Only move away if we can't fire; otherwise turn to fire
		cmd = AIRetreatFrom(a, targetPos);
	}
	else
	{
		// Move towards the enemy, fire if able
		// But don't bother firing if too far away

		if ((double)distanceSquared > SQUARED(gunRange * 2))
		{
			// Too far away; approach using most efficient method
			cmd = AIHunt(a, targetPos);
		}
		else
		{
#define MINIMUM_GUN_DISTANCE 30
			const bool willFire =
				canFire && (distanceSquared < SQUARED(gunRange * 2) ||
							distanceSquared > SQUARED(MINIMUM_GUN_DISTANCE));
			if (willFire)
			{
				// Hunt; this is the best direction to attack in
				cmd = AIHunt(a, targetPos);
			}
			else
			{
				// Track so that we end up in a favorable angle
				cmd = AITrack(a, targetPos);
			}
			// Don't fire if there's a friendly in the way
			const direction_e d = cmd2dir[cmd & CMD_DIRECTIONS];
			if (willFire && !AIHasFriendliesInLine(a, d))
			{
				cmd |= CMD_BUTTON1;
			}
		}
	}
	return cmd;
}

// Move away from the target
// Usually used for a simple flee
int AIRetreatFrom(const TActor *actor, const struct vec2 from)
{
	return AIReverseDirection(AIHunt(actor, from));
}

// Track moves an Actor towards a target, but in such a fashion that the Actor
// will come into 8-axis alignment with the target soonest.
// That is, given the following octant:
//            x  A      xxxx
//          x       xxxx
//        x     xxxx
//      x   xxxx      B
//    x  xxx
//  xxxxxxxxxxxxxxxxxxxxxxx
// Those in slice A will move left and those in slice B will move down-left.
int AITrack(const TActor *actor, const struct vec2 targetPos)
{
	const struct vec2 pos =
		svec2_add(actor->Pos, ActorGetAverageWeaponMuzzleOffset(actor));
	const float dx = fabsf(targetPos.x - pos.x);
	const float dy = fabsf(targetPos.y - pos.y);

	int cmd = 0;
	// Terminology: imagine the compass directions sliced into 16 equal parts,
	// and labelled in bearing/clock order. That is, the slices on the right
	// half are labelled 1-8.
	// In order to construct the movement, note that:
	// - If the target is to our left, we need to move left...
	// - Except if they are in slice 2 or 7
	// Slice 2 and 7 (and 10 and 15) can be found by (dy < 2dx && dy > dx)
	// - call this X-exception
	// Repeat this for all 4 cardinal directions.
	// Furthermore, give a bit of leeway for the 8 axes so we don't
	// fluctuate between perpendicular movement vectors.
	const bool xException = dy < 2 * dx && dy > dx * 1.1f;
	if (!xException)
	{
		if (pos.x - targetPos.x < dy * 0.1f)
			cmd |= CMD_RIGHT;
		else if (pos.x - targetPos.x > -dy * 0.1f)
			cmd |= CMD_LEFT;
	}
	const bool yException = dx < 2 * dy && dx > dy * 1.1f;
	if (!yException)
	{
		if (pos.y - targetPos.y < dx * 0.1f)
			cmd |= CMD_DOWN;
		else if (pos.y - targetPos.y > -dx * 0.1f)
			cmd |= CMD_UP;
	}

	return cmd;
}

int AIMoveAwayFromLine(
	const struct vec2 pos, const struct vec2 lineStart,
	const direction_e lineD)
{
	const struct vec2 dv = svec2_subtract(pos, lineStart);
	switch (lineD)
	{
	case DIRECTION_UP:
	case DIRECTION_DOWN: // fallthrough
		return dv.x < 0 ? CMD_LEFT : CMD_RIGHT;
	case DIRECTION_UPLEFT:
	case DIRECTION_DOWNRIGHT: // fallthrough
		return dv.x > dv.y ? (CMD_RIGHT | CMD_UP) : (CMD_LEFT | CMD_DOWN);
	case DIRECTION_LEFT:
	case DIRECTION_RIGHT: // fallthrough
		return dv.y < 0 ? CMD_UP : CMD_DOWN;
	case DIRECTION_DOWNLEFT:
	case DIRECTION_UPRIGHT: // fallthrough
		return dv.x < -dv.y ? (CMD_LEFT | CMD_UP) : (CMD_RIGHT | CMD_DOWN);
	default:
		CASSERT(false, "unknown direction");
		return 0;
	}
}
