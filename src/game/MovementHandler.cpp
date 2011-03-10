/*
 * Copyright (C) 2005-2011 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "Common.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Opcodes.h"
#include "Log.h"
#include "Corpse.h"
#include "Player.h"
#include "MapManager.h"
#include "Transports.h"
#include "BattleGround.h"
#include "WaypointMovementGenerator.h"
#include "MapPersistentStateMgr.h"
#include "ObjectMgr.h"
#include "World.h"
#include "Chat.h"

/*Movement anticheat DEBUG defines */
#define MOVEMENT_ANTICHEAT_DEBUG true
/*end Movement anticheate defines*/
void WorldSession::HandleMoveWorldportAckOpcode( WorldPacket & /*recv_data*/ )
{
    DEBUG_LOG( "WORLD: got MSG_MOVE_WORLDPORT_ACK." );
    HandleMoveWorldportAckOpcode();
}

void WorldSession::HandleMoveWorldportAckOpcode()
{
    // ignore unexpected far teleports
    if(!GetPlayer()->IsBeingTeleportedFar())
        return;

    // get start teleport coordinates (will used later in fail case)
    WorldLocation old_loc;
    GetPlayer()->GetPosition(old_loc);

    // get the teleport destination
    WorldLocation &loc = GetPlayer()->GetTeleportDest();

    // possible errors in the coordinate validity check (only cheating case possible)
    if (!MapManager::IsValidMapCoord(loc.mapid, loc.coord_x, loc.coord_y, loc.coord_z, loc.orientation))
    {
        sLog.outError("WorldSession::HandleMoveWorldportAckOpcode: %s was teleported far to a not valid location "
            "(map:%u, x:%f, y:%f, z:%f) We port him to his homebind instead..",
            GetPlayer()->GetGuidStr().c_str(), loc.mapid, loc.coord_x, loc.coord_y, loc.coord_z);
        // stop teleportation else we would try this again and again in LogoutPlayer...
        GetPlayer()->SetSemaphoreTeleportFar(false);
        // and teleport the player to a valid place
        GetPlayer()->TeleportToHomebind();
        return;
    }
    //reset falltimer at teleport
    GetPlayer()->m_anti_justteleported = 1;
    // get the destination map entry, not the current one, this will fix homebind and reset greeting
    MapEntry const* mEntry = sMapStore.LookupEntry(loc.mapid);

    Map* map = NULL;

    // prevent crash at attempt landing to not existed battleground instance
    if(mEntry->IsBattleGroundOrArena())
    {
        if (GetPlayer()->GetBattleGroundId())
            map = sMapMgr.FindMap(loc.mapid, GetPlayer()->GetBattleGroundId());

        if (!map)
        {
            DETAIL_LOG("WorldSession::HandleMoveWorldportAckOpcode: %s was teleported far to nonexisten battleground instance "
                " (map:%u, x:%f, y:%f, z:%f) Trying to port him to his previous place..",
                GetPlayer()->GetGuidStr().c_str(), loc.mapid, loc.coord_x, loc.coord_y, loc.coord_z);

            GetPlayer()->SetSemaphoreTeleportFar(false);

            // Teleport to previous place, if cannot be ported back TP to homebind place
            if (!GetPlayer()->TeleportTo(old_loc))
            {
                DETAIL_LOG("WorldSession::HandleMoveWorldportAckOpcode: %s cannot be ported to his previous place, teleporting him to his homebind place...",
                    GetPlayer()->GetGuidStr().c_str());
                GetPlayer()->TeleportToHomebind();
            }
            return;
        }
    }

    InstanceTemplate const* mInstance = ObjectMgr::GetInstanceTemplate(loc.mapid);

    // reset instance validity, except if going to an instance inside an instance
    if (GetPlayer()->m_InstanceValid == false && !mInstance)
        GetPlayer()->m_InstanceValid = true;

    GetPlayer()->SetSemaphoreTeleportFar(false);

    // relocate the player to the teleport destination

    if (!map)
        map = sMapMgr.CreateMap(loc.mapid, GetPlayer());

	if (!map)	// kia
	{
		sLog.outError("InstanceMgr can't create map %u for %s",loc.mapid, GetPlayer()->GetName());
		GetPlayer()->SetSemaphoreTeleportFar(false);
		GetPlayer()->RelocateToHomebind();
		return;
	}

    GetPlayer()->SetMap(map);
    GetPlayer()->Relocate(loc.coord_x, loc.coord_y, loc.coord_z, loc.orientation);

    GetPlayer()->SendInitialPacketsBeforeAddToMap();
    // the CanEnter checks are done in TeleporTo but conditions may change
    // while the player is in transit, for example the map may get full
    if (!GetPlayer()->GetMap()->Add(GetPlayer()))
    {
        // if player wasn't added to map, reset his map pointer!
        GetPlayer()->ResetMap();

        DETAIL_LOG("WorldSession::HandleMoveWorldportAckOpcode: %s was teleported far but couldn't be added to map "
            " (map:%u, x:%f, y:%f, z:%f) Trying to port him to his previous place..",
            GetPlayer()->GetGuidStr().c_str(), loc.mapid, loc.coord_x, loc.coord_y, loc.coord_z);

        // Teleport to previous place, if cannot be ported back TP to homebind place
        if (!GetPlayer()->TeleportTo(old_loc))
        {
            DETAIL_LOG("WorldSession::HandleMoveWorldportAckOpcode: %s cannot be ported to his previous place, teleporting him to his homebind place...",
                GetPlayer()->GetGuidStr().c_str());
            GetPlayer()->TeleportToHomebind();
        }
        return;
    }

    // battleground state prepare (in case join to BG), at relogin/tele player not invited
    // only add to bg group and object, if the player was invited (else he entered through command)
    if(_player->InBattleGround())
    {
        // cleanup setting if outdated
        if(!mEntry->IsBattleGroundOrArena())
        {
            // We're not in BG
            _player->SetBattleGroundId(0, BATTLEGROUND_TYPE_NONE);
            // reset destination bg team
            _player->SetBGTeam(TEAM_NONE);
        }
        // join to bg case
        else if(BattleGround *bg = _player->GetBattleGround())
        {
            if(_player->IsInvitedForBattleGroundInstance(_player->GetBattleGroundId()))
                bg->AddPlayer(_player);
        }
    }

    GetPlayer()->SendInitialPacketsAfterAddToMap();

    // flight fast teleport case
    if(GetPlayer()->GetMotionMaster()->GetCurrentMovementGeneratorType() == FLIGHT_MOTION_TYPE)
    {
        if(!_player->InBattleGround())
        {
            // short preparations to continue flight
            FlightPathMovementGenerator* flight = (FlightPathMovementGenerator*)(GetPlayer()->GetMotionMaster()->top());
            flight->Reset(*GetPlayer());
            return;
        }

        // battleground state prepare, stop flight
        GetPlayer()->GetMotionMaster()->MovementExpired();
        GetPlayer()->m_taxi.ClearTaxiDestinations();
    }

    // resurrect character at enter into instance where his corpse exist after add to map
    Corpse *corpse = GetPlayer()->GetCorpse();
    if (corpse && corpse->GetType() != CORPSE_BONES && corpse->GetMapId() == GetPlayer()->GetMapId())
    {
        if( mEntry->IsDungeon() )
        {
            GetPlayer()->ResurrectPlayer(0.5f);
            GetPlayer()->SpawnCorpseBones();
        }
    }

    if(mEntry->IsRaid() && mInstance)
    {
        if (time_t timeReset = sMapPersistentStateMgr.GetScheduler().GetResetTimeFor(mEntry->MapID))
        {
            uint32 timeleft = uint32(timeReset - time(NULL));
            GetPlayer()->SendInstanceResetWarning(mEntry->MapID, timeleft);
        }
    }

    // mount allow check
    if(!mEntry->IsMountAllowed())
        _player->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);

    // honorless target
    if(GetPlayer()->pvpInfo.inHostileArea)
        GetPlayer()->CastSpell(GetPlayer(), 2479, true);

    // resummon pet
    GetPlayer()->ResummonPetTemporaryUnSummonedIfAny();

    //lets process all delayed operations on successful teleport
    GetPlayer()->ProcessDelayedOperations();
}

void WorldSession::HandleMoveTeleportAckOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("MSG_MOVE_TELEPORT_ACK");

    ObjectGuid guid;

    recv_data >> guid;

    uint32 counter, time;
    recv_data >> counter >> time;
    DEBUG_LOG("Guid: %s", guid.GetString().c_str());
    DEBUG_LOG("Counter %u, time %u", counter, time/IN_MILLISECONDS);

    Unit *mover = _player->GetMover();
    Player *plMover = mover->GetTypeId() == TYPEID_PLAYER ? (Player*)mover : NULL;

    if(!plMover || !plMover->IsBeingTeleportedNear())
        return;

    if(guid != plMover->GetObjectGuid())
        return;

    plMover->SetSemaphoreTeleportNear(false);

    uint32 old_zone = plMover->GetZoneId();

    WorldLocation const& dest = plMover->GetTeleportDest();

    plMover->SetPosition(dest.coord_x, dest.coord_y, dest.coord_z, dest.orientation, true);

    uint32 newzone, newarea;
    plMover->GetZoneAndAreaId(newzone, newarea);
    plMover->UpdateZone(newzone, newarea);

    // new zone
    if(old_zone != newzone)
    {
        // honorless target
        if(plMover->pvpInfo.inHostileArea)
            plMover->CastSpell(plMover, 2479, true);
    }

    // resummon pet
    GetPlayer()->ResummonPetTemporaryUnSummonedIfAny();

    //lets process all delayed operations on successful teleport
    GetPlayer()->ProcessDelayedOperations();
}

void WorldSession::HandleMovementOpcodes( WorldPacket & recv_data )
{
    uint32 opcode = recv_data.GetOpcode();
    DEBUG_LOG("WORLD: Recvd %s (%u, 0x%X) opcode", LookupOpcodeName(opcode), opcode, opcode);

	if (!_player)
		return;

    Unit *mover = _player->GetMover();
    Player *plMover = mover->GetTypeId() == TYPEID_PLAYER ? (Player*)mover : NULL;

    // ignore, waiting processing in WorldSession::HandleMoveWorldportAckOpcode and WorldSession::HandleMoveTeleportAck
    if(plMover && plMover->IsBeingTeleported())
    {
        plMover->m_anti_justteleported = 1;
        recv_data.rpos(recv_data.wpos());                   // prevent warnings spam
        return;
    }

    /* extract packet */
    MovementInfo movementInfo;
    recv_data >> movementInfo;
	GetPlayer()->GetMover()->ShowMovementInfo(GetPlayer()->GetGUIDLow(), movementInfo, LookupOpcodeName(recv_data.GetOpcode())); 
    /*----------------*/

    if (!VerifyMovementInfo(movementInfo))
        return;
    
    /* handle special cases */
    if (movementInfo.HasMovementFlag(MOVEFLAG_ONTRANSPORT))
    {
        // transports size limited
        // (also received at zeppelin/lift leave by some reason with t_* as absolute in continent coordinates, can be safely skipped)
		if (movementInfo.GetTransportGuid().GetRawValue() && (movementInfo.GetTransportPos()->x > 60 || movementInfo.GetTransportPos()->y > 60 || movementInfo.GetTransportPos()->z > 100))
		{
			sLog.outCheat("Movement anticheat: %s move on transport %d to x=%f y=%f z=%f m=%d",
				GetPlayer()->GetName(), movementInfo.GetTransportGuid().GetEntry(), movementInfo.GetPos()->x, movementInfo.GetPos()->y, movementInfo.GetPos()->z,GetPlayer()->GetMapId());
            recv_data.rpos(recv_data.wpos());               // prevent warnings spam
            return;
		}

        if(movementInfo.GetTransportGuid().GetRawValue() && !MaNGOS::IsValidMapCoord(movementInfo.GetPos()->x+movementInfo.GetTransportPos()->x, movementInfo.GetPos()->y+movementInfo.GetTransportPos()->y,
            movementInfo.GetPos()->z+movementInfo.GetTransportPos()->z, movementInfo.GetPos()->o+movementInfo.GetTransportPos()->o) )
		{
			sLog.outCheat("Movement anticheat: %s move on transport %d to invalid coord x=%f y=%f z=%f m=%d",
				GetPlayer()->GetName(), movementInfo.GetTransportGuid().GetEntry(), movementInfo.GetPos()->x, movementInfo.GetPos()->y, movementInfo.GetPos()->z,GetPlayer()->GetMapId());
            recv_data.rpos(recv_data.wpos());               // prevent warnings spam
            return;
		}

		//if passenger on transport and transport changed - remove it
		if (plMover && plMover->m_transport && plMover->m_transport->GetGUID()!=movementInfo.t_guid.GetRawValue())
		{
            plMover->m_transport->RemovePassenger(GetPlayer());
            plMover->m_transport = NULL;
	        plMover->m_anti_transportGUID = 0;
		}

		if (plMover && (plMover->m_anti_transportGUID == 0) && (movementInfo.GetTransportGuid().GetRawValue() !=0))
        {
            // if we boarded a transport, add us to it
            if (!plMover->m_transport)
            {
                // elevators also cause the client to send MOVEFLAG_ONTRANSPORT - just unmount if the guid can be found in the transport list
                for (MapManager::TransportSet::const_iterator iter = sMapMgr.m_Transports.begin(); iter != sMapMgr.m_Transports.end(); ++iter)
                {
                    if ((*iter)->GetObjectGuid() == movementInfo.GetTransportGuid())
                    {
                        // unmount before boarding
                        plMover->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
                        plMover->m_transport = (*iter);
                        (*iter)->AddPassenger(plMover);
                        break;
                    }
                }
            }
            ///GetPlayer()->m_anti_transportGUID = GUID_LOPART(movementInfo.t_guid);
            //Correct finding GO guid in DB (thanks to GriffonHeart)
			GameObject *obj = NULL;//ObjectAccessor::GetGameObjectInWorld(movementInfo.t_guid.GetRawValue());
            if(obj)
                plMover->m_anti_transportGUID = obj->GetGUIDLow();
            else
                plMover->m_anti_transportGUID = GUID_LOPART(movementInfo.t_guid.GetRawValue());
        }
    }
    else if (plMover && (plMover->m_anti_transportGUID != 0))
    {
        if (plMover->m_transport)                      // if we were on a transport, leave
        {
            plMover->m_transport->RemovePassenger(plMover);
            plMover->m_transport = NULL;
        }
        movementInfo.ClearTransportData();
        plMover->m_anti_transportGUID = 0;
    }

    // fall damage generation (ignore in flight case that can be triggered also at lags in moment teleportation to another map).
    if (opcode == MSG_MOVE_FALL_LAND && plMover && !plMover->IsTaxiFlying())
    {
        plMover->m_anti_justjumped = 0;
        plMover->m_anti_jumpbase = 0;
        plMover->HandleFall(movementInfo);
    }

    /*----------------------*/
    //---- anti-cheat features -->>>
    bool check_passed = true;
	if (plMover)
	{
    //calc time deltas
    int32 timedelta = 1500;
    if (plMover->m_anti_lastmovetime !=0){
        timedelta = movementInfo.time - plMover->m_anti_lastmovetime;
        plMover->m_anti_deltamovetime += timedelta;
        plMover->m_anti_lastmovetime = movementInfo.time;
    } else {
        plMover->m_anti_lastmovetime = movementInfo.time;
    }

    uint32 CurrentMStime = WorldTimer::getMSTime();
    uint32 CurrentMStimeDelta = 1500;
    if (plMover->m_anti_lastMStime != 0){
        CurrentMStimeDelta = CurrentMStime - plMover->m_anti_lastMStime;
        plMover->m_anti_deltaMStime += CurrentMStimeDelta;
        plMover->m_anti_lastMStime = CurrentMStime;
    } else {
        plMover->m_anti_lastMStime = CurrentMStime;
    }

    //resync times on client login (first 15 sec for heavy areas)
    if (plMover->m_anti_deltaMStime < 15000 && plMover->m_anti_deltamovetime < 15000)
        plMover->m_anti_deltamovetime = plMover->m_anti_deltaMStime;

    int32 sync_time = plMover->m_anti_deltamovetime - plMover->m_anti_deltaMStime;

	if (sWorld.TargetGuid==plMover->GetGUIDLow())
		sLog.outBasic("dtime: %d, stime: %d || dMS: %d - dMV: %d || dt: %d", timedelta, CurrentMStime, plMover->m_anti_deltaMStime,  plMover->m_anti_deltamovetime, sync_time);

    uint32 curDest = plMover->m_taxi.GetTaxiDestination(); //check taxi flight
    if ((plMover->m_anti_transportGUID == 0) && World::GetEnableMvAnticheat() && !curDest)
    {
        UnitMoveType move_type;

		if (plMover->HasMovementFlag(MOVEFLAG_FLYING2)) move_type = plMover->HasMovementFlag(MOVEFLAG_BACKWARD) ? MOVE_FLIGHT_BACK : MOVE_FLIGHT;
        else if (plMover->HasMovementFlag(MOVEFLAG_SWIMMING)) move_type = plMover->HasMovementFlag(MOVEFLAG_BACKWARD) ? MOVE_SWIM_BACK : MOVE_SWIM;
        else if (plMover->HasMovementFlag(MOVEFLAG_WALK_MODE)) move_type = MOVE_WALK;
        //hmm... in first time after login player has MOVE_SWIMBACK instead MOVE_WALKBACK
        else move_type = plMover->HasMovementFlag(MOVEFLAG_BACKWARD) ? MOVE_SWIM_BACK : MOVE_RUN;

		float allowed_delta = 0;
		if (!plMover->m_anti_last_hspeed[move_type])
			plMover->m_anti_last_hspeed[move_type] = plMover->GetSpeed(move_type);
		float current_speed = plMover->m_anti_last_hspeed[move_type];
        float delta_x = plMover->GetPositionX() - movementInfo.GetPos()->x;
        float delta_y = plMover->GetPositionY() - movementInfo.GetPos()->y;
        float delta_z = plMover->GetPositionZ() - movementInfo.GetPos()->z;
        float real_delta = delta_x * delta_x + delta_y * delta_y;
		if (plMover->skip_check)
		{
			plMover->skip_check=false;
			real_delta=0;
		}
        float tg_z = -99999; //tangens

        /*int32 gmd = World::GetMistimingDelta();
        if (sync_time > gmd || sync_time < -gmd){
            timedelta = CurrentMStimeDelta;
            plMover->m_anti_mistiming_count++;

            sLog.outError("Movement anticheat: %s is mistaming exception. Exception count: %d, mistiming: %d ms ", plMover->GetName(), plMover->m_anti_mistiming_count, sync_time);
            if (plMover->m_anti_mistiming_count > World::GetMistimingAlarms())
            {
                plMover->GetSession()->KickPlayer();
				sLog.outError("Movement anticheat: %s is mistaming exception. Kicked.", plMover->GetName());
                ///sLog.outError("Movement anticheat: %s is mistaming exception. Exception count: %d, mistiming: %d ms ", GetPlayer()->GetName(), GetPlayer()->m_anti_mistiming_count, sync_time);
            }
            //check_passed = false;
        }*/

        float time_delta = (timedelta < 1500) ? (float)timedelta/1000 : 1.5f; //normalize time - 1.5 second allowed for heavy loaded server

        if (!(movementInfo.HasMovementFlag(MOVEFLAG_FLYING) || movementInfo.HasMovementFlag(MOVEFLAG_SWIMMING)))
          tg_z = (real_delta !=0) ? (delta_z*delta_z / real_delta) : -99999;

        //if (current_speed < plMover->m_anti_last_hspeed)
        //{
        //    allowed_delta = plMover->m_anti_last_hspeed;
        //    if (plMover->m_anti_lastspeed_changetime == 0 )
        //        plMover->m_anti_lastspeed_changetime = movementInfo.time + (uint32)floor(((plMover->m_anti_last_hspeed / current_speed) * 1000)) + 100; //100ms above for random fluctuating =)))
        //}	else 
		float dx, dy;
        dx = plMover->m_movementInfo.GetJumpInfo().xyspeed*plMover->m_movementInfo.GetJumpInfo().cosAngle*time_delta;
        dx = dx*dx;
        dy = plMover->m_movementInfo.GetJumpInfo().xyspeed*plMover->m_movementInfo.GetJumpInfo().sinAngle*time_delta;
        dy = dy*dy;
		allowed_delta = dx+dy;
		if (!allowed_delta)
			allowed_delta = current_speed*current_speed*time_delta*time_delta;
        allowed_delta = allowed_delta + 2;

		if (sWorld.TargetGuid==plMover->GetGUIDLow())
		{
			sLog.outBasic("dtime: %d, allow: %f speed: %f lastHspd: %f lastHPspd: %f", timedelta, allowed_delta, current_speed, plMover->GetSpeed(move_type), plMover->m_movementInfo.GetJumpInfo().xyspeed);
            static char const* move_type_name[MAX_MOVE_TYPE] = {  "Walk", "Run", "Walkback", "Swim", "Swimback", "Turn", "Fly", "Flyback" };
            sLog.outBasic("%s newcoord: tm:%d ftm:%d | %f,%f,%fo(%f) [%s]$%s",GetPlayer()->GetName(),movementInfo.time,movementInfo.fallTime,movementInfo.GetPos()->x,movementInfo.GetPos()->y,movementInfo.GetPos()->z,movementInfo.GetPos()->o,LookupOpcodeName(opcode),move_type_name[move_type]);
			sLog.outBasic("%f",tg_z);
		}

		if (opcode == MSG_MOVE_STOP)
		{
			plMover->m_anti_jumpbase = 0;
			plMover->m_anti_justjumped = 0;
			if (plMover->blink_test && (abs(delta_x)<0.001f) && (abs(delta_y)<0.001f) && (abs(delta_z)<0.001f))
			{
    			sLog.outCheat("Movement anticheat: %s is Blink exception (%f %f %f %d)->(%f %f %f). dz=%f [%X][%s]",
					plMover->GetName(), plMover->GetPositionX(), plMover->GetPositionY(), plMover->GetPositionZ(), plMover->GetMapId(),
					movementInfo.GetPos()->x, movementInfo.GetPos()->y, movementInfo.GetPos()->z, real_delta, movementInfo.GetMovementFlags(), LookupOpcodeName(opcode));
				check_passed = false;
			}
		}

		if (opcode == MSG_MOVE_START_FORWARD && (movementInfo.GetJumpInfo().sinAngle==0.f) &&
			(movementInfo.GetJumpInfo().cosAngle==0.f) && (movementInfo.GetJumpInfo().velocity==0.f) 
			&& (movementInfo.GetJumpInfo().xyspeed==0.f) && (real_delta>0.001f || abs(delta_z)>0.001f))
			plMover->blink_test = true;
		else 
			plMover->blink_test = false;

        //AntiGravitation (thanks to Meekro)
        float JumpHeight = plMover->m_anti_jumpbase - movementInfo.GetPos()->z;
        if ((plMover->m_anti_jumpbase != 0) && !(plMover->m_movementInfo.HasMovementFlag(MOVEFLAG_SWIMMING) || plMover->m_movementInfo.HasMovementFlag(MOVEFLAG_FLYING) || plMover->m_movementInfo.HasMovementFlag(MOVEFLAG_FLYING2))
                    && (JumpHeight < plMover->m_anti_last_vspeed)){
						sLog.outCheat("Movement anticheat: %s is graviJump exception at (%f %f %f %d). dz=%f [%X][%s]",plMover->GetName(), movementInfo.GetPos()->x, movementInfo.GetPos()->y, movementInfo.GetPos()->z, plMover->GetMapId(), movementInfo.GetPos()->z - plMover->m_anti_jumpbase,movementInfo.GetMovementFlags(),LookupOpcodeName(opcode));
            check_passed = false;
        }

        if (opcode == MSG_MOVE_JUMP && !plMover->IsInWater()){
            if (plMover->m_anti_justjumped >= 1){
                ///GetPlayer()->m_anti_justjumped = 0;
                check_passed = false; //don't process new jump packet
            } else {
                plMover->m_anti_justjumped += 1;
				plMover->m_anti_jumpbase = movementInfo.GetPos()->z;
            }
        } else if (plMover->IsInWater()) {
             plMover->m_anti_justjumped = 0;
        }

		if (movementInfo.HasMovementFlag(MOVEFLAG_UNK4))//(opcode==MSG_MOVE_HEARTBEAT)&&(movementInfo.j_xyspeed==0.0f)&&((movementInfo.j_cosAngle!=0.0f)||(movementInfo.j_sinAngle!=0.0f)))
		{
			sLog.outDebug("Movement: falling");
		}else
        if ((real_delta>allowed_delta) && (opcode!=MSG_MOVE_FALL_LAND))
        {
            sLog.outCheat("Movement anticheat: %s is speed exception. {real_delta=%f allowed_delta=%f | current_speed=%f preview_speed=%f time=%f}(%f %f %f %d)->(%f %f %f)[%X][%s]",
				plMover->GetName(),real_delta, allowed_delta, current_speed, plMover->GetSpeed(move_type),time_delta,plMover->GetPositionX(),plMover->GetPositionY(),plMover->GetPositionZ(), 
				plMover->GetMapId(),movementInfo.GetPos()->x,movementInfo.GetPos()->y,movementInfo.GetPos()->z,movementInfo.GetMovementFlags(),LookupOpcodeName(opcode));
            check_passed = false;
        }
        if ((real_delta>4900.0f) && !(real_delta < allowed_delta))
        {
            sLog.outCheat("Movement anticheat: %s is teleport exception. {real_delta=%f allowed_delta=%f | current_speed=%f preview_speed=%f time=%f}(%f %f %f %d)->(%f %f %f)[%X][%s]",
				plMover->GetName(),real_delta, allowed_delta, current_speed, plMover->GetSpeed(move_type),time_delta,plMover->GetPositionX(),plMover->GetPositionY(),plMover->GetPositionZ(),
				plMover->GetMapId(),movementInfo.GetPos()->x,movementInfo.GetPos()->y,movementInfo.GetPos()->z,movementInfo.GetMovementFlags(),LookupOpcodeName(opcode));
            check_passed = false; plMover->m_anti_alarmcount++;
        }
        if (opcode==MSG_MOVE_FALL_LAND) //if (movementInfo.time>GetPlayer()->m_anti_lastspeed_changetime)
        {
            plMover->m_anti_last_vspeed = -2.3f;
            if (plMover->m_anti_lastspeed_changetime != 0) plMover->m_anti_lastspeed_changetime = 0;
        }

        if (!movementInfo.HasMovementFlag(MOVEFLAG_FALLING) && (tg_z > 2.371f) && (delta_z < plMover->m_anti_last_vspeed))
        {
            sLog.outCheat("Movement anticheat: %s is mountain exception. {tg_z=%f} (%f %f %f %d)->(%f %f %f)[%X][%s]",
				plMover->GetName(),tg_z, plMover->GetPositionX(),plMover->GetPositionY(),plMover->GetPositionZ(), plMover->GetMapId(),movementInfo.GetPos()->x,movementInfo.GetPos()->y,movementInfo.GetPos()->z,movementInfo.GetMovementFlags(),LookupOpcodeName(opcode));
            check_passed = false;
        }

        if (movementInfo.HasMovementFlag(MOVEFLAG_LEVITATING) && !GetPlayer()->isGameMaster() && !GetPlayer()->canFLY)
		{
            sLog.outCheat("Movement anticheat: %s is Levitate exception. (%f %f %f %d)->(%f %f %f)[%X][%s]",
				plMover->GetName(), plMover->GetPositionX(), plMover->GetPositionY(), plMover->GetPositionZ(), plMover->GetMapId(), movementInfo.GetPos()->x, movementInfo.GetPos()->y, movementInfo.GetPos()->z, movementInfo.GetMovementFlags(), LookupOpcodeName(opcode));
			check_passed = false;
        }

        if (movementInfo.HasMovementFlag(MOVEFLAG_HOVER) && !GetPlayer()->isGameMaster() && !GetPlayer()->canHOVER)
		{
            sLog.outCheat("Movement anticheat: %s is Hover exception. (%f %f %f %d)->(%f %f %f)[%X][%s]",
				plMover->GetName(), plMover->GetPositionX(), plMover->GetPositionY(), plMover->GetPositionZ(), plMover->GetMapId(), movementInfo.GetPos()->x, movementInfo.GetPos()->y, movementInfo.GetPos()->z, movementInfo.GetMovementFlags(), LookupOpcodeName(opcode));
			check_passed = false;
        }

        if ((movementInfo.HasMovementFlag(MOVEFLAG_FLYING) || plMover->m_movementInfo.HasMovementFlag(MOVEFLAG_FLYING2)) && !plMover->canFLY && !plMover->isGameMaster() && (opcode!=201))// && !(plMover->HasAuraType(SPELL_AURA_FLY) || plMover->HasAuraType(SPELL_AURA_MOD_INCREASE_FLIGHT_SPEED)))
        {
            sLog.outCheat("Movement anticheat: %s is fly cheater (%f %f %f %d)[%X][%s]. {SPELL_AURA_FLY=[%X]}",
               plMover->GetName(), movementInfo.GetPos()->x, movementInfo.GetPos()->y, movementInfo.GetPos()->z, plMover->GetMapId(), movementInfo.GetMovementFlags(), LookupOpcodeName(opcode), plMover->HasAuraType(SPELL_AURA_FLY));
            check_passed = false;
        }

		if (movementInfo.HasMovementFlag(MOVEFLAG_WATERWALKING) && !plMover->isGameMaster() && !plMover->canWWALK)//(plMover->HasAuraType(SPELL_AURA_WATER_WALK) | plMover->HasAuraType(SPELL_AURA_GHOST)))
        {
			sLog.outCheat("Movement anticheat: %s is water-walk exception (%f %f %f %d)->(%f %f %f)[%s]. [%X]{SPELL_AURA_WATER_WALK=[%X]}", plMover->GetName(),plMover->GetPositionX(),plMover->GetPositionY(),plMover->GetPositionZ(),plMover->GetMapId(),movementInfo.GetPos()->x,movementInfo.GetPos()->y,movementInfo.GetPos()->z, LookupOpcodeName(opcode),movementInfo.GetMovementFlags(), plMover->HasAuraType(SPELL_AURA_WATER_WALK));
            check_passed = false;
        }
		if(movementInfo.GetPos()->z == 0.0f && !(movementInfo.HasMovementFlag(MOVEFLAG_SWIMMING) || plMover->m_movementInfo.HasMovementFlag(MOVEFLAG_CAN_FLY) || plMover->m_movementInfo.HasMovementFlag(MOVEFLAG_FLYING) || plMover->m_movementInfo.HasMovementFlag(MOVEFLAG_FLYING2)) && !plMover->isGameMaster() )
        {
            // Prevent using TeleportToPlan.
            Map *map = plMover->GetMap();
            if (map){
				float plane_z = map->GetTerrain()->GetHeight(movementInfo.GetPos()->x, movementInfo.GetPos()->y, MAX_HEIGHT) - movementInfo.GetPos()->z;
                plane_z = (plane_z < -500.0f) ? 0 : plane_z; //check holes in heigth map
                if(plane_z > 0.1f || plane_z < -0.1f)
                {
                    plMover->m_anti_teletoplane_count++;
                    check_passed = false;
                    sLog.outCheat("Movement anticheat: %s is teleport to plan exception. plane_z: %f ", plMover->GetName(), plane_z);
                    if (plMover->m_anti_teletoplane_count > World::GetTeleportToPlaneAlarms())
                    {
                        plMover->GetSession()->KickPlayer();
                        sLog.outCheat("Movement anticheat: %s is teleport to plan exception. Exception count: %d ", plMover->GetName(), plMover->m_anti_teletoplane_count);
                    }
                }
            }
        } else {
            if (plMover->m_anti_teletoplane_count !=0)
                plMover->m_anti_teletoplane_count = 0;
        }
	} else if (movementInfo.HasMovementFlag(MOVEFLAG_ONTRANSPORT) && opcode!=CMSG_MOVE_CHNG_TRANSPORT) {
            //antiwrap =)
        if (plMover->m_transport)
        {
            float trans_rad = movementInfo.GetTransportPos()->x*movementInfo.GetTransportPos()->x + movementInfo.GetTransportPos()->y*movementInfo.GetTransportPos()->y + movementInfo.GetTransportPos()->z*movementInfo.GetTransportPos()->z;
            if (trans_rad > 3600.0f)
			{
				if (sWorld.TargetGuid==plMover->GetGUIDLow())
				sLog.outBasic("trans_rad: %f", trans_rad);
                check_passed = false;
			}
        } else { 
            if (GameObjectData const* go_data = sObjectMgr.GetGOData(plMover->m_anti_transportGUID))
            {
                float delta_gox = go_data->posX - movementInfo.GetPos()->x;
                float delta_goy = go_data->posY - movementInfo.GetPos()->y;
                float delta_goz = go_data->posZ - movementInfo.GetPos()->z;
                int mapid = go_data->mapid;
				if (sWorld.TargetGuid==plMover->GetGUIDLow())
					sLog.outBasic("Movement anticheat: %s on some transport. xyzo: %f,%f,%f", plMover->GetName(), go_data->posX,go_data->posY,go_data->posZ);
                if (plMover->GetMapId() != mapid){
                    check_passed = false;
                } else if (mapid !=369 && mapid!=554) {
                    float delta_go = delta_gox*delta_gox + delta_goy*delta_goy;
                    if (delta_go > 3600.0f)
					{
						#ifdef MOVEMENT_ANTICHEAT_DEBUG
						sLog.outBasic("delta_go: %f", delta_go);
						#endif
                        check_passed = false;
					}
                }

            } else {
                sLog.outCheat("Movement anticheat: %s on undefined transport.", plMover->GetName());
				check_passed = false;}
			}
		}
	}
    /* process position-change */
    if (plMover && check_passed)
    {
        if (plMover->m_anti_alarmcount > 0)
		{
            sLog.outCheat("Movement anticheat: %s produce %d anticheat alarms",plMover->GetName(),plMover->m_anti_alarmcount);
			ChatHandler(plMover).PSendSysMessage("Movement anticheat: you produce %d anticheat alarms",plMover->m_anti_alarmcount);
            plMover->m_anti_mistiming_count = plMover->m_anti_alarmcount + plMover->m_anti_mistiming_count;
            plMover->m_anti_alarmcount = 0;
		    if (plMover->m_anti_mistiming_count > sWorld.GetMistimingAlarms())
		    {
			    std::string args="";
			    args.append(plMover->GetName());
			    args.append(" 72 Jailed-=-=-Player-=-=-Cheater");
				ChatHandler(this).Jail((char*)args.c_str());
				return;
		    }
        }
    } else 
	if (plMover)
	{
		recv_data.hexlikemy();
        plMover->m_anti_alarmcount++;
		if (plMover->m_anti_mistiming_count+plMover->m_anti_alarmcount > sWorld.GetMistimingAlarms())
		{
			std::string args="";
			args.append(plMover->GetName());
			args.append(" 72 Jailed-=-=-Player-=-=-Cheater");
			ChatHandler(this).Jail((char*)args.c_str());
			return;
		}
        if (sWorld.GetAlarmKickMvAnticheat()) // if kick cheater enabled
        {
                if (plMover->m_anti_alarmcount > sWorld.GetAlarmCountMvAnticheat())
                {
                    sLog.outCheat("Movement anticheat: %s kicked after %d anticheat alarms.",plMover->GetName(),plMover->m_anti_alarmcount);
                    plMover->GetSession()->KickPlayer(); 
                    return;
                }
		}
	}

    HandleMoverRelocation(movementInfo);

    if (plMover)
        plMover->UpdateFallInformationIfNeed(movementInfo, opcode);

    // after move info set
    if (opcode == MSG_MOVE_SET_WALK_MODE || opcode == MSG_MOVE_SET_RUN_MODE)
        mover->UpdateWalkMode(mover, false);

    WorldPacket data(opcode, recv_data.size());
    data << mover->GetPackGUID();             // write guid
    movementInfo.Write(data);                               // write data
    mover->SendMessageToSetExcept(&data, _player);

	/*if (_player->m_mover != _player)	//kia patch MK
	{
		if (_player->GetDistance2d(_player->m_mover) > 50.0f)
			_player->CastStop();
	}*/
}

void WorldSession::HandleForceSpeedChangeAckOpcodes(WorldPacket &recv_data)
{
    uint32 opcode = recv_data.GetOpcode();
    DEBUG_LOG("WORLD: Recvd %s (%u, 0x%X) opcode", LookupOpcodeName(opcode), opcode, opcode);

    /* extract packet */
    ObjectGuid guid;
    MovementInfo movementInfo;
    float  newspeed;

    recv_data >> guid;
    recv_data >> Unused<uint32>();                          // counter or moveEvent
    recv_data >> movementInfo;
    recv_data >> newspeed;
	GetPlayer()->GetMover()->ShowMovementInfo(GetPlayer()->GetGUIDLow(), movementInfo, LookupOpcodeName(recv_data.GetOpcode())); 

    // now can skip not our packet
    if(_player->GetObjectGuid() != guid)
        return;

    /*----------------*/

    // client ACK send one packet for mounted/run case and need skip all except last from its
    // in other cases anti-cheat check can be fail in false case
    UnitMoveType move_type;
    UnitMoveType force_move_type;

    static char const* move_type_name[MAX_MOVE_TYPE] = {  "Walk", "Run", "RunBack", "Swim", "SwimBack", "TurnRate", "Flight", "FlightBack" };

    switch(opcode)
    {
        case CMSG_FORCE_WALK_SPEED_CHANGE_ACK:          move_type = MOVE_WALK;          force_move_type = MOVE_WALK;        break;
        case CMSG_FORCE_RUN_SPEED_CHANGE_ACK:           move_type = MOVE_RUN;           force_move_type = MOVE_RUN;         break;
        case CMSG_FORCE_RUN_BACK_SPEED_CHANGE_ACK:      move_type = MOVE_RUN_BACK;      force_move_type = MOVE_RUN_BACK;    break;
        case CMSG_FORCE_SWIM_SPEED_CHANGE_ACK:          move_type = MOVE_SWIM;          force_move_type = MOVE_SWIM;        break;
        case CMSG_FORCE_SWIM_BACK_SPEED_CHANGE_ACK:     move_type = MOVE_SWIM_BACK;     force_move_type = MOVE_SWIM_BACK;   break;
        case CMSG_FORCE_TURN_RATE_CHANGE_ACK:           move_type = MOVE_TURN_RATE;     force_move_type = MOVE_TURN_RATE;   break;
        case CMSG_FORCE_FLIGHT_SPEED_CHANGE_ACK:        move_type = MOVE_FLIGHT;        force_move_type = MOVE_FLIGHT;      break;
        case CMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE_ACK:   move_type = MOVE_FLIGHT_BACK;   force_move_type = MOVE_FLIGHT_BACK; break;
        default:
            sLog.outError("WorldSession::HandleForceSpeedChangeAck: Unknown move type opcode: %u", opcode);
            return;
    }

	if (GetPlayer()->GetMover()->GetTypeId() == TYPEID_PLAYER)
	{
		if (GetPlayer()->IsInWorld())	//kia
		{
			((Player*)GetPlayer()->GetMover())->m_movementInfo = movementInfo;
			((Player*)GetPlayer()->GetMover())->SetPosition(movementInfo.GetPos()->x,movementInfo.GetPos()->y,movementInfo.GetPos()->z,movementInfo.GetPos()->o);
		}
	}
	else
		((Creature*)GetPlayer()->GetMover())->Relocate(movementInfo.GetPos()->x,movementInfo.GetPos()->y,movementInfo.GetPos()->z,movementInfo.GetPos()->o);

	GetPlayer()->m_anti_last_hspeed[move_type] = GetPlayer()->GetSpeed(move_type);
	if (sWorld.TargetGuid==GetPlayer()->GetGUIDLow())
		sLog.outBasic("mt: %s nspd: %f spd: %f", move_type_name[move_type], newspeed, GetPlayer()->GetSpeed(move_type));


	// skip all forced speed changes except last and unexpected
    // in run/mounted case used one ACK and it must be skipped.m_forced_speed_changes[MOVE_RUN} store both.
    if(_player->m_forced_speed_changes[force_move_type] > 0)
    {
        --_player->m_forced_speed_changes[force_move_type];
        if(_player->m_forced_speed_changes[force_move_type] > 0)
            return;
    }

    if (!_player->GetTransport() && fabs(_player->GetSpeed(move_type) - newspeed) > 0.01f)
    {
        if(_player->GetSpeed(move_type) > newspeed)         // must be greater - just correct
        {
            sLog.outError("%sSpeedChange player %s is NOT correct (must be %f instead %f), force set to correct value",
                move_type_name[move_type], _player->GetName(), _player->GetSpeed(move_type), newspeed);
            _player->SetSpeedRate(move_type,_player->GetSpeedRate(move_type),true);
        }
        else                                                // must be lesser - cheating
        {
            BASIC_LOG("Player %s from account id %u kicked for incorrect speed (must be %f instead %f)",
                _player->GetName(),_player->GetSession()->GetAccountId(),_player->GetSpeed(move_type), newspeed);
            _player->GetSession()->KickPlayer();
        }
    }
}

void WorldSession::HandleSetActiveMoverOpcode(WorldPacket &recv_data)
{
    DEBUG_LOG("WORLD: Recvd CMSG_SET_ACTIVE_MOVER");
    recv_data.hexlike();

    ObjectGuid guid;
    recv_data >> guid;

	if (!_player || !_player->GetMover() || !_player->IsInWorld())
		return;

    if(_player->GetMover()->GetObjectGuid() != guid)
    {
        sLog.outError("HandleSetActiveMoverOpcode: incorrect mover guid: mover is %s and should be %s",
            _player->GetMover()->GetObjectGuid().GetString().c_str(), guid.GetString().c_str());
        return;
    }

    WorldPacket data(SMSG_TIME_SYNC_REQ, 4);                // new 2.0.x, enable movement
    data << uint32(0x00000000);                             // on blizz it increments periodically
    SendPacket(&data);
	_player->skip_check = true;
}

void WorldSession::HandleMoveNotActiveMoverOpcode(WorldPacket &recv_data)
{
    DEBUG_LOG("WORLD: Recvd CMSG_MOVE_NOT_ACTIVE_MOVER");
    recv_data.hexlike();

    ObjectGuid old_mover_guid;
    MovementInfo mi;
	unsigned char unk1;

    recv_data >> old_mover_guid.ReadAsPacked();
    recv_data >> mi;
	recv_data >> unk1;

	if (!_player || !_player->GetMover())
		return;

    if(_player->GetMover()->GetObjectGuid() == old_mover_guid)
    {
        sLog.outError("HandleMoveNotActiveMover: incorrect mover guid: mover is %s and should be %s instead of %s",
            _player->GetMover()->GetObjectGuid().GetString().c_str(),
            _player->GetObjectGuid().GetString().c_str(),
            old_mover_guid.GetString().c_str());
        recv_data.rpos(recv_data.wpos());                   // prevent warnings spam
        return;
    }
	GetPlayer()->GetMover()->ShowMovementInfo(GetPlayer()->GetGUIDLow(), mi, LookupOpcodeName(recv_data.GetOpcode())); 

	if (GetPlayer()->GetMover()->GetTypeId() == TYPEID_PLAYER)
	{
		((Player*)GetPlayer()->GetMover())->m_movementInfo = mi;
		((Player*)GetPlayer()->GetMover())->SetPosition(mi.GetPos()->x,mi.GetPos()->y,mi.GetPos()->z,mi.GetPos()->o);
	}
	else
		((Creature*)GetPlayer()->GetMover())->Relocate(mi.GetPos()->x,mi.GetPos()->y,mi.GetPos()->z,mi.GetPos()->o);
}

void WorldSession::HandleMountSpecialAnimOpcode(WorldPacket& /*recvdata*/)
{
    //DEBUG_LOG("WORLD: Recvd CMSG_MOUNTSPECIAL_ANIM");

    WorldPacket data(SMSG_MOUNTSPECIAL_ANIM, 8);
    data << GetPlayer()->GetObjectGuid();

    GetPlayer()->SendMessageToSet(&data, false);
}

void WorldSession::HandleMoveKnockBackAck( WorldPacket & recv_data )
{
    DEBUG_LOG("CMSG_MOVE_KNOCK_BACK_ACK");
    // Currently not used but maybe use later for recheck final player position
    // (must be at call same as into "recv_data >> x >> y >> z >> orientation;"

    Unit *mover = _player->GetMover();
    Player *plMover = mover->GetTypeId() == TYPEID_PLAYER ? (Player*)mover : NULL;

    ObjectGuid guid;
    MovementInfo movementInfo;

    recv_data >> guid;

    // ignore, waiting processing in WorldSession::HandleMoveWorldportAckOpcode and WorldSession::HandleMoveTeleportAck
	if (GetPlayer()->GetGUID()!=guid.GetRawValue())//(plMover && plMover->IsBeingTeleported())
    {
        recv_data.rpos(recv_data.wpos());                   // prevent warnings spam
        return;
    }

    recv_data >> Unused<uint32>();                          // knockback packets counter
    recv_data >> movementInfo;
	GetPlayer()->GetMover()->ShowMovementInfo(GetPlayer()->GetGUIDLow(), movementInfo, LookupOpcodeName(recv_data.GetOpcode())); 

    if (!VerifyMovementInfo(movementInfo, guid))
        return;

    HandleMoverRelocation(movementInfo);

    WorldPacket data(MSG_MOVE_KNOCK_BACK, recv_data.size() + 15);
    data << ObjectGuid(mover->GetObjectGuid());
    data << movementInfo;
    data << movementInfo.GetJumpInfo().sinAngle;
    data << movementInfo.GetJumpInfo().cosAngle;
    data << movementInfo.GetJumpInfo().xyspeed;
    data << movementInfo.GetJumpInfo().velocity;
    mover->SendMessageToSetExcept(&data, _player);

    //Save movement flags

	if (GetPlayer()->GetMover()->GetTypeId() == TYPEID_PLAYER)
	{
		((Player*)GetPlayer()->GetMover())->m_movementInfo = movementInfo;
		((Player*)GetPlayer()->GetMover())->SetPosition(movementInfo.GetPos()->x,movementInfo.GetPos()->y,movementInfo.GetPos()->z,movementInfo.GetPos()->o);
		((Player*)GetPlayer()->GetMover())->m_anti_last_vspeed = movementInfo.GetJumpInfo().velocity < 3.2f ? movementInfo.GetJumpInfo().velocity - 1.0f : 3.2f;
		((Player*)GetPlayer()->GetMover())->m_anti_lastspeed_changetime = movementInfo.time + 1750;
	}
	else
		((Creature*)GetPlayer()->GetMover())->Relocate(movementInfo.GetPos()->x,movementInfo.GetPos()->y,movementInfo.GetPos()->z,movementInfo.GetPos()->o);
}

void WorldSession::HandleMoveHoverAck( WorldPacket& recv_data )
{
    DEBUG_LOG("CMSG_MOVE_HOVER_ACK");

	uint64 guid;

    recv_data >> guid;

	// skip not personal message;
    if(GetPlayer()->GetGUID()!=guid)
	{
		recv_data.rpos(recv_data.wpos());                   // prevent warnings spam
	    return;
	}

    recv_data >> Unused<uint32>();                          // unk

    MovementInfo movementInfo;

    recv_data >> movementInfo;
	GetPlayer()->GetMover()->ShowMovementInfo(GetPlayer()->GetGUIDLow(), movementInfo, LookupOpcodeName(recv_data.GetOpcode())); 

	if (GetPlayer()->GetMover()->GetTypeId() == TYPEID_PLAYER)
	{
        ((Player*)GetPlayer()->GetMover())->canHOVER = movementInfo.HasMovementFlag(MOVEFLAG_HOVER);
		((Player*)GetPlayer()->GetMover())->m_movementInfo = movementInfo;
		((Player*)GetPlayer()->GetMover())->SetPosition(movementInfo.GetPos()->x,movementInfo.GetPos()->y,movementInfo.GetPos()->z,movementInfo.GetPos()->o);
	}
	else
		((Creature*)GetPlayer()->GetMover())->Relocate(movementInfo.GetPos()->x,movementInfo.GetPos()->y,movementInfo.GetPos()->z,movementInfo.GetPos()->o);

    recv_data >> Unused<uint32>();                          // unk2
}

void WorldSession::HandleMoveWaterWalkAck(WorldPacket& recv_data)
{
    DEBUG_LOG("CMSG_MOVE_WATER_WALK_ACK");

	uint64 guid;

	recv_data >> guid;

	// skip not personal message;
    if(GetPlayer()->GetGUID()!=guid)
	{
		recv_data.rpos(recv_data.wpos());                   // prevent warnings spam
	    return;
	}

    recv_data.read_skip<uint32>();                          // unk

    MovementInfo movementInfo;

    recv_data >> movementInfo;
	GetPlayer()->GetMover()->ShowMovementInfo(GetPlayer()->GetGUIDLow(), movementInfo, LookupOpcodeName(recv_data.GetOpcode())); 

	if (GetPlayer()->GetMover()->GetTypeId() == TYPEID_PLAYER)
	{
		((Player*)GetPlayer()->GetMover())->canWWALK = (movementInfo.HasMovementFlag(MOVEFLAG_WATERWALKING));
		((Player*)GetPlayer()->GetMover())->m_movementInfo = movementInfo;
		((Player*)GetPlayer()->GetMover())->SetPosition(movementInfo.GetPos()->x,movementInfo.GetPos()->y,movementInfo.GetPos()->z,movementInfo.GetPos()->o);
	}
	else
		((Creature*)GetPlayer()->GetMover())->Relocate(movementInfo.GetPos()->x,movementInfo.GetPos()->y,movementInfo.GetPos()->z,movementInfo.GetPos()->o);

    recv_data >> Unused<uint32>();                          // unk2
}

void WorldSession::HandleSummonResponseOpcode(WorldPacket& recv_data)
{
    if(!_player->isAlive() || _player->isInCombat() )
        return;

    uint64 summoner_guid;
    bool agree;
    recv_data >> summoner_guid;
    recv_data >> agree;

    _player->SummonIfPossible(agree);
}

bool WorldSession::VerifyMovementInfo(MovementInfo const& movementInfo, ObjectGuid const& guid) const
{
    // ignore wrong guid (player attempt cheating own session for not own guid possible...)
    if (guid != _player->GetMover()->GetObjectGuid())
        return false;

    return VerifyMovementInfo(movementInfo);
}

bool WorldSession::VerifyMovementInfo(MovementInfo const& movementInfo) const
{
    if (!MaNGOS::IsValidMapCoord(movementInfo.GetPos()->x, movementInfo.GetPos()->y, movementInfo.GetPos()->z, movementInfo.GetPos()->o))
        return false;

    if (movementInfo.HasMovementFlag(MOVEFLAG_ONTRANSPORT))
    {
        // transports size limited
        // (also received at zeppelin/lift leave by some reason with t_* as absolute in continent coordinates, can be safely skipped)
        if( movementInfo.GetTransportPos()->x > 50 || movementInfo.GetTransportPos()->y > 50 || movementInfo.GetTransportPos()->z > 100 )
            return false;

        if( !MaNGOS::IsValidMapCoord(movementInfo.GetPos()->x + movementInfo.GetTransportPos()->x, movementInfo.GetPos()->y + movementInfo.GetTransportPos()->y,
            movementInfo.GetPos()->z + movementInfo.GetTransportPos()->z, movementInfo.GetPos()->o + movementInfo.GetTransportPos()->o) )
        {
            return false;
        }
    }

    return true;
}

void WorldSession::HandleMoverRelocation(MovementInfo& movementInfo)
{
    movementInfo.UpdateTime(WorldTimer::getMSTime());

    Unit *mover = _player->GetMover();

    if (Player *plMover = mover->GetTypeId() == TYPEID_PLAYER ? (Player*)mover : NULL)
    {
        if (movementInfo.HasMovementFlag(MOVEFLAG_ONTRANSPORT))
        {
            if (!plMover->m_transport)
            {
                // elevators also cause the client to send MOVEFLAG_ONTRANSPORT - just unmount if the guid can be found in the transport list
                for (MapManager::TransportSet::const_iterator iter = sMapMgr.m_Transports.begin(); iter != sMapMgr.m_Transports.end(); ++iter)
                {
                    if ((*iter)->GetObjectGuid() == movementInfo.GetTransportGuid())
                    {
                        plMover->m_transport = (*iter);
                        (*iter)->AddPassenger(plMover);
                        break;
                    }
                }
            }
        }
        else if (plMover->m_transport)               // if we were on a transport, leave
        {
            plMover->m_transport->RemovePassenger(plMover);
            plMover->m_transport = NULL;
            movementInfo.ClearTransportData();
        }

        if (movementInfo.HasMovementFlag(MOVEFLAG_SWIMMING) != plMover->IsInWater())
        {
            // now client not include swimming flag in case jumping under water
            plMover->SetInWater( !plMover->IsInWater() || plMover->GetTerrain()->IsUnderWater(movementInfo.GetPos()->x, movementInfo.GetPos()->y, movementInfo.GetPos()->z) );
        }

        plMover->SetPosition(movementInfo.GetPos()->x, movementInfo.GetPos()->y, movementInfo.GetPos()->z, movementInfo.GetPos()->o);
        plMover->m_movementInfo = movementInfo;

        if(movementInfo.GetPos()->z < -500.0f)
        {
            if(plMover->InBattleGround()
                && plMover->GetBattleGround()
                && plMover->GetBattleGround()->HandlePlayerUnderMap(_player))
            {
                // do nothing, the handle already did if returned true
            }
            else
            {
                // NOTE: this is actually called many times while falling
                // even after the player has been teleported away
                // TODO: discard movement packets after the player is rooted
                if(plMover->isAlive())
                {
                    plMover->EnvironmentalDamage(DAMAGE_FALL_TO_VOID, plMover->GetMaxHealth());
                    // pl can be alive if GM/etc
                    if(!plMover->isAlive())
                    {
                        // change the death state to CORPSE to prevent the death timer from
                        // starting in the next player update
                        plMover->KillPlayer();
                        plMover->BuildPlayerRepop();
                    }
                }

                // cancel the death timer here if started
                plMover->RepopAtGraveyard();
            }
        }
    }
    else                                                    // creature charmed
    {
        if (mover->IsInWorld())
            mover->GetMap()->CreatureRelocation((Creature*)mover, movementInfo.GetPos()->x, movementInfo.GetPos()->y, movementInfo.GetPos()->z, movementInfo.GetPos()->o);
    }
}
