#include <omp.h>
#include "Common.h"
#include "Log.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "World.h"
#include "ObjectMgr.h"
#include "SpellMgr.h"
#include "Unit.h"
#include "QuestDef.h"
#include "Player.h"
#include "Creature.h"
#include "Spell.h"
#include "Group.h"
#include "SpellAuras.h"
#include "MapManager.h"
#include "ObjectAccessor.h"
#include "CreatureAI.h"
#include "CreatureAINew.h"
#include "Formulas.h"
#include "Pet.h"
#include "Util.h"
#include "Totem.h"
#include "BattleGround.h"
#include "OutdoorPvP.h"
#include "InstanceSaveMgr.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "Path.h"
#include "PathFinder.h"
#include "CreatureGroups.h"
#include "PetAI.h"
#include "NullCreatureAI.h"
#include "ScriptCalls.h"
#include "../scripts/ScriptMgr.h"
#include "InstanceData.h"

#include <math.h>

float baseMoveSpeed[MAX_MOVE_TYPE] =
{
    2.5f,                                                   // MOVE_WALK
    7.0f,                                                   // MOVE_RUN
    1.25f,                                                  // MOVE_RUN_BACK
    4.722222f,                                              // MOVE_SWIM
    4.5f,                                                   // MOVE_SWIM_BACK
    3.141594f,                                              // MOVE_TURN_RATE
    7.0f,                                                   // MOVE_FLIGHT
    4.5f,                                                   // MOVE_FLIGHT_BACK
};

void InitTriggerAuraData();

// auraTypes contains attacker auras capable of proc'ing cast auras
static Unit::AuraTypeSet GenerateAttakerProcCastAuraTypes()
{
    static Unit::AuraTypeSet auraTypes;
    auraTypes.insert(SPELL_AURA_DUMMY);
    auraTypes.insert(SPELL_AURA_PROC_TRIGGER_SPELL);
    auraTypes.insert(SPELL_AURA_MOD_HASTE);
    auraTypes.insert(SPELL_AURA_OVERRIDE_CLASS_SCRIPTS);
    return auraTypes;
}

// auraTypes contains victim auras capable of proc'ing cast auras
static Unit::AuraTypeSet GenerateVictimProcCastAuraTypes()
{
    static Unit::AuraTypeSet auraTypes;
    auraTypes.insert(SPELL_AURA_DUMMY);
    auraTypes.insert(SPELL_AURA_PRAYER_OF_MENDING);
    auraTypes.insert(SPELL_AURA_PROC_TRIGGER_SPELL);
    return auraTypes;
}

// auraTypes contains auras capable of proc effect/damage (but not cast) for attacker
static Unit::AuraTypeSet GenerateAttakerProcEffectAuraTypes()
{
    static Unit::AuraTypeSet auraTypes;
    auraTypes.insert(SPELL_AURA_MOD_DAMAGE_DONE);
    auraTypes.insert(SPELL_AURA_PROC_TRIGGER_DAMAGE);
    auraTypes.insert(SPELL_AURA_MOD_CASTING_SPEED);
    auraTypes.insert(SPELL_AURA_MOD_RATING);
    return auraTypes;
}

// auraTypes contains auras capable of proc effect/damage (but not cast) for victim
static Unit::AuraTypeSet GenerateVictimProcEffectAuraTypes()
{
    static Unit::AuraTypeSet auraTypes;
    auraTypes.insert(SPELL_AURA_MOD_RESISTANCE);
    auraTypes.insert(SPELL_AURA_PROC_TRIGGER_DAMAGE);
    auraTypes.insert(SPELL_AURA_MOD_PARRY_PERCENT);
    auraTypes.insert(SPELL_AURA_MOD_BLOCK_PERCENT);
    auraTypes.insert(SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN);
    return auraTypes;
}

static Unit::AuraTypeSet attackerProcCastAuraTypes = GenerateAttakerProcCastAuraTypes();
static Unit::AuraTypeSet attackerProcEffectAuraTypes = GenerateAttakerProcEffectAuraTypes();

static Unit::AuraTypeSet victimProcCastAuraTypes = GenerateVictimProcCastAuraTypes();
static Unit::AuraTypeSet victimProcEffectAuraTypes   = GenerateVictimProcEffectAuraTypes();

// auraTypes contains auras capable of proc'ing for attacker and victim
static Unit::AuraTypeSet GenerateProcAuraTypes()
{
    InitTriggerAuraData();

    Unit::AuraTypeSet auraTypes;
    auraTypes.insert(attackerProcCastAuraTypes.begin(),attackerProcCastAuraTypes.end());
    auraTypes.insert(attackerProcEffectAuraTypes.begin(),attackerProcEffectAuraTypes.end());
    auraTypes.insert(victimProcCastAuraTypes.begin(),victimProcCastAuraTypes.end());
    auraTypes.insert(victimProcEffectAuraTypes.begin(),victimProcEffectAuraTypes.end());
    return auraTypes;
}

static Unit::AuraTypeSet procAuraTypes = GenerateProcAuraTypes();

bool IsPassiveStackableSpell( uint32 spellId )
{
    if(!IsPassiveSpell(spellId))
        return false;

    SpellEntry const* spellProto = spellmgr.LookupSpell(spellId);
    if(!spellProto)
        return false;

    for(int j = 0; j < 3; ++j)
    {
        if(std::find(procAuraTypes.begin(),procAuraTypes.end(),spellProto->EffectApplyAuraName[j])!=procAuraTypes.end())
            return false;
    }

    return true;
}

Unit::Unit()
: WorldObject(), i_motionMaster(this), m_ThreatManager(this), m_HostilRefManager(this)
, m_IsInNotifyList(false), m_Notified(false), IsAIEnabled(false), NeedChangeAI(false)
, i_AI(NULL), i_disabledAI(NULL), m_removedAurasCount(0), m_procDeep(0), m_unitTypeMask(UNIT_MASK_NONE)
, _lastDamagedTime(0)
{
    m_objectType |= TYPEMASK_UNIT;
    m_objectTypeId = TYPEID_UNIT;
                                                            // 2.3.2 - 0x70
    m_updateFlag = (UPDATEFLAG_HIGHGUID | UPDATEFLAG_LIVING | UPDATEFLAG_HASPOSITION);

    m_attackTimer[BASE_ATTACK]   = 0;
    m_attackTimer[OFF_ATTACK]    = 0;
    m_attackTimer[RANGED_ATTACK] = 0;
    m_modAttackSpeedPct[BASE_ATTACK] = 1.0f;
    m_modAttackSpeedPct[OFF_ATTACK] = 1.0f;
    m_modAttackSpeedPct[RANGED_ATTACK] = 1.0f;

    m_extraAttacks = 0;
    m_canDualWield = false;
    m_justCCed = 0;

    m_rootTimes = 0;

    m_state = 0;
    m_form = FORM_NONE;
    m_deathState = ALIVE;

    for (uint32 i = 0; i < CURRENT_MAX_SPELL; i++)
        m_currentSpells[i] = NULL;

    m_addDmgOnce = 0;

    for(int i = 0; i < MAX_TOTEM; ++i)
        m_TotemSlot[i]  = 0;

    m_ObjectSlot[0] = m_ObjectSlot[1] = m_ObjectSlot[2] = m_ObjectSlot[3] = 0;
    //m_Aura = NULL;
    //m_AurasCheck = 2000;
    //m_removeAuraTimer = 4;
    //tmpAura = NULL;

    m_AurasUpdateIterator = m_Auras.end();
    m_Visibility = VISIBILITY_ON;

    m_interruptMask = 0;
    m_detectInvisibilityMask = 0;
    m_invisibilityMask = 0;
    m_transform = 0;
    m_ShapeShiftFormSpellId = 0;
    m_canModifyStats = false;

    for (int i = 0; i < MAX_SPELL_IMMUNITY; ++i)
        m_spellImmune[i].clear();
    for (int i = 0; i < UNIT_MOD_END; ++i)
    {
        m_auraModifiersGroup[i][BASE_VALUE] = 0.0f;
        m_auraModifiersGroup[i][BASE_PCT] = 1.0f;
        m_auraModifiersGroup[i][TOTAL_VALUE] = 0.0f;
        m_auraModifiersGroup[i][TOTAL_PCT] = 1.0f;
    }
                                                            // implement 50% base damage from offhand
    m_auraModifiersGroup[UNIT_MOD_DAMAGE_OFFHAND][TOTAL_PCT] = 0.5f;

    for (int i = 0; i < 3; i++)
    {
        m_weaponDamage[i][MINDAMAGE] = BASE_MINDAMAGE;
        m_weaponDamage[i][MAXDAMAGE] = BASE_MAXDAMAGE;
    }
    for (int i = 0; i < MAX_STATS; ++i)
        m_createStats[i] = 0.0f;

    m_attacking = NULL;
    m_modMeleeHitChance = 0.0f;
    m_modRangedHitChance = 0.0f;
    m_modSpellHitChance = 0.0f;
    m_baseSpellCritChance = 5;

    m_CombatTimer = 0;
    m_lastManaUse = 0;

    //m_victimThreat = 0.0f;
    for (int i = 0; i < MAX_SPELL_SCHOOL; ++i)
        m_threatModifier[i] = 1.0f;
    m_isSorted = true;
    for (int i = 0; i < MAX_MOVE_TYPE; ++i)
        m_speed_rate[i] = 1.0f;

    m_charmInfo = NULL;
    m_unit_movement_flags = 0;
    m_reducedThreatPercent = 0;
    m_misdirectionTargetGUID = 0;
    m_misdirectionLastTargetGUID = 0;

    // remove aurastates allowing special moves
    for(int i=0; i < MAX_REACTIVE; ++i)
        m_reactiveTimer[i] = 0;
        
    IsRotating = 0;
    m_attackVictimOnEnd = false;
    
    _focusSpell = NULL;
    _targetLocked = false;
}

Unit::~Unit()
{
    // set current spells as deletable
    for (uint32 i = 0; i < CURRENT_MAX_SPELL; i++)
    {
        if (m_currentSpells[i])
        {
            m_currentSpells[i]->SetReferencedFromCurrent(false);
            m_currentSpells[i] = NULL;
        }
    }

    RemoveAllGameObjects();
    RemoveAllDynObjects();
    _DeleteAuras();

    // remove veiw point for spectator
    if (!m_sharedVision.empty())
    {
        for (auto itr : m_sharedVision)
        {
            if(Player* p = GetPlayer(itr))
            {
                if (p->isSpectator() && p->getSpectateFrom())
                {
                    p->getSpectateFrom()->RemovePlayerFromVision(p);
                    if (m_sharedVision.empty())
                        break;
                    --itr;
                } else {
                    RemovePlayerFromVision(p);
                    if (m_sharedVision.empty())
                        break;
                    --itr;
                }
            }
        }
    }

    if(m_charmInfo) delete m_charmInfo;

    assert(!m_attacking);
    assert(m_attackers.empty());
    assert(m_sharedVision.empty());

#ifdef WITH_UNIT_CRASHFIX
    for (unsigned int i = 0; i < TOTAL_AURAS; i++) {
        if (m_modAuras[i]._M_impl._M_node._M_prev == NULL) {
            sLog.outError("AURA:Corrupted m_modAuras _M_prev (%p) at index %d (higuid %d loguid %d)", m_modAuras, i, GetGUIDHigh(), GetGUIDLow());
            m_modAuras[i]._M_impl._M_node._M_prev = m_modAuras[i]._M_impl._M_node._M_next;
        }

        if (m_modAuras[i]._M_impl._M_node._M_next == NULL) {
            sLog.outError("AURA:Corrupted m_modAuras _M_next (%p) at index %d (higuid %d loguid %d)", m_modAuras, i, GetGUIDHigh(), GetGUIDLow());
            m_modAuras[i]._M_impl._M_node._M_next = m_modAuras[i]._M_impl._M_node._M_prev;
        }
    }
#endif
}

void Unit::Update( uint32 p_time )
{
    /*if(p_time > m_AurasCheck)
    {
    m_AurasCheck = 2000;
    _UpdateAura();
    }else
    m_AurasCheck -= p_time;*/

    // WARNING! Order of execution here is important, do not change.
    // Spells must be processed with event system BEFORE they go to _UpdateSpells.
    // Or else we may have some SPELL_STATE_FINISHED spells stalled in pointers, that is bad.
    m_Events.Update( p_time );

    if (!IsInWorld())
        return;

    _UpdateSpells( p_time );
    if (m_justCCed)
        m_justCCed--;

    // update combat timer only for players and pets
    if (IsInCombat() && (GetTypeId() == TYPEID_PLAYER || (this->ToCreature())->IsPet() || (this->ToCreature())->isCharmed()))
    {
        // Check UNIT_STAT_MELEE_ATTACKING or UNIT_STAT_CHASE (without UNIT_STAT_FOLLOW in this case) so pets can reach far away
        // targets without stopping half way there and running off.
        // These flags are reset after target dies or another command is given.
        if( m_HostilRefManager.isEmpty() )
        {
            // m_CombatTimer set at aura start and it will be freeze until aura removing
            if ( m_CombatTimer <= p_time )
                ClearInCombat();
            else
                m_CombatTimer -= p_time;
        }
    }

    //not implemented before 3.0.2
    //if(!HasUnitState(UNIT_STAT_CASTING))
    {
        if(uint32 base_att = getAttackTimer(BASE_ATTACK))
            setAttackTimer(BASE_ATTACK, (p_time >= base_att ? 0 : base_att - p_time) );
        if(uint32 ranged_att = getAttackTimer(RANGED_ATTACK))
            setAttackTimer(RANGED_ATTACK, (p_time >= ranged_att ? 0 : ranged_att - p_time) );
        if(uint32 off_att = getAttackTimer(OFF_ATTACK))
            setAttackTimer(OFF_ATTACK, (p_time >= off_att ? 0 : off_att - p_time) );
    }

    // update abilities available only for fraction of time
    UpdateReactives( p_time );

    ModifyAuraState(AURA_STATE_HEALTHLESS_20_PERCENT, GetHealth() < GetMaxHealth()*0.20f);
    ModifyAuraState(AURA_STATE_HEALTHLESS_35_PERCENT, GetHealth() < GetMaxHealth()*0.35f);

    if(!IsUnitRotating())
        i_motionMaster.UpdateMotion(p_time);
    else
        AutoRotate(p_time);
}

bool Unit::haveOffhandWeapon() const
{
    if(GetTypeId() == TYPEID_PLAYER)
        return (this->ToPlayer())->GetWeaponForAttack(OFF_ATTACK,true);
    else
        return m_canDualWield;
}

void Unit::SendMonsterMoveWithSpeedToCurrentDestination(Player* player)
{
    float x, y, z;
    if(GetMotionMaster()->GetDestination(x, y, z))
        SendMonsterMoveWithSpeed(x, y, z, 0, player);
}

void Unit::SendMonsterMoveWithSpeed(float x, float y, float z, uint32 transitTime, Player* player)
{
    if (!transitTime)
    {
        float dx = x - GetPositionX();
        float dy = y - GetPositionY();
        float dz = z - GetPositionZ();

        float dist = ((dx*dx) + (dy*dy) + (dz*dz));
        if(dist<0)
            dist = 0;
        else
            dist = sqrt(dist);

        double speed = GetSpeed((HasUnitMovementFlag(MOVEMENTFLAG_WALK_MODE)) ? MOVE_WALK : MOVE_RUN);
        if(speed<=0)
            speed = 2.5f;
        speed *= 0.001f;
        transitTime = static_cast<uint32>(dist / speed + 0.5);
    }
    //float orientation = (float)atan2((double)dy, (double)dx);
    SendMonsterMove(x, y, z, transitTime, player);
}

void Unit::SendMonsterStop()
{
    WorldPacket data( SMSG_MONSTER_MOVE, (17 + GetPackGUID().size()) );
    data.append(GetPackGUID());
    data << GetPositionX() << GetPositionY() << GetPositionZ();
    data << getMSTime();
    data << uint8(1);
    SendMessageToSet(&data, true);

    clearUnitState(UNIT_STAT_MOVE);
}

void Unit::SendMonsterMove(float NewPosX, float NewPosY, float NewPosZ, uint32 Time, Player* player)
{
    uint32 flags = SPLINEFLAG_NONE;
    if (!HasUnitMovementFlag(MOVEMENTFLAG_WALK_MODE))  //Is Run mode client side
        flags |= SPLINEFLAG_WALKMODE;

    if (HasUnitMovementFlag(MOVEMENTFLAG_LEVITATING | MOVEMENTFLAG_FLYING))
        flags |= SPLINEFLAG_FLYING;

    WorldPacket data( SMSG_MONSTER_MOVE, (41 + GetPackGUID().size()) );
    data.append(GetPackGUID());

    data << GetPositionX() << GetPositionY() << GetPositionZ();
    data << getMSTime();

    data << uint8(0);
    data << uint32(flags);

    data << Time;                                           // Time in between points
    data << uint32(1);                                      // 1 single waypoint
    data << NewPosX << NewPosY << NewPosZ;                  // the single waypoint Point B

    if(player)
        player->GetSession()->SendPacket(&data);
    else
        SendMessageToSet( &data, true );

    addUnitState(UNIT_STAT_MOVE);
    RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_MOVE|AURA_INTERRUPT_FLAG_TURNING);   
}

void Unit::SetFacing(float ori, WorldObject* obj)
{
    SetOrientation(obj ? GetAngle(obj) : ori);

    WorldPacket data(SMSG_MONSTER_MOVE, (1+12+4+1+(obj ? 8 : 4)+4+4+4+12+GetPackGUID().size()));
    data.append(GetPackGUID());
    data << uint8(0);//unk
    data << GetPositionX() << GetPositionY() << GetPositionZ();
    data << getMSTime();
    if (obj)
    {
        data << uint8(SPLINETYPE_FACING_TARGET);
        data << uint64(obj->GetGUID());
    }
    else
    {
        data << uint8(SPLINETYPE_FACING_ANGLE);
        data << ori;
    }
    data << uint32(SPLINEFLAG_NONE);
    data << uint32(0);//move time 0
    data << uint32(1);//one point
    data << GetPositionX() << GetPositionY() << GetPositionZ();
    SendMessageToSet(&data, true);

    RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TURNING);   
}

/*void Unit::SendMonsterMove(float NewPosX, float NewPosY, float NewPosZ, uint8 type, uint32 MovementFlags, uint32 Time, Player* player)
{
    WorldPacket data( SMSG_MONSTER_MOVE, (41 + GetPackGUID().size()) );
    data.append(GetPackGUID());

    // Point A, starting location
    data << GetPositionX() << GetPositionY() << GetPositionZ();
    // unknown field - unrelated to orientation
    // seems to increment about 1000 for every 1.7 seconds
    // for now, we'll just use mstime
    data << getMSTime();

    data << uint8(type);                                    // unknown
    switch(type)
    {
        case 0:                                             // normal packet
            break;
        case 1:                                             // stop packet
            SendMessageToSet( &data, true );
            return;
        case 3:                                             // not used currently
            data << uint64(0);                              // probably target guid
            break;
        case 4:                                             // not used currently
            data << float(0);                               // probably orientation
            break;
    }

    //Movement Flags (0x0 = walk, 0x100 = run, 0x200 = fly/swim)
    data << uint32((MovementFlags & MOVEMENTFLAG_LEVITATING) ? MOVEFLAG_FLY : MOVEFLAG_WALK);

    data << Time;                                           // Time in between points
    data << uint32(1);                                      // 1 single waypoint
    data << NewPosX << NewPosY << NewPosZ;                  // the single waypoint Point B

    if(player)
        player->GetSession()->SendPacket(&data);
    else
        SendMessageToSet( &data, true );
}*/

void Unit::SendMonsterMoveByPath(Path const& path, uint32 start, uint32 end, uint32 traveltime)
{
    if (!traveltime)
        traveltime = uint32(path.GetTotalLength(start, end) * 32);

    uint32 pathSize = end - start;

    uint32 flags = SPLINEFLAG_NONE;
    if (!HasUnitMovementFlag(MOVEMENTFLAG_WALK_MODE))  //Is Run mode client side
        flags |= SPLINEFLAG_WALKMODE;

    if (HasUnitMovementFlag(MOVEMENTFLAG_LEVITATING | MOVEMENTFLAG_FLYING) || HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_TAXI_FLIGHT))
        flags |= SPLINEFLAG_FLYING;

    uint32 packSize = (flags & SPLINEFLAG_FLYING) ? pathSize*4*3 : 4*3 + (pathSize-1)*4;
    WorldPacket data( SMSG_MONSTER_MOVE, (GetPackGUID().size()+4+4+4+4+1+4+4+4+packSize) );
    data.append(GetPackGUID());
    data << GetPositionX();
    data << GetPositionY();
    data << GetPositionZ();
    data << uint32(getMSTime());
    data << uint8(0);
    data << uint32(flags);
    data << uint32(traveltime);
    data << uint32(pathSize);

    if (flags & SPLINEFLAG_FLYING)
    {
        // sending a taxi flight path
        for (uint32 i = start; i < end; ++i)
        {
            data << float(((Path)path)[i].x);
            data << float(((Path)path)[i].y);
            data << float(((Path)path)[i].z);
        }
    }
    else
    {
        // sending a series of points

        // destination
        data << ((Path)path)[end-1].x;
        data << ((Path)path)[end-1].y;
        data << ((Path)path)[end-1].z;

        // all other points are relative to the center of the path
        float mid_X = (GetPositionX() + ((Path)path)[end-1].x) * 0.5f;
        float mid_Y = (GetPositionY() + ((Path)path)[end-1].y) * 0.5f;
        float mid_Z = (GetPositionZ() + ((Path)path)[end-1].z) * 0.5f;

        for (uint32 i = start; i < end - 1; ++i)
            data.appendPackXYZ(mid_X - ((Path)path)[i].x, mid_Y - ((Path)path)[i].y, mid_Z - ((Path)path)[i].z);
    }

    SendMessageToSet(&data, true);

    addUnitState(UNIT_STAT_MOVE);
    RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_MOVE|AURA_INTERRUPT_FLAG_TURNING);   
}

void Unit::resetAttackTimer(WeaponAttackType type)
{
    m_attackTimer[type] = uint32(GetAttackTime(type) * m_modAttackSpeedPct[type]);
}

bool Unit::IsWithinCombatRange(Unit *obj, float dist2compare) const
{
    if (!obj || !IsInMap(obj)) return false;

    float dx = GetPositionX() - obj->GetPositionX();
    float dy = GetPositionY() - obj->GetPositionY();
    float dz = GetPositionZ() - obj->GetPositionZ();
    float distsq = dx*dx + dy*dy + dz*dz;

    float sizefactor = GetCombatReach() + obj->GetCombatReach();
    float maxdist = dist2compare + sizefactor;

    return distsq < maxdist * maxdist;
}

bool Unit::IsWithinMeleeRange(Unit *obj, float dist) const
{
    if (!obj || !IsInMap(obj)) return false;

    float dx = GetPositionX() - obj->GetPositionX();
    float dy = GetPositionY() - obj->GetPositionY();
    float dz = GetPositionZ() - obj->GetPositionZ();
    float distsq = dx*dx + dy*dy + dz*dz;

    float sizefactor = GetMeleeReach() + obj->GetMeleeReach();
    float maxdist = dist + sizefactor;

    return distsq < maxdist * maxdist;
}

void Unit::GetRandomContactPoint( const Unit* obj, float &x, float &y, float &z, float distance2dMin, float distance2dMax ) const
{
    float combat_reach = GetCombatReach();
    if(combat_reach < 0.1) // sometimes bugged for players
    {
        //sLog.outError("Unit %u (Type: %u) has invalid combat_reach %f",GetGUIDLow(),GetTypeId(),combat_reach);
       // if(GetTypeId() ==  TYPEID_UNIT)
          //  sLog.outError("Creature entry %u has invalid combat_reach", (this->ToCreature())->GetEntry());
        combat_reach = DEFAULT_COMBAT_REACH;
    }
    uint32 attacker_number = getAttackers().size();
    if(attacker_number > 0) --attacker_number;
    GetNearPoint(obj,x,y,z,obj->GetCombatReach(), distance2dMin+(distance2dMax-distance2dMin)*GetMap()->rand_norm()
                 , GetAngle(obj) + (attacker_number ? (M_PI/2 - M_PI * GetMap()->rand_norm()) * (float)attacker_number / combat_reach / 3 : 0));
}

void Unit::StartAutoRotate(uint8 type, uint32 fulltime, double Angle, bool attackVictimOnEnd)
{
    m_attackVictimOnEnd = attackVictimOnEnd;

    if (Angle > 0)
    {
        RotateAngle = Angle;
    }
    else
    {
        if(GetVictim())
            RotateAngle = GetAngle(GetVictim());
        else
            RotateAngle = GetOrientation();
    }

    RotateTimer = fulltime;    
    RotateTimerFull = fulltime;    
    IsRotating = type;
    LastTargetGUID = GetUInt64Value(UNIT_FIELD_TARGET);
    SetTarget(0);
}

void Unit::AutoRotate(uint32 time)
{
    if(!IsRotating)return;
    if(IsRotating == CREATURE_ROTATE_LEFT)
    {
        RotateAngle += (double)time/RotateTimerFull*(double)M_PI*2;
        if (RotateAngle >= M_PI*2)RotateAngle = 0;
    }
    else
    {
        RotateAngle -= (double)time/RotateTimerFull*(double)M_PI*2;
        if (RotateAngle < 0)RotateAngle = M_PI*2;
    }    
    SetOrientation(RotateAngle);
    StopMoving();
    if(RotateTimer <= time)
    {
        IsRotating = CREATURE_ROTATE_NONE;
        RotateAngle = 0;
        RotateTimer = RotateTimerFull;
        if (m_attackVictimOnEnd)
            SetTarget(LastTargetGUID);
    }else RotateTimer -= time;
}

void Unit::RemoveMovementImpairingAuras()
{
    for(AuraMap::iterator iter = m_Auras.begin(); iter != m_Auras.end();)
    {
        if(spellmgr.GetSpellCustomAttr(iter->second->GetId()) & SPELL_ATTR_CU_MOVEMENT_IMPAIR)
            RemoveAura(iter);
        else
            ++iter;
    }
}

void Unit::RemoveSpellsCausingAura(AuraType auraType)
{
    if (auraType >= TOTAL_AURAS) return;
    AuraList::iterator iter, next;
    for (iter = m_modAuras[auraType].begin(); iter != m_modAuras[auraType].end(); iter = next)
    {
        next = iter;
        ++next;

        if (*iter)
        {
            RemoveAurasDueToSpell((*iter)->GetId());
            if (!m_modAuras[auraType].empty())
                next = m_modAuras[auraType].begin();
            else
                return;
        }
    }
}

void Unit::RemoveAuraTypeByCaster(AuraType auraType, uint64 casterGUID)
{
    if (auraType >= TOTAL_AURAS) return;

    for(AuraList::iterator iter = m_modAuras[auraType].begin(); iter != m_modAuras[auraType].end(); )
    {
        Aura *aur = *iter;
        ++iter;

        if (aur)
        {
            uint32 removedAuras = m_removedAurasCount;
            RemoveAurasByCasterSpell(aur->GetId(), casterGUID);
            if (m_removedAurasCount > removedAuras + 1)
                iter = m_modAuras[auraType].begin();
        }
    }
}

void Unit::RemoveAurasWithInterruptFlags(uint32 flag, uint32 except, bool withChanneled)
{
    if(!(m_interruptMask & flag))
        return;

    // interrupt auras
    AuraList::iterator iter;
    for (iter = m_interruptableAuras.begin(); iter != m_interruptableAuras.end(); )
    {
        Aura *aur = *iter;
        ++iter;

        //sLog.outDetail("auraflag:%u flag:%u = %u", aur->GetSpellProto()->AuraInterruptFlags,flag, aur->GetSpellProto()->AuraInterruptFlags & flag);

        if(aur && (aur->GetSpellProto()->AuraInterruptFlags & flag))
        {
            if(aur->IsInUse())
                sLog.outError("Aura %u is trying to remove itself! Flag %u. May cause crash!", aur->GetId(), flag);

            else if(!except || aur->GetId() != except)
            {
                uint32 removedAuras = m_removedAurasCount;

                RemoveAurasDueToSpell(aur->GetId());
                if (m_removedAurasCount > removedAuras + 1)
                    iter = m_interruptableAuras.begin();

            }
        }
    }

    // interrupt channeled spell
    if (withChanneled) {
        if(Spell* spell = m_currentSpells[CURRENT_CHANNELED_SPELL])
            if(spell->getState() == SPELL_STATE_CASTING
                && (spell->m_spellInfo->ChannelInterruptFlags & flag)
                && spell->m_spellInfo->Id != except)
                InterruptNonMeleeSpells(false);
    }

    UpdateInterruptMask();
}

void Unit::UpdateInterruptMask()
{
    m_interruptMask = 0;
    for(AuraList::iterator i = m_interruptableAuras.begin(); i != m_interruptableAuras.end(); ++i)
    {
        if(*i)
            m_interruptMask |= (*i)->GetSpellProto()->AuraInterruptFlags;
    }
    if(Spell* spell = m_currentSpells[CURRENT_CHANNELED_SPELL])
        if(spell->getState() == SPELL_STATE_CASTING)
            m_interruptMask |= spell->m_spellInfo->ChannelInterruptFlags;
}

uint32 Unit::GetAuraCount(uint32 spellId) const
{
    uint32 count = 0;
    for (AuraMap::const_iterator itr = m_Auras.lower_bound(spellEffectPair(spellId, 0)); itr != m_Auras.upper_bound(spellEffectPair(spellId, 0)); ++itr)
    {
        if (!itr->second->GetStackAmount())
            count++;
        else
            count += (uint32)itr->second->GetStackAmount();
    }

    return count;
}

bool Unit::HasAuraType(AuraType auraType) const
{
    return (!m_modAuras[auraType].empty());
}

bool Unit::HasAuraTypeWithFamilyFlags(AuraType auraType, uint32 familyName  ,uint64 familyFlags) const
{
    if(!HasAuraType(auraType)) return false;
    AuraList const &auras = GetAurasByType(auraType);
    for(AuraList::const_iterator itr = auras.begin(); itr != auras.end(); ++itr)
        if(SpellEntry const *iterSpellProto = (*itr)->GetSpellProto())
            if(iterSpellProto->SpellFamilyName == familyName && iterSpellProto->SpellFamilyFlags & familyFlags)
                return true;
    return false;
}

bool Unit::HasAuraWithCaster(uint32 spellId, uint32 effIndex, uint64 owner) const
{
    for(auto itr : m_Auras)
    {
        if(    itr.second->GetId() == spellId
            && itr.second->GetEffIndex() == effIndex
            && itr.second->GetCasterGUID() == owner)
            return true;
    }
    return false;
}

bool Unit::HasAuraWithCasterNot(uint32 spellId, uint32 effIndex, uint64 owner) const
{
    for(auto itr : m_Auras)
    {
         if(   itr.second->GetId() == spellId
            && itr.second->GetEffIndex() == effIndex
            && itr.second->GetCasterGUID() != owner)
            return true;
    }
    return false;
}

/* Called by DealDamage for auras that have a chance to be dispelled on damage taken. */
void Unit::RemoveSpellbyDamageTaken(uint32 damage, uint32 spell)
{
    if (spellmgr.GetSpellCustomAttr(spell) & SPELL_ATTR_CU_CANT_BREAK_CC)
        return;

    // The chance to dispel an aura depends on the damage taken with respect to the casters level.
    uint32 max_dmg = getLevel() > 8 ? 30 * getLevel() - 100 : 50;
    float chance = (float(damage) / max_dmg * 100.0f)*0.8;

    AuraList::iterator i, next;
    for(i = m_ccAuras.begin(); i != m_ccAuras.end(); i = next)
    {
        next = i;
        ++next;

        if(*i && (!spell || (*i)->GetId() != spell) && roll_chance_f(chance))
        {
            RemoveAurasDueToSpell((*i)->GetId());
            if (!m_ccAuras.empty())
                next = m_ccAuras.begin();
            else
                return;
        }
    }
}

uint32 Unit::DealDamage(Unit *pVictim, uint32 damage, CleanDamage const* cleanDamage, DamageEffectType damagetype, SpellSchoolMask damageSchoolMask, SpellEntry const *spellProto, bool durabilityLoss)
{
    if (!pVictim->IsAlive() || pVictim->isInFlight() || pVictim->GetTypeId() == TYPEID_UNIT && (pVictim->ToCreature())->IsInEvadeMode())
        return 0;

    // Kidney Shot
    if (pVictim->HasAura(408) || pVictim->HasAura(8643)) {
        Aura *aur = NULL;
        if (pVictim->HasAura(408))
            aur = pVictim->GetAura(408, 0);
        else if (pVictim->HasAura(8643))
            aur = pVictim->GetAura(8643, 0);
        if (aur) {
            Unit *ksCaster = aur->GetCaster();
            if (ksCaster && ksCaster->GetTypeId() == TYPEID_PLAYER) {
                if (ksCaster->HasSpell(14176))
                    damage *= 1.09f;
                else if (ksCaster->HasSpell(14175))
                    damage *= 1.06f;
                else if (ksCaster->HasSpell(14174))
                    damage *= 1.03f;
            }
        }
    }
    
    // Spell 37224: This hack should be removed one day
    if (HasAura(37224) && spellProto && spellProto->SpellFamilyFlags == 0x1000000000LL && spellProto->SpellIconID == 2562)
        damage += 30;
    
    //You don't lose health from damage taken from another player while in a sanctuary
    //You still see it in the combat log though
    if(pVictim != this && GetTypeId() == TYPEID_PLAYER && pVictim->GetTypeId() == TYPEID_PLAYER)
    {
        const AreaTableEntry *area = GetAreaEntryByAreaID(pVictim->GetAreaId());
        if(area && (area->flags & AREA_FLAG_SANCTUARY || (World::IsZoneSanctuary(area->ID))))       //sanctuary
            return 0;
    }

    //Script Event damage taken
    if( pVictim->GetTypeId()== TYPEID_UNIT && (pVictim->ToCreature())->IsAIEnabled )
    {
        (pVictim->ToCreature())->AI()->DamageTaken(this, damage);
        if ((pVictim->ToCreature())->getAI())
            (pVictim->ToCreature())->getAI()->onDamageTaken(this, damage);

        // Set tagging
        if(!pVictim->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_OTHER_TAGGER) && !(pVictim->ToCreature())->IsPet())
        {
            //Set Loot
            switch(GetTypeId())
            {
                case TYPEID_PLAYER:
                {
                    (pVictim->ToCreature())->SetLootRecipient(this);
                    //Set tagged
                    (pVictim->ToCreature())->SetFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_OTHER_TAGGER);
                    break;
                }
                case TYPEID_UNIT:
                {
                    if((this->ToCreature())->IsPet())
                    {
                        (pVictim->ToCreature())->SetLootRecipient(this->GetOwner());
                        (pVictim->ToCreature())->SetFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_OTHER_TAGGER);
                    }
                    break;
                }
            }
        }
    }

    if (damagetype != NODAMAGE)
    {
       // interrupting auras with AURA_INTERRUPT_FLAG_DAMAGE before checking !damage (absorbed damage breaks that type of auras)
        if (spellProto)
        {
            if (!(spellProto->AttributesEx4 & SPELL_ATTR_EX4_DAMAGE_DOESNT_BREAK_AURAS))
                pVictim->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_DAMAGE, spellProto->Id);
        }
        else
            pVictim->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_DAMAGE, 0);
            
        pVictim->RemoveSpellbyDamageTaken(damage, spellProto ? spellProto->Id : 0);
    }

    if(!damage)
    {
        // Rage from physical damage received .
        if(cleanDamage && cleanDamage->damage && (damageSchoolMask & SPELL_SCHOOL_MASK_NORMAL) && pVictim->GetTypeId() == TYPEID_PLAYER && (pVictim->getPowerType() == POWER_RAGE))
            (pVictim->ToPlayer())->RewardRage(cleanDamage->damage, 0, false);

        return 0;
    }

    DEBUG_LOG("DealDamageStart");

    uint32 health = pVictim->GetHealth();
    sLog.outDebug("deal dmg:%d to health:%d ",damage,health);

    // duel ends when player has 1 or less hp
    bool duel_hasEnded = false;
    if(pVictim->GetTypeId() == TYPEID_PLAYER && (pVictim->ToPlayer())->duel && damage >= (health-1))
    {
        // prevent kill only if killed in duel and killed by opponent or opponent controlled creature
        if((pVictim->ToPlayer())->duel->opponent==this || (pVictim->ToPlayer())->duel->opponent->GetGUID() == GetOwnerGUID())
            damage = health-1;

        duel_hasEnded = true;
    }

    // Rage from Damage made (only from direct weapon damage)
    if( cleanDamage && damagetype==DIRECT_DAMAGE && this != pVictim && GetTypeId() == TYPEID_PLAYER && (getPowerType() == POWER_RAGE))
    {
        uint32 weaponSpeedHitFactor;

        switch(cleanDamage->attackType)
        {
            case BASE_ATTACK:
            {
                if(cleanDamage->hitOutCome == MELEE_HIT_CRIT)
                    weaponSpeedHitFactor = uint32(GetAttackTime(cleanDamage->attackType)/1000.0f * 7);
                else
                    weaponSpeedHitFactor = uint32(GetAttackTime(cleanDamage->attackType)/1000.0f * 3.5f);

                (this->ToPlayer())->RewardRage(damage, weaponSpeedHitFactor, true);

                break;
            }
            case OFF_ATTACK:
            {
                if(cleanDamage->hitOutCome == MELEE_HIT_CRIT)
                    weaponSpeedHitFactor = uint32(GetAttackTime(cleanDamage->attackType)/1000.0f * 3.5f);
                else
                    weaponSpeedHitFactor = uint32(GetAttackTime(cleanDamage->attackType)/1000.0f * 1.75f);

                (this->ToPlayer())->RewardRage(damage, weaponSpeedHitFactor, true);

                break;
            }
            case RANGED_ATTACK:
                break;
        }
    }

    if(pVictim->GetTypeId() == TYPEID_PLAYER && GetTypeId() == TYPEID_PLAYER)
    {
        if((pVictim->ToPlayer())->InBattleGround())
        {
            Player *killer = (this->ToPlayer());
            if(killer != (pVictim->ToPlayer()))
                if(BattleGround *bg = killer->GetBattleGround())
                    bg->UpdatePlayerScore(killer, SCORE_DAMAGE_DONE, damage);
        }
    }

    if (pVictim->GetTypeId() == TYPEID_UNIT && !(pVictim->ToCreature())->IsPet())
    {
        if(!(pVictim->ToCreature())->hasLootRecipient())
            (pVictim->ToCreature())->SetLootRecipient(this);

        if(GetCharmerOrOwnerPlayerOrPlayerItself())
            (pVictim->ToCreature())->LowerPlayerDamageReq(health < damage ?  health : damage);
    }
    
    if (health <= damage)
    {
        DEBUG_LOG("DealDamage: victim just died");
        Kill(pVictim, durabilityLoss);
        
        //Hook for OnPVPKill Event
        if (pVictim->GetTypeId() == TYPEID_PLAYER && GetTypeId() == TYPEID_PLAYER)
        {
            Player *killer = ToPlayer();
            Player *killed = pVictim->ToPlayer();
            sScriptMgr.OnPVPKill(killer, killed);
        }
    }
    else                                                    // if (health <= damage)
    {
        DEBUG_LOG("DealDamageAlive");

        pVictim->ModifyHealth(- (int32)damage);

        if(damagetype != DOT)
        {
            if(!GetVictim())
            /*{
                // if have target and damage pVictim just call AI reaction
                if(pVictim != GetVictim() && pVictim->GetTypeId()==TYPEID_UNIT && (pVictim->ToCreature())->IsAIEnabled)
                    (pVictim->ToCreature())->AI()->AttackedBy(this);
            }
            else*/
            {
                // if not have main target then attack state with target (including AI call)
                if(pVictim != GetVictim() && pVictim->GetTypeId()==TYPEID_UNIT && (pVictim->ToCreature())->IsAIEnabled)
                    (pVictim->ToCreature())->AI()->AttackedBy(this);

                //start melee attacks only after melee hit
                if(!ToCreature() || ToCreature()->GetReactState() != REACT_PASSIVE)
                    Attack(pVictim,(damagetype == DIRECT_DAMAGE));
            }
        }

        if(damagetype == DIRECT_DAMAGE || damagetype == SPELL_DIRECT_DAMAGE)
        {
            //TODO: This is from procflag, I do not know which spell needs this
            //Maim?
            //if (!spellProto || !(spellProto->AuraInterruptFlags&AURA_INTERRUPT_FLAG_DIRECT_DAMAGE))
                pVictim->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_DIRECT_DAMAGE, spellProto ? spellProto->Id : 0);
        }

        if (pVictim->GetTypeId() != TYPEID_PLAYER)
        {
            if(spellProto && IsDamageToThreatSpell(spellProto)) {
                //sLog.outString("DealDamage (IsDamageToThreatSpell), AddThreat : %f * 2 = %f",damage,damage*2);
                pVictim->AddThreat(this, damage*2, damageSchoolMask, spellProto);
            } else {
                float threat = damage * spellmgr.GetSpellThreatModPercent(spellProto);
                //sLog.outString("DealDamage, AddThreat : %f",threat);
                pVictim->AddThreat(this, threat, damageSchoolMask, spellProto);
            }
        }
        else                                                // victim is a player
        {
            // Rage from damage received
            if(this != pVictim && pVictim->getPowerType() == POWER_RAGE)
            {
                uint32 rage_damage = damage + (cleanDamage ? cleanDamage->damage : 0);
                (pVictim->ToPlayer())->RewardRage(rage_damage, 0, false);
            }

            // random durability for items (HIT TAKEN)
            if (roll_chance_f(sWorld.getRate(RATE_DURABILITY_LOSS_DAMAGE)))
            {
              EquipmentSlots slot = EquipmentSlots(GetMap()->urand(0,EQUIPMENT_SLOT_END-1));
                (pVictim->ToPlayer())->DurabilityPointLossForEquipSlot(slot);
            }
        }

        if(GetTypeId()==TYPEID_PLAYER)
        {
            // random durability for items (HIT DONE)
            if (roll_chance_f(sWorld.getRate(RATE_DURABILITY_LOSS_DAMAGE)))
            {
              EquipmentSlots slot = EquipmentSlots(GetMap()->urand(0,EQUIPMENT_SLOT_END-1));
                (this->ToPlayer())->DurabilityPointLossForEquipSlot(slot);
            }
        }

        if (damagetype != NODAMAGE && damage)// && pVictim->GetTypeId() == TYPEID_PLAYER)
        {
            /*const SpellEntry *se = i->second->GetSpellProto();
            next = i; ++next;
            if (spellProto && spellProto->Id == se->Id) // Not drop auras added by self
                continue;
            if( se->AuraInterruptFlags & AURA_INTERRUPT_FLAG_DAMAGE )
            {
                bool remove = true;
                if (se->procFlags & (1<<3))
                {
                    if (!roll_chance_i(se->procChance))
                        remove = false;
                }
                if (remove)
                {
                    pVictim->RemoveAurasDueToSpell(i->second->GetId());
                    // FIXME: this may cause the auras with proc chance to be rerolled several times
                    next = vAuras.begin();
                }
            }
        }*/

            if(pVictim != this && pVictim->GetTypeId() == TYPEID_PLAYER) // does not support creature push_back
            {
                if(damagetype != DOT)
                {
                    if(Spell* spell = pVictim->m_currentSpells[CURRENT_GENERIC_SPELL])
                    {
                        if(spell->getState() == SPELL_STATE_PREPARING)
                        {
                            uint32 interruptFlags = spell->m_spellInfo->InterruptFlags;
                            if(interruptFlags & SPELL_INTERRUPT_FLAG_DAMAGE)
                                pVictim->InterruptNonMeleeSpells(false);
                            else if(interruptFlags & SPELL_INTERRUPT_FLAG_PUSH_BACK)
                                spell->Delayed();
                        }
                    }

                    if(Spell* spell = pVictim->m_currentSpells[CURRENT_CHANNELED_SPELL])
                    {
                        if(spell->getState() == SPELL_STATE_CASTING)
                        {
                            uint32 channelInterruptFlags = spell->m_spellInfo->ChannelInterruptFlags;
                            if (((channelInterruptFlags & CHANNEL_FLAG_DELAY) != 0) && (damagetype != DOT))
                                spell->DelayedChannel();
                        }
                    }
                }
            }
        }

        // last damage from duel opponent
        if(duel_hasEnded)
        {
            assert(pVictim->GetTypeId()==TYPEID_PLAYER);
            Player *he = pVictim->ToPlayer();

            assert(he->duel);

            he->SetHealth(he->GetMaxHealth()/10.0f);

            he->duel->opponent->CombatStopWithPets(true);
            he->CombatStopWithPets(true);

            he->CastSpell(he, 7267, true);                  // beg
            he->DuelComplete(DUEL_WON);
        }
    }

    DEBUG_LOG("DealDamageEnd returned %d damage", damage);

    return damage;
}

void Unit::CastStop(uint32 except_spellid)
{
    for (uint32 i = CURRENT_FIRST_NON_MELEE_SPELL; i < CURRENT_MAX_SPELL; i++)
        if (m_currentSpells[i] && m_currentSpells[i]->m_spellInfo->Id!=except_spellid)
            InterruptSpell(i,false, false);
}

uint32 Unit::CastSpell(Unit* Victim, uint32 spellId, bool triggered, Item *castItem, Aura* triggeredByAura, uint64 originalCaster)
{
    SpellEntry const *spellInfo = spellmgr.LookupSpell(spellId );

    if(!spellInfo)
    {
        sLog.outError("CastSpell: unknown spell id %i by caster: %s %u)", spellId,(GetTypeId()==TYPEID_PLAYER ? "player (GUID:" : "creature (Entry:"),(GetTypeId()==TYPEID_PLAYER ? GetGUIDLow() : GetEntry()));
        return SPELL_FAILED_UNKNOWN;
    }

    return CastSpell(Victim,spellInfo,triggered,castItem,triggeredByAura, originalCaster);
}

uint32 Unit::CastSpell(Unit* Victim,SpellEntry const *spellInfo, bool triggered, Item *castItem, Aura* triggeredByAura, uint64 originalCaster, bool skipHit)
{
    if(!spellInfo)
    {
        sLog.outError("CastSpell: unknown spell by caster: %s %u)", (GetTypeId()==TYPEID_PLAYER ? "player (GUID:" : "creature (Entry:"),(GetTypeId()==TYPEID_PLAYER ? GetGUIDLow() : GetEntry()));
        return SPELL_FAILED_UNKNOWN;
    }

    SpellCastTargets targets;
    uint32 targetMask = spellInfo->Targets;
    //if(targetMask & (TARGET_FLAG_UNIT|TARGET_FLAG_UNK2))
    for(int i = 0; i < 3; ++i)
    {
        if(spellmgr.SpellTargetType[spellInfo->EffectImplicitTargetA[i]] == TARGET_TYPE_UNIT_TARGET)
        {
            /*SpellRangeEntry const* srange = sSpellRangeStore.LookupEntry(spellInfo->rangeIndex);
            if(srange && GetSpellMaxRange(srange) == 0.0f)
            {
                Victim = this;
                break;
            }
            else */if(!Victim)
            {
                sLog.outError("CastSpell: spell id %i by caster: %s %u) does not have unit target", spellInfo->Id,(GetTypeId()==TYPEID_PLAYER ? "player (GUID:" : "creature (Entry:"),(GetTypeId()==TYPEID_PLAYER ? GetGUIDLow() : GetEntry()));
                return SPELL_FAILED_BAD_TARGETS;
            }
            else
                break;
        }
    }
    targets.setUnitTarget(Victim);

    if(targetMask & (TARGET_FLAG_SOURCE_LOCATION|TARGET_FLAG_DEST_LOCATION))
    {
        if(!Victim)
        {
            sLog.outError("CastSpell: spell id %i by caster: %s %u) does not have destination", spellInfo->Id,(GetTypeId()==TYPEID_PLAYER ? "player (GUID:" : "creature (Entry:"),(GetTypeId()==TYPEID_PLAYER ? GetGUIDLow() : GetEntry()));
            return SPELL_FAILED_BAD_TARGETS;
        }
        targets.setDestination(Victim);
    }

    if (castItem)
        DEBUG_LOG("WORLD: cast Item spellId - %i", spellInfo->Id);

    if(!originalCaster && triggeredByAura)
        originalCaster = triggeredByAura->GetCasterGUID();

    Spell *spell = new Spell(this, spellInfo, triggered, originalCaster, NULL, false );

    spell->m_CastItem = castItem;
    spell->m_skipHitCheck = skipHit;
    return spell->prepare(&targets, triggeredByAura);
}

uint32 Unit::CastCustomSpell(Unit* target, uint32 spellId, int32 const* bp0, int32 const* bp1, int32 const* bp2, bool triggered, Item *castItem, Aura* triggeredByAura, uint64 originalCaster)
{
    CustomSpellValues values;
    if(bp0) values.AddSpellMod(SPELLVALUE_BASE_POINT0, *bp0);
    if(bp1) values.AddSpellMod(SPELLVALUE_BASE_POINT1, *bp1);
    if(bp2) values.AddSpellMod(SPELLVALUE_BASE_POINT2, *bp2);
    return CastCustomSpell(spellId, values, target, triggered, castItem, triggeredByAura, originalCaster);
}

uint32 Unit::CastCustomSpell(uint32 spellId, SpellValueMod mod, uint32 value, Unit* target, bool triggered, Item *castItem, Aura* triggeredByAura, uint64 originalCaster)
{
    CustomSpellValues values;
    values.AddSpellMod(mod, value);
    return CastCustomSpell(spellId, values, target, triggered, castItem, triggeredByAura, originalCaster);
}

uint32 Unit::CastCustomSpell(uint32 spellId, CustomSpellValues const &value, Unit* Victim, bool triggered, Item *castItem, Aura* triggeredByAura, uint64 originalCaster)
{
    SpellEntry const *spellInfo = spellmgr.LookupSpell(spellId );
    if(!spellInfo)
    {
        sLog.outError("CastSpell: unknown spell id %i by caster: %s %u)", spellId,(GetTypeId()==TYPEID_PLAYER ? "player (GUID:" : "creature (Entry:"),(GetTypeId()==TYPEID_PLAYER ? GetGUIDLow() : GetEntry()));
        return SPELL_FAILED_UNKNOWN;
    }

    SpellCastTargets targets;
    uint32 targetMask = spellInfo->Targets;

    //check unit target
    for(int i = 0; i < 3; ++i)
    {
        if(spellmgr.SpellTargetType[spellInfo->EffectImplicitTargetA[i]] == TARGET_TYPE_UNIT_TARGET)
        {
            if(!Victim)
            {
                sLog.outError("CastSpell: spell id %i by caster: %s %u) does not have unit target", spellInfo->Id,(GetTypeId()==TYPEID_PLAYER ? "player (GUID:" : "creature (Entry:"),(GetTypeId()==TYPEID_PLAYER ? GetGUIDLow() : GetEntry()));
                return SPELL_FAILED_BAD_TARGETS;
            }
            else
                break;
        }
    }
    targets.setUnitTarget(Victim);

    //check destination
    if(targetMask & (TARGET_FLAG_SOURCE_LOCATION|TARGET_FLAG_DEST_LOCATION))
    {
        if(!Victim)
        {
            sLog.outError("CastSpell: spell id %i by caster: %s %u) does not have destination", spellInfo->Id,(GetTypeId()==TYPEID_PLAYER ? "player (GUID:" : "creature (Entry:"),(GetTypeId()==TYPEID_PLAYER ? GetGUIDLow() : GetEntry()));
            return SPELL_FAILED_BAD_TARGETS;
        }
        targets.setDestination(Victim);
    }

    if(!originalCaster && triggeredByAura)
        originalCaster = triggeredByAura->GetCasterGUID();

    Spell *spell = new Spell(this, spellInfo, triggered, originalCaster );

    if(castItem)
    {
        DEBUG_LOG("WORLD: cast Item spellId - %i", spellInfo->Id);
        spell->m_CastItem = castItem;
    }

    for(CustomSpellValues::const_iterator itr = value.begin(); itr != value.end(); ++itr)
        spell->SetSpellValue(itr->first, itr->second);

    return spell->prepare(&targets, triggeredByAura);
}

// used for scripting
uint32 Unit::CastSpell(float x, float y, float z, uint32 spellId, bool triggered, Item *castItem, Aura* triggeredByAura, uint64 originalCaster)
{
    SpellEntry const *spellInfo = spellmgr.LookupSpell(spellId );

    if(!spellInfo)
    {
        sLog.outError("CastSpell(x,y,z): unknown spell id %i by caster: %s %u)", spellId,(GetTypeId()==TYPEID_PLAYER ? "player (GUID:" : "creature (Entry:"),(GetTypeId()==TYPEID_PLAYER ? GetGUIDLow() : GetEntry()));
        return SPELL_FAILED_UNKNOWN;
    }

    if (castItem)
        DEBUG_LOG("WORLD: cast Item spellId - %i", spellInfo->Id);

    if(!originalCaster && triggeredByAura)
        originalCaster = triggeredByAura->GetCasterGUID();

    Spell *spell = new Spell(this, spellInfo, triggered, originalCaster );

    SpellCastTargets targets;
    targets.setDestination(x, y, z);
    spell->m_CastItem = castItem;
    return spell->prepare(&targets, triggeredByAura);
}

// used for scripting
uint32 Unit::CastSpell(GameObject *go, uint32 spellId, bool triggered, Item *castItem, Aura* triggeredByAura, uint64 originalCaster)
{
    if(!go)
        return SPELL_FAILED_UNKNOWN;

    SpellEntry const *spellInfo = spellmgr.LookupSpell(spellId );

    if(!spellInfo)
    {
        sLog.outError("CastSpell(x,y,z): unknown spell id %i by caster: %s %u)", spellId,(GetTypeId()==TYPEID_PLAYER ? "player (GUID:" : "creature (Entry:"),(GetTypeId()==TYPEID_PLAYER ? GetGUIDLow() : GetEntry()));
        return SPELL_FAILED_UNKNOWN;
    }

    if(!(spellInfo->Targets & ( TARGET_FLAG_OBJECT | TARGET_FLAG_OBJECT_UNK)))
    {
        sLog.outError("CastSpell: spell id %i by caster: %s %u) is not gameobject spell", spellId,(GetTypeId()==TYPEID_PLAYER ? "player (GUID:" : "creature (Entry:"),(GetTypeId()==TYPEID_PLAYER ? GetGUIDLow() : GetEntry()));
        return SPELL_FAILED_UNKNOWN;
    }

    if (castItem)
        DEBUG_LOG("WORLD: cast Item spellId - %i", spellInfo->Id);

    if(!originalCaster && triggeredByAura)
        originalCaster = triggeredByAura->GetCasterGUID();

    Spell *spell = new Spell(this, spellInfo, triggered, originalCaster );

    SpellCastTargets targets;
    targets.setGOTarget(go);
    spell->m_CastItem = castItem;
    return spell->prepare(&targets, triggeredByAura);
}

// Obsolete func need remove, here only for comotability vs another patches
uint32 Unit::SpellNonMeleeDamageLog(Unit *pVictim, uint32 spellID, uint32 damage, bool isTriggeredSpell, bool useSpellDamage)
{
    SpellEntry const *spellInfo = spellmgr.LookupSpell(spellID);
    SpellNonMeleeDamage damageInfo(this, pVictim, spellInfo->Id, spellInfo->SchoolMask);
    damage = SpellDamageBonus(pVictim, spellInfo, damage, SPELL_DIRECT_DAMAGE);
    CalculateSpellDamageTaken(&damageInfo, damage, spellInfo);
    SendSpellNonMeleeDamageLog(&damageInfo);
    DealSpellDamage(&damageInfo, true);
    return damageInfo.damage;
}

void Unit::CalculateSpellDamageTaken(SpellNonMeleeDamage *damageInfo, int32 damage, SpellEntry const *spellInfo, WeaponAttackType attackType, bool crit)
{
    if (damage < 0)
        return;

    Unit *pVictim = damageInfo->target;
    if(!pVictim || !pVictim->IsAlive())
        return;

    SpellSchoolMask damageSchoolMask = SpellSchoolMask(damageInfo->schoolMask);
    uint32 crTypeMask = pVictim->GetCreatureTypeMask();
    // Check spell crit chance
    //bool crit = isSpellCrit(pVictim, spellInfo, damageSchoolMask, attackType);
    bool blocked = false;
    // Per-school calc
    switch (spellInfo->DmgClass)
    {
        // Melee and Ranged Spells
        case SPELL_DAMAGE_CLASS_RANGED:
        case SPELL_DAMAGE_CLASS_MELEE:
        {
            // Physical Damage
            if ( damageSchoolMask & SPELL_SCHOOL_MASK_NORMAL )
            {
                // Get blocked status
                blocked = isSpellBlocked(pVictim, spellInfo, attackType);
            }

            if (crit)
            {
                damageInfo->HitInfo|= SPELL_HIT_TYPE_CRIT;

                // Calculate crit bonus
                uint32 crit_bonus = damage;
                // Apply crit_damage bonus for melee spells
                if(Player* modOwner = GetSpellModOwner())
                    modOwner->ApplySpellMod(spellInfo->Id, SPELLMOD_CRIT_DAMAGE_BONUS, crit_bonus);
                damage += crit_bonus;

                // Apply SPELL_AURA_MOD_ATTACKER_RANGED_CRIT_DAMAGE or SPELL_AURA_MOD_ATTACKER_MELEE_CRIT_DAMAGE
                int32 critPctDamageMod=0;
                if(attackType == RANGED_ATTACK)
                    critPctDamageMod += pVictim->GetTotalAuraModifier(SPELL_AURA_MOD_ATTACKER_RANGED_CRIT_DAMAGE);
                else
                {
                    critPctDamageMod += pVictim->GetTotalAuraModifier(SPELL_AURA_MOD_ATTACKER_MELEE_CRIT_DAMAGE);
                    critPctDamageMod += GetTotalAuraModifier(SPELL_AURA_MOD_CRIT_DAMAGE_BONUS_MELEE);
                }
                // Increase crit damage from SPELL_AURA_MOD_CRIT_PERCENT_VERSUS
                critPctDamageMod += GetTotalAuraModifierByMiscMask(SPELL_AURA_MOD_CRIT_PERCENT_VERSUS, crTypeMask);

                if (critPctDamageMod!=0)
                    damage = int32((damage) * float((100.0f + critPctDamageMod)/100.0f));

                // Resilience - reduce crit damage
                if (pVictim->GetTypeId()==TYPEID_PLAYER)
                    damage -= (pVictim->ToPlayer())->GetMeleeCritDamageReduction(damage);
            }
            // Spell weapon based damage CAN BE crit & blocked at same time
            if (blocked)
            {
                damageInfo->blocked = uint32(pVictim->GetShieldBlockValue());
                if (damage < damageInfo->blocked)
                    damageInfo->blocked = damage;
                damage-=damageInfo->blocked;
            }
        }
        break;
        // Magical Attacks
        case SPELL_DAMAGE_CLASS_NONE:
        case SPELL_DAMAGE_CLASS_MAGIC:
        {
            // If crit add critical bonus
            if (crit)
            {
                damageInfo->HitInfo|= SPELL_HIT_TYPE_CRIT;
                damage = SpellCriticalBonus(spellInfo, damage, pVictim);
                // Resilience - reduce crit damage
                if (pVictim->GetTypeId()==TYPEID_PLAYER && !(spellInfo->AttributesEx4 & SPELL_ATTR_EX4_IGNORE_RESISTANCES))
                    damage -= (pVictim->ToPlayer())->GetSpellCritDamageReduction(damage);
            }
        }
        break;
    }


    if( damageSchoolMask & SPELL_SCHOOL_MASK_NORMAL  && (spellmgr.GetSpellCustomAttr(spellInfo->Id) & SPELL_ATTR_CU_IGNORE_ARMOR) == 0)
        damage = CalcArmorReducedDamage(pVictim, damage);

    // Calculate absorb resist
    if(damage > 0)
    {
        CalcAbsorbResist(pVictim, damageSchoolMask, SPELL_DIRECT_DAMAGE, damage, &damageInfo->absorb, &damageInfo->resist, (spellInfo ? spellInfo->Id : 0));
        damage-= damageInfo->absorb + damageInfo->resist;
    }
    else
        damage = 0;
        
    if (spellInfo && spellInfo->Id == 46576) {
        if (Aura* aur = pVictim->GetAura(46458, 0))
            damage = 300 * aur->GetStackAmount();
    }    
    
    damageInfo->damage = damage;
}

void Unit::DealSpellDamage(SpellNonMeleeDamage *damageInfo, bool durabilityLoss)
{
    if (damageInfo==0)
        return;

    Unit *pVictim = damageInfo->target;

    if(!this || !pVictim)
        return;

    if (!pVictim->IsAlive() || pVictim->isInFlight() || pVictim->GetTypeId() == TYPEID_UNIT && (pVictim->ToCreature())->IsInEvadeMode())
        return;

    SpellEntry const *spellProto = spellmgr.LookupSpell(damageInfo->SpellID);
    if (spellProto == NULL)
    {
        sLog.outError("Unit::DealSpellDamage have wrong damageInfo->SpellID: %u", damageInfo->SpellID);
        return;
    }

    //You don't lose health from damage taken from another player while in a sanctuary
    //You still see it in the combat log though
    if(pVictim != this && GetTypeId() == TYPEID_PLAYER && pVictim->GetTypeId() == TYPEID_PLAYER)
    {
        const AreaTableEntry *area = GetAreaEntryByAreaID(pVictim->GetAreaId());
        if(area && area->flags & 0x800)                     //sanctuary
            return;
    }

    // update at damage Judgement aura duration that applied by attacker at victim
    if(damageInfo->damage && spellProto->Id == 35395)
    {
        AuraMap& vAuras = pVictim->GetAuras();
        for(AuraMap::iterator itr = vAuras.begin(); itr != vAuras.end(); ++itr)
        {
            SpellEntry const *spellInfo = (*itr).second->GetSpellProto();
            if(spellInfo->SpellFamilyName == SPELLFAMILY_PALADIN && spellInfo->AttributesEx3 & SPELL_ATTR_EX3_CANT_MISS)
            {
                (*itr).second->SetAuraDuration((*itr).second->GetAuraMaxDuration());
                (*itr).second->UpdateAuraDuration();
            }
        }
    }
    // Call default DealDamage
    CleanDamage cleanDamage(damageInfo->cleanDamage, BASE_ATTACK, MELEE_HIT_NORMAL);
    DealDamage(pVictim, damageInfo->damage, &cleanDamage, SPELL_DIRECT_DAMAGE, SpellSchoolMask(damageInfo->schoolMask), spellProto, durabilityLoss);
}

//TODO for melee need create structure as in
void Unit::CalculateMeleeDamage(Unit *pVictim, uint32 damage, CalcDamageInfo *damageInfo, WeaponAttackType attackType)
{
    damageInfo->attacker         = this;
    damageInfo->target           = pVictim;
    damageInfo->damageSchoolMask = GetMeleeDamageSchoolMask();
    damageInfo->attackType       = attackType;
    damageInfo->damage           = 0;
    damageInfo->cleanDamage      = 0;
    damageInfo->absorb           = 0;
    damageInfo->resist           = 0;
    damageInfo->blocked_amount   = 0;

    damageInfo->TargetState      = 0;
    damageInfo->HitInfo          = 0;
    damageInfo->procAttacker     = PROC_FLAG_NONE;
    damageInfo->procVictim       = PROC_FLAG_NONE;
    damageInfo->procEx           = PROC_EX_NONE;
    damageInfo->hitOutCome       = MELEE_HIT_EVADE;

    if(!this || !pVictim)
        return;
    if(!this->IsAlive() || !pVictim->IsAlive())
        return;

    // Select HitInfo/procAttacker/procVictim flag based on attack type
    switch (attackType)
    {
        case BASE_ATTACK:
            damageInfo->procAttacker = PROC_FLAG_SUCCESSFUL_MELEE_HIT;
            damageInfo->procVictim   = PROC_FLAG_TAKEN_MELEE_HIT;
            damageInfo->HitInfo      = HITINFO_NORMALSWING2;
            break;
        case OFF_ATTACK:
            damageInfo->procAttacker = PROC_FLAG_SUCCESSFUL_MELEE_HIT | PROC_FLAG_SUCCESSFUL_OFFHAND_HIT;
            damageInfo->procVictim   = PROC_FLAG_TAKEN_MELEE_HIT;//|PROC_FLAG_TAKEN_OFFHAND_HIT // not used
            damageInfo->HitInfo = HITINFO_LEFTSWING;
            break;
        case RANGED_ATTACK:
            damageInfo->procAttacker = PROC_FLAG_SUCCESSFUL_RANGED_HIT;
            damageInfo->procVictim   = PROC_FLAG_TAKEN_RANGED_HIT;
            damageInfo->HitInfo = 0x08;// test
            break;
        default:
            break;
    }

    // Physical Immune check
    if(damageInfo->target->IsImmunedToDamage(SpellSchoolMask(damageInfo->damageSchoolMask),true))
    {
       damageInfo->HitInfo       |= HITINFO_NORMALSWING;
       damageInfo->TargetState    = VICTIMSTATE_IS_IMMUNE;

       damageInfo->procEx |=PROC_EX_IMMUNE;
       damageInfo->damage         = 0;
       damageInfo->cleanDamage    = 0;
       return;
    }
    damage += CalculateDamage(damageInfo->attackType, false, NULL, damageInfo->target);
    // Add melee damage bonus
    MeleeDamageBonus(damageInfo->target, &damage, damageInfo->attackType);
    // Calculate armor reduction
    damageInfo->damage = (damageInfo->damageSchoolMask & SPELL_SCHOOL_MASK_NORMAL) ? CalcArmorReducedDamage(damageInfo->target, damage) : damage;
    damageInfo->cleanDamage += damage - damageInfo->damage;

    damageInfo->hitOutCome = RollMeleeOutcomeAgainst(damageInfo->target, damageInfo->attackType, (SpellSchoolMask)damageInfo->damageSchoolMask);

    // Disable parry or dodge for ranged attack
    if(damageInfo->attackType == RANGED_ATTACK)
    {
        if (damageInfo->hitOutCome == MELEE_HIT_PARRY) damageInfo->hitOutCome = MELEE_HIT_NORMAL;
        if (damageInfo->hitOutCome == MELEE_HIT_DODGE) damageInfo->hitOutCome = MELEE_HIT_MISS;
    }

    switch(damageInfo->hitOutCome)
    {
        case MELEE_HIT_EVADE:
        {
            damageInfo->HitInfo    |= HITINFO_MISS|HITINFO_SWINGNOHITSOUND;
            damageInfo->TargetState = VICTIMSTATE_EVADES;

            damageInfo->procEx|=PROC_EX_EVADE;
            damageInfo->damage = 0;
            damageInfo->cleanDamage = 0;
            return;
        }
        case MELEE_HIT_MISS:
        {
            damageInfo->HitInfo    |= HITINFO_MISS;
            damageInfo->TargetState = VICTIMSTATE_NORMAL;

            damageInfo->procEx|=PROC_EX_MISS;
            damageInfo->damage = 0;
            damageInfo->cleanDamage = 0;
            break;
        }
        case MELEE_HIT_NORMAL:
            damageInfo->TargetState = VICTIMSTATE_NORMAL;
            damageInfo->procEx|=PROC_EX_NORMAL_HIT;
            break;
        case MELEE_HIT_CRIT:
        {
            damageInfo->HitInfo     |= HITINFO_CRITICALHIT;
            damageInfo->TargetState  = VICTIMSTATE_NORMAL;

            damageInfo->procEx|=PROC_EX_CRITICAL_HIT;
            // Crit bonus calc
            damageInfo->damage += damageInfo->damage;
            int32 mod=0;
            // Apply SPELL_AURA_MOD_ATTACKER_RANGED_CRIT_DAMAGE or SPELL_AURA_MOD_ATTACKER_MELEE_CRIT_DAMAGE
            if(damageInfo->attackType == RANGED_ATTACK)
                mod += damageInfo->target->GetTotalAuraModifier(SPELL_AURA_MOD_ATTACKER_RANGED_CRIT_DAMAGE);
            else
            {
                mod += damageInfo->target->GetTotalAuraModifier(SPELL_AURA_MOD_ATTACKER_MELEE_CRIT_DAMAGE);
                mod += GetTotalAuraModifier(SPELL_AURA_MOD_CRIT_DAMAGE_BONUS_MELEE);
            }

            uint32 crTypeMask = damageInfo->target->GetCreatureTypeMask();

            // Increase crit damage from SPELL_AURA_MOD_CRIT_PERCENT_VERSUS
            mod += GetTotalAuraModifierByMiscMask(SPELL_AURA_MOD_CRIT_PERCENT_VERSUS, crTypeMask);
            if (mod!=0)
                damageInfo->damage = int32((damageInfo->damage) * float((100.0f + mod)/100.0f));

            // Resilience - reduce crit damage
            if (pVictim->GetTypeId()==TYPEID_PLAYER)
            {
                uint32 resilienceReduction = (pVictim->ToPlayer())->GetMeleeCritDamageReduction(damageInfo->damage);
                damageInfo->damage      -= resilienceReduction;
                damageInfo->cleanDamage += resilienceReduction;
            }
            break;
        }
        case MELEE_HIT_PARRY:
            damageInfo->TargetState  = VICTIMSTATE_PARRY;
            damageInfo->procEx|=PROC_EX_PARRY;
            damageInfo->cleanDamage += damageInfo->damage;
            damageInfo->damage = 0;
            break;

        case MELEE_HIT_DODGE:
            damageInfo->TargetState  = VICTIMSTATE_DODGE;
            damageInfo->procEx|=PROC_EX_DODGE;
            damageInfo->cleanDamage += damageInfo->damage;
            damageInfo->damage = 0;
            break;
        case MELEE_HIT_BLOCK:
        {
            damageInfo->TargetState = VICTIMSTATE_NORMAL;
            damageInfo->procEx|=PROC_EX_BLOCK;
            damageInfo->blocked_amount = damageInfo->target->GetShieldBlockValue();
            if (damageInfo->blocked_amount >= damageInfo->damage)
            {
                damageInfo->TargetState = VICTIMSTATE_BLOCKS;
                damageInfo->blocked_amount = damageInfo->damage;
            }
            damageInfo->damage      -= damageInfo->blocked_amount;
            damageInfo->cleanDamage += damageInfo->blocked_amount;
            break;
        }
        case MELEE_HIT_GLANCING:
        {
            damageInfo->HitInfo     |= HITINFO_GLANCING;
            damageInfo->TargetState  = VICTIMSTATE_NORMAL;
            damageInfo->procEx|=PROC_EX_NORMAL_HIT;
            int32 leveldif = int32(pVictim->getLevel()) - int32(getLevel());
            if (leveldif > 3) leveldif = 3;
            float reducePercent = 1 - leveldif * 0.1f;
            damageInfo->cleanDamage += damageInfo->damage-uint32(reducePercent *  damageInfo->damage);
            damageInfo->damage   = uint32(reducePercent *  damageInfo->damage);
            break;
        }
        case MELEE_HIT_CRUSHING:
        {
            damageInfo->HitInfo     |= HITINFO_CRUSHING;
            damageInfo->TargetState  = VICTIMSTATE_NORMAL;
            damageInfo->procEx|=PROC_EX_NORMAL_HIT;
            // 150% normal damage
            damageInfo->damage += (damageInfo->damage / 2);
            break;
        }
        default:

            break;
    }

    // Calculate absorb resist
    if(int32(damageInfo->damage) > 0)
    {
        damageInfo->procVictim |= PROC_FLAG_TAKEN_ANY_DAMAGE;
        // Calculate absorb & resists
        CalcAbsorbResist(damageInfo->target, SpellSchoolMask(damageInfo->damageSchoolMask), DIRECT_DAMAGE, damageInfo->damage, &damageInfo->absorb, &damageInfo->resist, 0);
        damageInfo->damage-=damageInfo->absorb + damageInfo->resist;
        if (damageInfo->absorb)
        {
            damageInfo->HitInfo|=HITINFO_ABSORB;
            damageInfo->procEx|=PROC_EX_ABSORB;
            damageInfo->procVictim |= PROC_FLAG_HAD_DAMAGE_BUT_ABSORBED;
        }
        if (damageInfo->resist)
            damageInfo->HitInfo|=HITINFO_RESIST;

    }
    else // Umpossible get negative result but....
        damageInfo->damage = 0;
}

void Unit::DealMeleeDamage(CalcDamageInfo *damageInfo, bool durabilityLoss)
{
    if (damageInfo==0) return;
    Unit *pVictim = damageInfo->target;

    if(!this || !pVictim)
        return;

    if (!pVictim->IsAlive() || pVictim->isInFlight() || pVictim->GetTypeId() == TYPEID_UNIT && (pVictim->ToCreature())->IsInEvadeMode())
        return;

    //You don't lose health from damage taken from another player while in a sanctuary
    //You still see it in the combat log though
    if(pVictim != this && GetTypeId() == TYPEID_PLAYER && pVictim->GetTypeId() == TYPEID_PLAYER)
    {
        const AreaTableEntry *area = GetAreaEntryByAreaID(pVictim->GetAreaId());
        if(area && area->flags & 0x800)                     //sanctuary
            return;
    }
    /*
    // Hmmmm dont like this emotes cloent must by self do all animations
    if (damageInfo->HitInfo&HITINFO_CRITICALHIT)
        pVictim->HandleEmoteCommand(EMOTE_ONESHOT_WOUNDCRITICAL);
    if(damageInfo->blocked_amount && damageInfo->TargetState!=VICTIMSTATE_BLOCKS)
        pVictim->HandleEmoteCommand(EMOTE_ONESHOT_PARRYSHIELD);
 
    if(damageInfo->TargetState == VICTIMSTATE_PARRY) // Parry rush
    {
        // Get attack timers
        float offtime  = float(pVictim->getAttackTimer(OFF_ATTACK));
        float basetime = float(pVictim->getAttackTimer(BASE_ATTACK));
        // Reduce attack time
        if (pVictim->haveOffhandWeapon() && offtime < basetime)
        {
            float percent20 = pVictim->GetAttackTime(OFF_ATTACK) * 0.20;
            float percent60 = 3 * percent20;
            if(offtime > percent20 && offtime <= percent60)
            {
                pVictim->setAttackTimer(OFF_ATTACK, uint32(percent20));
            }
            else if(offtime > percent60)
            {
                offtime -= 2 * percent20;
                pVictim->setAttackTimer(OFF_ATTACK, uint32(offtime));
            }
        }
        else
        {
            float percent20 = pVictim->GetAttackTime(BASE_ATTACK) * 0.20;
            float percent60 = 3 * percent20;
            if(basetime > percent20 && basetime <= percent60)
            {
                pVictim->setAttackTimer(BASE_ATTACK, uint32(percent20));
            }
            else if(basetime > percent60)
            {
                basetime -= 2 * percent20;
                pVictim->setAttackTimer(BASE_ATTACK, uint32(basetime));
            }
        }
    }
    */
    // Call default DealDamage
    CleanDamage cleanDamage(damageInfo->cleanDamage,damageInfo->attackType,damageInfo->hitOutCome);
    DealDamage(pVictim, damageInfo->damage, &cleanDamage, DIRECT_DAMAGE, SpellSchoolMask(damageInfo->damageSchoolMask), NULL, durabilityLoss);

    // If this is a creature and it attacks from behind it has a probability to daze it's victim
    if( (damageInfo->hitOutCome==MELEE_HIT_CRIT || damageInfo->hitOutCome==MELEE_HIT_CRUSHING || damageInfo->hitOutCome==MELEE_HIT_NORMAL || damageInfo->hitOutCome==MELEE_HIT_GLANCING) &&
        GetTypeId() != TYPEID_PLAYER && !(this->ToCreature())->GetCharmerOrOwnerGUID() && !pVictim->HasInArc(M_PI, this)
        && (pVictim->GetTypeId() == TYPEID_PLAYER || !(pVictim->ToCreature())->isWorldBoss()))
    {
        // -probability is between 0% and 40%
        // 20% base chance
        float Probability = 20;

        //there is a newbie protection, at level 10 just 7% base chance; assuming linear function
        if( pVictim->getLevel() < 30 )
            Probability = 0.65f*pVictim->getLevel()+0.5;

        uint32 VictimDefense=pVictim->GetDefenseSkillValue();
        uint32 AttackerMeleeSkill=GetUnitMeleeSkill();

        Probability *= AttackerMeleeSkill/(float)VictimDefense;

        if(Probability > 40)
            Probability = 40;

        if(roll_chance_f(Probability))
            CastSpell(pVictim, 1604, true);
    }

    // update at damage Judgement aura duration that applied by attacker at victim
    if(damageInfo->damage)
    {
        AuraMap& vAuras = pVictim->GetAuras();
        for(AuraMap::iterator itr = vAuras.begin(); itr != vAuras.end(); ++itr)
        {
            SpellEntry const *spellInfo = (*itr).second->GetSpellProto();
            if( spellInfo->AttributesEx3 & 0x40000 && spellInfo->SpellFamilyName == SPELLFAMILY_PALADIN && ((*itr).second->GetCasterGUID() == GetGUID()) && spellInfo->Id != 41461) //Gathios judgement of blood (can't seem to find a general rule to avoid this hack)
            {
                (*itr).second->SetAuraDuration((*itr).second->GetAuraMaxDuration());
                (*itr).second->UpdateAuraDuration();
            }
        }
    }

    if(GetTypeId() == TYPEID_PLAYER)
        (this->ToPlayer())->CastItemCombatSpell(pVictim, damageInfo->attackType, damageInfo->procVictim, damageInfo->procEx);

    // Do effect if any damage done to target
    if (damageInfo->procVictim & PROC_FLAG_TAKEN_ANY_DAMAGE)
    {
        // victim's damage shield
        std::set<Aura*> alreadyDone;
        uint32 removedAuras = pVictim->m_removedAurasCount;
        AuraList const& vDamageShields = pVictim->GetAurasByType(SPELL_AURA_DAMAGE_SHIELD);
        for(AuraList::const_iterator i = vDamageShields.begin(), next = vDamageShields.begin(); i != vDamageShields.end(); i = next)
        {
           next++;
           if (alreadyDone.find(*i) == alreadyDone.end())
           {
               alreadyDone.insert(*i);
               uint32 damage=(*i)->GetModifier()->m_amount;
               SpellEntry const *spellProto = spellmgr.LookupSpell((*i)->GetId());
               if(!spellProto)
                   continue;
               //Calculate absorb resist ??? no data in opcode for this possibly unable to absorb or resist?
               //uint32 absorb;
               //uint32 resist;
               //CalcAbsorbResist(pVictim, SpellSchools(spellProto->School), SPELL_DIRECT_DAMAGE, damage, &absorb, &resist);
               //damage-=absorb + resist;

               WorldPacket data(SMSG_SPELLDAMAGESHIELD,(8+8+4+4));
               data << uint64(pVictim->GetGUID());
               data << uint64(GetGUID());
               data << uint32(spellProto->SchoolMask);
               data << uint32(damage);
               pVictim->SendMessageToSet(&data, true );

               pVictim->DealDamage(this, damage, 0, SPELL_DIRECT_DAMAGE, GetSpellSchoolMask(spellProto), spellProto, true);

               if (pVictim->m_removedAurasCount > removedAuras)
               {
                   removedAuras = pVictim->m_removedAurasCount;
                   next = vDamageShields.begin();
               }
           }
        }
    }
}


void Unit::HandleEmoteCommand(uint32 emote_id)
{
    WorldPacket data( SMSG_EMOTE, 12 );
    data << emote_id << GetGUID();
    WPAssert(data.size() == 12);

    SendMessageToSet(&data, true);
}

uint32 Unit::CalcArmorReducedDamage(Unit* pVictim, const uint32 damage)
{
    uint32 newdamage = 0;
    float armor = pVictim->GetArmor();
    // Ignore enemy armor by SPELL_AURA_MOD_TARGET_RESISTANCE aura
    armor += GetTotalAuraModifierByMiscMask(SPELL_AURA_MOD_TARGET_RESISTANCE, SPELL_SCHOOL_MASK_NORMAL);

    if (armor<0.0f) armor=0.0f;

    float tmpvalue = 0.0f;
    if(getLevel() <= 59)                                    //Level 1-59
        tmpvalue = armor / (armor + 400.0f + 85.0f * getLevel());
    else if(getLevel() < 70)                                //Level 60-69
        tmpvalue = armor / (armor - 22167.5f + 467.5f * getLevel());
    else                                                    //Level 70+
        tmpvalue = armor / (armor + 10557.5f);

    if(tmpvalue < 0.0f)
        tmpvalue = 0.0f;
    if(tmpvalue > 0.75f)
        tmpvalue = 0.75f;
    newdamage = uint32(damage - (damage * tmpvalue));

    return (newdamage > 1) ? newdamage : 1;
}

void Unit::CalcAbsorbResist(Unit *pVictim,SpellSchoolMask schoolMask, DamageEffectType damagetype, const uint32 damage, uint32 *absorb, uint32 *resist, uint32 spellId)
{
    if(!pVictim || !pVictim->IsAlive() || !damage)
        return;

    SpellEntry const* spellProto = spellmgr.LookupSpell(spellId);

    // Magic damage, check for resists
    if(  (schoolMask & SPELL_SCHOOL_MASK_SPELL)                                          // Is magic and not holy
         && (  !spellProto 
               || !spellmgr.IsBinaryMagicResistanceSpell(spellProto) 
               || !(spellProto->AttributesEx4 & SPELL_ATTR_EX4_IGNORE_RESISTANCES) 
               || !(spellProto->AttributesEx3 & SPELL_ATTR_EX3_CANT_MISS) ) // Non binary spell (this was already handled in DoSpellHitOnUnit) (see Spell::IsBinaryMagicResistanceSpell for more)
      )              
    {
        // Get base victim resistance for school
        int32 resistance = (float)pVictim->GetResistance(GetFirstSchoolInMask(schoolMask));
        // Ignore resistance by self SPELL_AURA_MOD_TARGET_RESISTANCE aura (aka spell penetration)
        resistance += (float)GetTotalAuraModifierByMiscMask(SPELL_AURA_MOD_TARGET_RESISTANCE, schoolMask);
        // Resistance can't be negative
        
        if(resistance < 0) 
            resistance = 0;

        float fResistance = (float)resistance * (float)(0.15f / getLevel()); //% from 0.0 to 1.0
     
        //can't seem to find the proper rule for this... meanwhile let's have use an approximation
        int32 levelDiff = pVictim->getLevel() - getLevel();
        if(levelDiff > 0)
            fResistance += (int32) ((levelDiff<3?levelDiff:3) * (0.006f)); //Cap it a 3 level diff, probably not blizz but this doesn't change anything at HL and is A LOT less boring for people pexing

        // Resistance can't be more than 75%
        if (fResistance > 0.75f)
            fResistance = 0.75f;

        uint32 ran = GetMap()->urand(0, 100);
        uint32 faq[4] = {24,6,4,6};
        uint8 m = 0;
        float Binom = 0.0f;
        for (uint8 i = 0; i < 4; i++)
        {
            Binom += 2400 *( powf(fResistance, i) * powf( (1-fResistance), (4-i)))/faq[i];
            if (ran > Binom )
                ++m;
            else
                break;
        }
        if (damagetype == DOT && m == 4)
            *resist += uint32(damage - 1);
        else
            *resist += uint32(damage * m / 4);
        if(*resist > damage)
            *resist = damage;
    }
    else
        *resist = 0;

    int32 RemainingDamage = damage - *resist;

    AuraList const& vOverrideScripts = pVictim->GetAurasByType(SPELL_AURA_OVERRIDE_CLASS_SCRIPTS);
    for(AuraList::const_iterator i = vOverrideScripts.begin(), next; i != vOverrideScripts.end(); i = next)
    {
        next = i; ++next;

        if (pVictim->GetTypeId() != TYPEID_PLAYER)
            break;

        // Shadow of Death - set cheat death on cooldown
        if ((*i)->GetSpellProto()->Id == 40251 && pVictim->GetHealth() <= RemainingDamage)
        {
            (pVictim->ToPlayer())->AddSpellCooldown(31231,0,time(NULL)+60);
            break;
        }
    }

    // Need to remove expired auras after
    bool expiredExists = false;

    // absorb without mana cost
    int32 reflectDamage = 0;
    Aura* reflectAura = NULL;
    AuraList const& vSchoolAbsorb = pVictim->GetAurasByType(SPELL_AURA_SCHOOL_ABSORB);
    for(AuraList::const_iterator i = vSchoolAbsorb.begin(); i != vSchoolAbsorb.end() && RemainingDamage > 0; ++i)
    {
        int32 *p_absorbAmount = &(*i)->GetModifier()->m_amount;

        // should not happen....
        if (*p_absorbAmount <=0)
        {
            expiredExists = true;
            continue;
        }

        if (((*i)->GetModifier()->m_miscvalue & schoolMask)==0)
            continue;

        // Cheat Death
        if((*i)->GetSpellProto()->SpellFamilyName==SPELLFAMILY_ROGUE && (*i)->GetSpellProto()->SpellIconID == 2109)
        {
            if ((pVictim->ToPlayer())->HasSpellCooldown(31231))
                continue;
            if (pVictim->GetHealth() <= RemainingDamage)
            {
                int32 chance = *p_absorbAmount;
                if (roll_chance_i(chance))
                {
                    pVictim->CastSpell(pVictim,31231,true);
                    (pVictim->ToPlayer())->AddSpellCooldown(31231,0,time(NULL)+60);

                    // with health > 10% lost health until health==10%, in other case no losses
                    uint32 health10 = pVictim->GetMaxHealth()/10;
                    RemainingDamage = pVictim->GetHealth() > health10 ? pVictim->GetHealth() - health10 : 0;
                }
            }
            continue;
        }
        
        // Shadow of Death
        if ((*i)->GetSpellProto()->Id == 40251)
        {
            if (pVictim->GetHealth() <= RemainingDamage)
            {
                RemainingDamage = 0;
                // Will be cleared next update
                (*i)->SetAuraDuration(0);
            }
            continue;
        }

        int32 currentAbsorb;

        //Reflective Shield
        if ((pVictim != this))
        {
            if(Unit* caster = (*i)->GetCaster())
            {
                if ((*i)->GetSpellProto()->SpellFamilyName == SPELLFAMILY_PRIEST && (*i)->GetSpellProto()->SpellFamilyFlags == 0x1)
                {
                    AuraList const& vOverRideCS = caster->GetAurasByType(SPELL_AURA_OVERRIDE_CLASS_SCRIPTS);
                    for(AuraList::const_iterator k = vOverRideCS.begin(); k != vOverRideCS.end(); ++k)
                    {
                        switch((*k)->GetModifier()->m_miscvalue)
                        {
                            case 5065:                          // Rank 1
                            case 5064:                          // Rank 2
                            case 5063:                          // Rank 3
                            case 5062:                          // Rank 4
                            case 5061:                          // Rank 5
                            {
                                if(RemainingDamage >= *p_absorbAmount)
                                     reflectDamage = *p_absorbAmount * (*k)->GetModifier()->m_amount/100;
                                else
                                    reflectDamage = (*k)->GetModifier()->m_amount * RemainingDamage/100;
                                reflectAura = *i;

                            } break;
                            default: break;
                        }

                        if(reflectDamage)
                            break;
                    }
                }
                // Reflective Shield, NPC
                else if ((*i)->GetSpellProto()->Id == 41475)
                {
                    if(RemainingDamage >= *p_absorbAmount)
                        reflectDamage = *p_absorbAmount * 0.5f;
                    else
                        reflectDamage = RemainingDamage * 0.5f;
                    reflectAura = *i;
                }
            }
        }

        if (RemainingDamage >= *p_absorbAmount)
        {
            currentAbsorb = *p_absorbAmount;
            expiredExists = true;
        }
        else
            currentAbsorb = RemainingDamage;

        *p_absorbAmount -= currentAbsorb;
        RemainingDamage -= currentAbsorb;
    }
    // do not cast spells while looping auras; auras can get invalid otherwise
    if (reflectDamage)
        pVictim->CastCustomSpell(this, 33619, &reflectDamage, NULL, NULL, true, NULL, reflectAura);

    // Remove all expired absorb auras
    if (expiredExists)
    {
        for (AuraList::const_iterator i = vSchoolAbsorb.begin(); i != vSchoolAbsorb.end(); )
        {
            Aura *aur = (*i);
            ++i;
            if (aur->GetModifier()->m_amount <= 0)
            {
                uint32 removedAuras = pVictim->m_removedAurasCount;
                pVictim->RemoveAurasDueToSpell( aur->GetId() );
                if (removedAuras + 1 < pVictim->m_removedAurasCount)
                    i = vSchoolAbsorb.begin();
            }
        }
    }

    // absorb by mana cost
    AuraList const& vManaShield = pVictim->GetAurasByType(SPELL_AURA_MANA_SHIELD);
    for(AuraList::const_iterator i = vManaShield.begin(), next; i != vManaShield.end() && RemainingDamage > 0; i = next)
    {
        next = i; ++next;
        int32 *p_absorbAmount = &(*i)->GetModifier()->m_amount;

        // check damage school mask
        if(((*i)->GetModifier()->m_miscvalue & schoolMask)==0)
            continue;

        int32 currentAbsorb;
        if (RemainingDamage >= *p_absorbAmount)
            currentAbsorb = *p_absorbAmount;
        else
            currentAbsorb = RemainingDamage;

        float manaMultiplier = (*i)->GetSpellProto()->EffectMultipleValue[(*i)->GetEffIndex()];
        if(Player *modOwner = GetSpellModOwner())
            modOwner->ApplySpellMod((*i)->GetId(), SPELLMOD_MULTIPLE_VALUE, manaMultiplier);

        if(manaMultiplier)
        {
            int32 maxAbsorb = int32(pVictim->GetPower(POWER_MANA) / manaMultiplier);
            if (currentAbsorb > maxAbsorb)
                currentAbsorb = maxAbsorb;
        }

        *p_absorbAmount -= currentAbsorb;
        if(*p_absorbAmount <= 0)
        {
            pVictim->RemoveAurasDueToSpell((*i)->GetId());
            next = vManaShield.begin();
        }

        int32 manaReduction = int32(currentAbsorb * manaMultiplier);
        pVictim->ApplyPowerMod(POWER_MANA, manaReduction, false);

        RemainingDamage -= currentAbsorb;
    }

    // only split damage if not damaging yourself
    if(pVictim != this)
    {
        AuraList const& vSplitDamageFlat = pVictim->GetAurasByType(SPELL_AURA_SPLIT_DAMAGE_FLAT);
        for(AuraList::const_iterator i = vSplitDamageFlat.begin(), next; i != vSplitDamageFlat.end() && RemainingDamage >= 0; i = next)
        {
            next = i; ++next;

            // check damage school mask
            if(((*i)->GetModifier()->m_miscvalue & schoolMask)==0)
                continue;

            // Damage can be splitted only if aura has an alive caster
            Unit *caster = (*i)->GetCaster();
            if(!caster || caster == pVictim || !caster->IsInWorld() || !caster->IsAlive())
                continue;

            int32 currentAbsorb;
            if (RemainingDamage >= (*i)->GetModifier()->m_amount)
                currentAbsorb = (*i)->GetModifier()->m_amount;
            else
                currentAbsorb = RemainingDamage;

            RemainingDamage -= currentAbsorb;

            SendSpellNonMeleeDamageLog(caster, (*i)->GetSpellProto()->Id, currentAbsorb, schoolMask, 0, 0, false, 0, false);

            CleanDamage cleanDamage = CleanDamage(currentAbsorb, BASE_ATTACK, MELEE_HIT_NORMAL);
            DealDamage(caster, currentAbsorb, &cleanDamage, DOT, schoolMask, (*i)->GetSpellProto(), false);
        }



        AuraList const& vSplitDamagePct = pVictim->GetAurasByType(SPELL_AURA_SPLIT_DAMAGE_PCT);
        for(AuraList::const_iterator i = vSplitDamagePct.begin(), next; i != vSplitDamagePct.end() && RemainingDamage >= 0; i = next)
        {
            next = i; ++next;
            int32 *p_absorbAmount = &(*i)->GetModifier()->m_amount;

            // check damage school mask
            if(((*i)->GetModifier()->m_miscvalue & schoolMask)==0)
                continue;

            // Damage can be splitted only if aura has an alive caster
            Unit *caster = (*i)->GetCaster();
            if(!caster || caster == pVictim || !caster->IsInWorld() || !caster->IsAlive())
                continue;

            int32 splitted = int32(RemainingDamage * (*i)->GetModifier()->m_amount / 100.0f);

            RemainingDamage -= splitted;

            SendSpellNonMeleeDamageLog(caster, (*i)->GetSpellProto()->Id, splitted, schoolMask, 0, 0, false, 0, false);

            CleanDamage cleanDamage = CleanDamage(splitted, BASE_ATTACK, MELEE_HIT_NORMAL);
            DealDamage(caster, splitted, &cleanDamage, DOT, schoolMask, (*i)->GetSpellProto(), false);
        }
    }

    *absorb = damage - RemainingDamage - *resist;
}

bool Unit::canMelee( bool extra )
{
    if(HasUnitState(UNIT_STAT_LOST_CONTROL))
        return false;

    if(HasUnitState(UNIT_STAT_CASTING) && !extra)
        return false;

    if (HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PACIFIED))
        return false;

    return true;
}

void Unit::AttackerStateUpdate (Unit *pVictim, WeaponAttackType attType, bool extra )
{
    if (ToPlayer() && ToPlayer()->isSpectator())
        return;

    if(!extra && HasUnitState(UNIT_STAT_CANNOT_AUTOATTACK) || HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PACIFIED) )
        return;

    if (!pVictim->IsAlive())
        return;

    if(attType == BASE_ATTACK && sWorld.getConfig(CONFIG_TESTSERVER_ENABLE) && sWorld.getConfig(CONFIG_TESTSERVER_DISABLE_MAINHAND))
        return;
        
    CombatStart(pVictim);
    RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_MELEE_ATTACK);
    
    if (pVictim->GetTypeId() == TYPEID_UNIT && (pVictim->ToCreature())->IsAIEnabled)
        (pVictim->ToCreature())->AI()->AttackedBy(this);

    uint32 hitInfo;
    if (attType == BASE_ATTACK)
        hitInfo = HITINFO_NORMALSWING2;
    else if (attType == OFF_ATTACK)
        hitInfo = HITINFO_LEFTSWING;
    else
        return;                                             // ignore ranged case

    // melee attack spell casted at main hand attack only
    if (!extra && attType == BASE_ATTACK && m_currentSpells[CURRENT_MELEE_SPELL])
    {
        m_currentSpells[CURRENT_MELEE_SPELL]->cast();
        return;
    }

    CalcDamageInfo damageInfo;
    CalculateMeleeDamage(pVictim, 0, &damageInfo, attType);
    // Send log damage message to client
    SendAttackStateUpdate(&damageInfo);
    DealMeleeDamage(&damageInfo,true);
    ProcDamageAndSpell(damageInfo.target, damageInfo.procAttacker, damageInfo.procVictim, damageInfo.procEx, damageInfo.damage, damageInfo.attackType);

    if (GetTypeId() == TYPEID_PLAYER)
        DEBUG_LOG("AttackerStateUpdate: (Player) %u attacked %u (TypeId: %u) for %u dmg, absorbed %u, blocked %u, resisted %u.",
            GetGUIDLow(), pVictim->GetGUIDLow(), pVictim->GetTypeId(), damageInfo.damage, damageInfo.absorb, damageInfo.blocked_amount, damageInfo.resist);
    else
        DEBUG_LOG("AttackerStateUpdate: (NPC)    %u attacked %u (TypeId: %u) for %u dmg, absorbed %u, blocked %u, resisted %u.",
            GetGUIDLow(), pVictim->GetGUIDLow(), pVictim->GetTypeId(), damageInfo.damage, damageInfo.absorb, damageInfo.blocked_amount, damageInfo.resist);

    // HACK: Warrior enrage not losing procCharges when dealing melee damage
    if (GetTypeId() == TYPEID_PLAYER) {
        uint32 enrageId = 0;
        if (HasAura(12880))
            enrageId = 12880;
        else if (HasAura(14201))
            enrageId = 14201;
        else if (HasAura(14202))
            enrageId = 14202;
        else if (HasAura(14203))
            enrageId = 14203;
        else if (HasAura(14204))
            enrageId = 14204;
            
        if (enrageId) {
            if (Aura* enrageAura = GetAuraByCasterSpell(enrageId, GetGUID())) {
                enrageAura->SetAuraProcCharges(enrageAura->GetAuraProcCharges()-1);
            }
        }
    }
}

MeleeHitOutcome Unit::RollMeleeOutcomeAgainst(const Unit *pVictim, WeaponAttackType attType, SpellSchoolMask schoolMask) const
{
    // This is only wrapper

    // Miss chance based on melee
    //float miss_chance = MeleeMissChanceCalc(pVictim, attType);
    float miss_chance = MeleeSpellMissChance(pVictim, attType, int32(GetWeaponSkillValue(attType,pVictim)) - int32(pVictim->GetDefenseSkillValue(this)), 0);

    // Critical hit chance
    float crit_chance = GetUnitCriticalChance(attType, pVictim);

    // stunned target cannot dodge and this is checked in GetUnitDodgeChance() (returned 0 in this case)
    float dodge_chance = pVictim->GetUnitDodgeChance();
    float block_chance = pVictim->GetUnitBlockChance();
    float parry_chance = pVictim->GetUnitParryChance(); 

    // Useful if want to specify crit & miss chances for melee, else it could be removed
    DEBUG_LOG ("MELEE OUTCOME: miss %f crit %f dodge %f parry %f block %f", miss_chance,crit_chance,dodge_chance,parry_chance,block_chance);

    return RollMeleeOutcomeAgainst(pVictim, attType, int32(crit_chance*100), int32(miss_chance*100), int32(dodge_chance*100),int32(parry_chance*100),int32(block_chance*100), false);
}

MeleeHitOutcome Unit::RollMeleeOutcomeAgainst (const Unit *pVictim, WeaponAttackType attType, int32 crit_chance, int32 miss_chance, int32 dodge_chance, int32 parry_chance, int32 block_chance, bool SpellCasted ) const
{
    if(pVictim->GetTypeId()==TYPEID_UNIT && (pVictim->ToCreature())->IsInEvadeMode())
        return MELEE_HIT_EVADE;

    int32 attackerMaxSkillValueForLevel = GetMaxSkillValueForLevel(pVictim);
    int32 victimMaxSkillValueForLevel = pVictim->GetMaxSkillValueForLevel(this);

    int32 attackerWeaponSkill = GetWeaponSkillValue(attType,pVictim);
    int32 victimDefenseSkill = pVictim->GetDefenseSkillValue(this);

    // bonus from skills is 0.04%
    int32    skillBonus  = 4 * ( attackerWeaponSkill - victimMaxSkillValueForLevel );
    int32    skillBonus2 = 4 * ( attackerMaxSkillValueForLevel - victimDefenseSkill );
    int32    sum = 0, tmp = 0;
    int32    roll = GetMap()->urand (0, 10000);

    DEBUG_LOG ("RollMeleeOutcomeAgainst: skill bonus of %d for attacker", skillBonus);
    DEBUG_LOG ("RollMeleeOutcomeAgainst: rolled %d, miss %d, dodge %d, parry %d, block %d, crit %d",
        roll, miss_chance, dodge_chance, parry_chance, block_chance, crit_chance);

    tmp = miss_chance;

    if (tmp > 0 && roll < (sum += tmp ))
    {
        DEBUG_LOG ("RollMeleeOutcomeAgainst: MISS");
        return MELEE_HIT_MISS;
    }

    // always crit against a sitting target (except 0 crit chance)
    if( pVictim->GetTypeId() == TYPEID_PLAYER && crit_chance > 0 && !pVictim->IsStandState() )
    {
        DEBUG_LOG ("RollMeleeOutcomeAgainst: CRIT (sitting victim)");
        return MELEE_HIT_CRIT;
    }

    // Dodge chance

    // only players can't dodge if attacker is behind
    if (pVictim->GetTypeId() == TYPEID_PLAYER && !pVictim->HasInArc(M_PI,this))
    {
        DEBUG_LOG ("RollMeleeOutcomeAgainst: attack came from behind and victim was a player.");
    }
    else
    {
        if(dodge_chance > 0) // check if unit _can_ dodge
        {
            int32 real_dodge_chance = dodge_chance;
            real_dodge_chance -= skillBonus;

            // Reduce dodge chance by attacker expertise rating
            if (GetTypeId() == TYPEID_PLAYER)
                real_dodge_chance -= int32((this->ToPlayer())->GetExpertiseDodgeOrParryReduction(attType)*100);
            // Modify dodge chance by attacker SPELL_AURA_MOD_COMBAT_RESULT_CHANCE
            real_dodge_chance+= GetTotalAuraModifierByMiscValue(SPELL_AURA_MOD_COMBAT_RESULT_CHANCE, VICTIMSTATE_DODGE)*100;
            real_dodge_chance+= GetTotalAuraModifier(SPELL_AURA_MOD_ENEMY_DODGE)*100;

            if (   (real_dodge_chance > 0)                                        
                && roll < (sum += real_dodge_chance))
            {
                DEBUG_LOG ("RollMeleeOutcomeAgainst: DODGE <%d, %d)", sum-real_dodge_chance, sum);
                return MELEE_HIT_DODGE;
            }
        }
    }

    // parry & block chances

    // check if attack comes from behind, nobody can parry or block if attacker is behind
    if (!pVictim->HasInArc(M_PI,this))
    {
        DEBUG_LOG ("RollMeleeOutcomeAgainst: attack came from behind.");
    }
    else
    {
        if(parry_chance > 0) // check if unit _can_ parry
        {
            int32 real_parry_chance = parry_chance;
            real_parry_chance -= skillBonus;

            // Reduce parry chance by attacker expertise rating
            if (GetTypeId() == TYPEID_PLAYER)
                real_parry_chance -= int32((this->ToPlayer())->GetExpertiseDodgeOrParryReduction(attType)*100);

            if (   (real_parry_chance > 0)     
                && (roll < (sum += real_parry_chance)))
            {
                DEBUG_LOG ("RollMeleeOutcomeAgainst: PARRY <%d, %d)", sum-real_parry_chance, sum);
                ((Unit*)pVictim)->HandleParryRush();
                return MELEE_HIT_PARRY;
            }
        }

        if(block_chance > 0)
        {
            int32 real_block_chance = block_chance;
            if(block_chance > 0) // check if unit _can_ block
                real_block_chance -= skillBonus;

            if (   (real_block_chance > 0)      
                && (roll < (sum += real_block_chance)))
            {
                // Critical chance
                int16 blocked_crit_chance = crit_chance + skillBonus2;
                if ( GetTypeId() == TYPEID_PLAYER && SpellCasted && blocked_crit_chance > 0 )
                {
                    if ( roll_chance_i(blocked_crit_chance/100))
                    {
                        DEBUG_LOG ("RollMeleeOutcomeAgainst: BLOCKED CRIT");
                        return MELEE_HIT_BLOCK_CRIT;
                    }
                }
                DEBUG_LOG ("RollMeleeOutcomeAgainst: BLOCK <%d, %d)", sum-blocked_crit_chance, sum);
                return MELEE_HIT_BLOCK;
            }
        }
    }

    // Critical chance
    int32 real_crit_chance = crit_chance + skillBonus2;

    if (real_crit_chance > 0 && roll < (sum += real_crit_chance))
    {
        DEBUG_LOG ("RollMeleeOutcomeAgainst: CRIT <%d, %d)", sum-real_crit_chance, sum);
        if(GetTypeId() == TYPEID_UNIT && ((this->ToCreature())->GetCreatureInfo()->flags_extra & CREATURE_FLAG_EXTRA_NO_CRIT))
            DEBUG_LOG ("RollMeleeOutcomeAgainst: CRIT DISABLED)");
        else
            return MELEE_HIT_CRIT;
    }

    if(!sWorld.getConfig(CONFIG_TESTSERVER_ENABLE) || !sWorld.getConfig(CONFIG_TESTSERVER_DISABLE_GLANCING))
    {
        // Max 40% chance to score a glancing blow against mobs that are higher level (can do only players and pets and not with ranged weapon)
        if( attType != RANGED_ATTACK && !SpellCasted &&
            (GetTypeId() == TYPEID_PLAYER || (this->ToCreature())->IsPet()) &&
            pVictim->GetTypeId() != TYPEID_PLAYER && !(pVictim->ToCreature())->IsPet() &&
            getLevel() < pVictim->getLevelForTarget(this) )
        {
            // cap possible value (with bonuses > max skill)
            int32 skill = attackerWeaponSkill;
            int32 maxskill = attackerMaxSkillValueForLevel;
            skill = (skill > maxskill) ? maxskill : skill;

            tmp = (10 + (victimDefenseSkill - skill)) * 100;
            tmp = tmp > 4000 ? 4000 : tmp;
            if (roll < (sum += tmp))
            {
                DEBUG_LOG ("RollMeleeOutcomeAgainst: GLANCING <%d, %d)", sum-4000, sum);
                return MELEE_HIT_GLANCING;
            }
        }
    }

    if(GetTypeId()!=TYPEID_PLAYER && !((this->ToCreature())->GetCreatureInfo()->flags_extra & CREATURE_FLAG_EXTRA_NO_CRUSH) && !(this->ToCreature())->IsPet() && !SpellCasted /*Only autoattack can be crushing blow*/ )
    {
        // mobs can score crushing blows if they're 3 or more levels above victim
        // or when their weapon skill is 15 or more above victim's defense skill
        tmp = victimDefenseSkill;
        int32 tmpmax = victimMaxSkillValueForLevel;
        // having defense above your maximum (from items, talents etc.) has no effect
        tmp = tmp > tmpmax ? tmpmax : tmp;
        // tmp = mob's level * 5 - player's current defense skill
        tmp = attackerMaxSkillValueForLevel - tmp;
        if(tmp >= 15)
        {
            // add 2% chance per lacking skill point, min. is 15%
            tmp = tmp * 200 - 1500;
            if (roll < (sum += tmp))
            {
                DEBUG_LOG ("RollMeleeOutcomeAgainst: CRUSHING <%d, %d)", sum-tmp, sum);
                return MELEE_HIT_CRUSHING;
            }
        }
    }

    DEBUG_LOG ("RollMeleeOutcomeAgainst: NORMAL");
    return MELEE_HIT_NORMAL;
}

uint32 Unit::CalculateDamage(WeaponAttackType attType, bool normalized, SpellEntry const* spellProto, Unit* target)
{
    float min_damage, max_damage;

    if (normalized && GetTypeId()==TYPEID_PLAYER) {
        (this->ToPlayer())->CalculateMinMaxDamage(attType,normalized,min_damage, max_damage, target);
    }
    else
    {
        switch (attType)
        {
            case RANGED_ATTACK:
                min_damage = GetFloatValue(UNIT_FIELD_MINRANGEDDAMAGE);
                max_damage = GetFloatValue(UNIT_FIELD_MAXRANGEDDAMAGE);
                break;
            case BASE_ATTACK:
                min_damage = GetFloatValue(UNIT_FIELD_MINDAMAGE);
                max_damage = GetFloatValue(UNIT_FIELD_MAXDAMAGE);
                break;
            case OFF_ATTACK:
                min_damage = GetFloatValue(UNIT_FIELD_MINOFFHANDDAMAGE);
                max_damage = GetFloatValue(UNIT_FIELD_MAXOFFHANDDAMAGE);
                break;
                // Just for good manner
            default:
                min_damage = 0.0f;
                max_damage = 0.0f;
                break;
        }
    }

    if (min_damage > max_damage)
    {
        std::swap(min_damage,max_damage);
    }

    if(max_damage == 0.0f)
        max_damage = 5.0f;

    return GetMap()->urand((uint32)min_damage, (uint32)max_damage);
}

float Unit::CalculateLevelPenalty(SpellEntry const* spellProto) const
{
    if(spellProto->spellLevel <= 0)
        return 1.0f;

    float LvlPenalty = 0.0f;

    if(spellProto->spellLevel < 20)
        LvlPenalty = 20.0f - spellProto->spellLevel * 3.75f;
    float LvlFactor = (float(spellProto->spellLevel) + 6.0f) / float(getLevel());
    if(LvlFactor > 1.0f)
        LvlFactor = 1.0f;

    return (100.0f - LvlPenalty) * LvlFactor / 100.0f;
}

void Unit::SendAttackStart(Unit* pVictim)
{
    WorldPacket data( SMSG_ATTACKSTART, 16 );
    data << uint64(GetGUID());
    data << uint64(pVictim->GetGUID());

    SendMessageToSet(&data, true);
    DEBUG_LOG( "WORLD: Sent SMSG_ATTACKSTART" );
}

void Unit::SendAttackStop(Unit* victim)
{
    if(!victim)
        return;

    WorldPacket data( SMSG_ATTACKSTOP, (4+16) );            // we guess size
    data.append(GetPackGUID());
    data.append(victim->GetPackGUID());                     // can be 0x00...
    data << uint32(0);                                      // can be 0x1
    SendMessageToSet(&data, true);
    sLog.outDetail("%s %u stopped attacking %s %u", (GetTypeId()==TYPEID_PLAYER ? "player" : "creature"), GetGUIDLow(), (victim->GetTypeId()==TYPEID_PLAYER ? "player" : "creature"),victim->GetGUIDLow());
}

bool Unit::isSpellBlocked(Unit *pVictim, SpellEntry const *spellProto, WeaponAttackType attackType)
{
    if (pVictim->HasInArc(M_PI,this))
    {
       float blockChance = pVictim->GetUnitBlockChance();

       float fAttackerSkill = GetWeaponSkillValue(attackType, pVictim)*0.04;
       float fDefenserSkill = pVictim->GetDefenseSkillValue(this)*0.04;

       blockChance += (fDefenserSkill - fAttackerSkill);

       if (blockChance < 0.0)
           blockChance = 0.0;

       if (roll_chance_f(blockChance))
           return true;
    }
    return false;
}

// Melee based spells can be miss, parry or dodge on this step
// Crit or block - determined on damage calculation phase! (and can be both in some time)
float Unit::MeleeSpellMissChance(const Unit *pVictim, WeaponAttackType attType, int32 skillDiff, uint32 spellId) const
{
    // Calculate hit chance (more correct for chance mod)
    int32 HitChance;

    // PvP - PvE melee chances
    /*int32 lchance = pVictim->GetTypeId() == TYPEID_PLAYER ? 5 : 7;
    int32 leveldif = pVictim->getLevelForTarget(this) - getLevelForTarget(pVictim);
    if(leveldif < 3)
        HitChance = 95 - leveldif;
    else
        HitChance = 93 - (leveldif - 2) * lchance;*/
    if (spellId || attType == RANGED_ATTACK || !haveOffhandWeapon() || (GetTypeId() == TYPEID_UNIT && ToCreature()->isWorldBoss()))
        HitChance = 95.0f;
    else
        HitChance = 76.0f;

    // Hit chance depends from victim auras
    if(attType == RANGED_ATTACK)
        HitChance += pVictim->GetTotalAuraModifier(SPELL_AURA_MOD_ATTACKER_RANGED_HIT_CHANCE);
    else
        HitChance += pVictim->GetTotalAuraModifier(SPELL_AURA_MOD_ATTACKER_MELEE_HIT_CHANCE);

    // Spellmod from SPELLMOD_RESIST_MISS_CHANCE
    if(spellId)
    {
        if(Player *modOwner = GetSpellModOwner())
            modOwner->ApplySpellMod(spellId, SPELLMOD_RESIST_MISS_CHANCE, HitChance);
    }

    // Miss = 100 - hit
    float miss_chance= 100.0f - HitChance;

    // Bonuses from attacker aura and ratings
    if (attType == RANGED_ATTACK)
        miss_chance -= m_modRangedHitChance;
    else
        miss_chance -= m_modMeleeHitChance;

    // bonus from skills is 0.04%
    //miss_chance -= skillDiff * 0.04f;
    int32 diff = -skillDiff;
    if(pVictim->GetTypeId()==TYPEID_PLAYER)
        miss_chance += diff > 0 ? diff * 0.04 : diff * 0.02;
    else
        miss_chance += diff > 10 ? 2 + (diff - 10) * 0.4 : diff * 0.1;

    // Limit miss chance from 0 to 60%
    if (miss_chance < 0.0f)
        return 0.0f;
    if (miss_chance > 60.0f)
        return 60.0f;
    return miss_chance;
}


int32 Unit::GetMechanicResistChance(const SpellEntry *spell)
{
    if(!spell)
        return 0;

    int32 resist_mech = 0;
    for(int eff = 0; eff < 3; ++eff)
    {
        if(spell->Effect[eff] == 0)
           break;
        int32 effect_mech = GetEffectMechanic(spell, eff);
        /*if (spell->EffectApplyAuraName[eff] == SPELL_AURA_MOD_TAUNT && (GetEntry() == 24882 || GetEntry() == 23576))
            return int32(1);*/
        if (effect_mech)
        {
            int32 temp = GetTotalAuraModifierByMiscValue(SPELL_AURA_MOD_MECHANIC_RESISTANCE, effect_mech);
            if (resist_mech < temp)
                resist_mech = temp;
        }
    }

    return resist_mech;
}

// Melee based spells hit result calculations
SpellMissInfo Unit::MeleeSpellHitResult(Unit *pVictim, SpellEntry const *spell)
{
    WeaponAttackType attType = BASE_ATTACK;

    if (spell->DmgClass == SPELL_DAMAGE_CLASS_RANGED)
        attType = RANGED_ATTACK;

    // bonus from skills is 0.04% per skill Diff
    int32 attackerWeaponSkill = int32(GetWeaponSkillValue(attType,pVictim));
    int32 skillDiff = attackerWeaponSkill - int32(pVictim->GetMaxSkillValueForLevel(this));
    int32 fullSkillDiff = attackerWeaponSkill - int32(pVictim->GetDefenseSkillValue(this));

    uint32 roll = GetMap()->urand (0, 10000);
    uint32 missChance = uint32(MeleeSpellMissChance(pVictim, attType, fullSkillDiff, spell->Id)*100.0f);

    // Roll miss
    uint32 tmp = spell->AttributesEx3 & SPELL_ATTR_EX3_CANT_MISS ? 0 : missChance;
    if (roll < tmp)
        return SPELL_MISS_MISS;

    // Some spells cannot be parry/dodge
    if (spell->Attributes & SPELL_ATTR_IMPOSSIBLE_DODGE_PARRY_BLOCK)
        return SPELL_MISS_NONE;

    // Chance resist mechanic
    int32 resist_chance = pVictim->GetMechanicResistChance(spell)*100;
    
    // Reduce spell hit chance for dispel mechanic spells from victim SPELL_AURA_MOD_DISPEL_RESIST
    if (IsDispelSpell(spell))
        resist_chance += pVictim->GetTotalAuraModifier(SPELL_AURA_MOD_DISPEL_RESIST)*100;

    tmp += resist_chance;
    bool resist = roll < tmp;

    if (resist)
        return SPELL_MISS_RESIST;

    // Ranged attack can`t miss too
    if (attType == RANGED_ATTACK)
        return SPELL_MISS_NONE;

    bool attackFromBehind = (!pVictim->HasInArc(M_PI,this) || spell->AttributesEx2 & SPELL_ATTR_EX2_BEHIND_TARGET);

    // Roll dodge
    int32 dodgeChance = int32(pVictim->GetUnitDodgeChance()*100.0f) - skillDiff * 4;
    // Reduce enemy dodge chance by SPELL_AURA_MOD_COMBAT_RESULT_CHANCE
    dodgeChance+= GetTotalAuraModifierByMiscValue(SPELL_AURA_MOD_COMBAT_RESULT_CHANCE, VICTIMSTATE_DODGE)*100;
    dodgeChance+= GetTotalAuraModifier(SPELL_AURA_MOD_ENEMY_DODGE)*100;

    // Reduce dodge chance by attacker expertise rating
    if (GetTypeId() == TYPEID_PLAYER)
        dodgeChance-=int32((this->ToPlayer())->GetExpertiseDodgeOrParryReduction(attType) * 100.0f);
    if (dodgeChance < 0)
        dodgeChance = 0;

    // Can`t dodge from behind in PvP (but its possible in PvE)
    if (GetTypeId() == TYPEID_PLAYER && pVictim->GetTypeId() == TYPEID_PLAYER && attackFromBehind)
        dodgeChance = 0;

    // Rogue talent`s cant be dodged
    AuraList const& mCanNotBeDodge = GetAurasByType(SPELL_AURA_IGNORE_COMBAT_RESULT);
    for(AuraList::const_iterator i = mCanNotBeDodge.begin(); i != mCanNotBeDodge.end(); ++i)
    {
        if((*i)->GetModifier()->m_miscvalue == VICTIMSTATE_DODGE)       // can't be dodged rogue finishing move
        {
            if(spell->SpellFamilyName==SPELLFAMILY_ROGUE && (spell->SpellFamilyFlags & SPELLFAMILYFLAG_ROGUE__FINISHING_MOVE))
            {
                dodgeChance = 0;
                break;
            }
        }
    }

    tmp += dodgeChance;
    if (roll < tmp)
        return SPELL_MISS_DODGE;

    // Roll parry
    int32 parryChance = int32(pVictim->GetUnitParryChance()*100.0f)  - skillDiff * 4;
    // Reduce parry chance by attacker expertise rating
    if (GetTypeId() == TYPEID_PLAYER)
        parryChance-=int32((this->ToPlayer())->GetExpertiseDodgeOrParryReduction(attType) * 100.0f);
    // Can`t parry from behind
    if (parryChance < 0 || attackFromBehind)
        parryChance = 0;

    tmp += parryChance;
    if (roll < tmp)
        return SPELL_MISS_PARRY;

    if(spellmgr.isFullyBlockableSpell(spell))
    {
        if (pVictim->HasInArc(M_PI,this))
        {
           float blockChance = pVictim->GetUnitBlockChance();
           blockChance -= (0.04*fullSkillDiff);

           if (blockChance < 0.0)
               blockChance = 0.0;

           tmp += blockChance*100;
           if (roll < tmp)
                return SPELL_MISS_BLOCK;
        }
    }

    return SPELL_MISS_NONE;
}

/*  From 0.0f to 1.0f. Used for binaries spell resistance.
http://www.wowwiki.com/Formulas:Magical_resistance#Magical_Resistances
*/
float Unit::GetAverageSpellResistance(Unit* caster, SpellSchoolMask damageSchoolMask)
{
    if(!caster)
        return 0;

    int32 resistance = GetResistance(GetFirstSchoolInMask(damageSchoolMask));
    resistance += caster->GetTotalAuraModifierByMiscMask(SPELL_AURA_MOD_TARGET_RESISTANCE, damageSchoolMask); // spell penetration

    if(resistance < 0)
        resistance = 0;

    float resistChance = (0.75f * resistance / (caster->getLevel() * 5));
    if(resistChance > 0.75f)
        resistChance = 0.75f;

    return resistChance;
}

SpellMissInfo Unit::MagicSpellHitResult(Unit *pVictim, SpellEntry const *spell, Item* castItem)
{
    // Can`t miss on dead target (on skinning for example)
    if (!pVictim->IsAlive() || spell->AttributesEx3 & SPELL_ATTR_EX3_CANT_MISS)
        return SPELL_MISS_NONE;
        
    // Always 1% resist chance. Send this as SPELL_MISS_MISS (this is not BC blizzlike, this was changed in WotLK).
    uint32 rand = GetMap()->urand(0,10000);
    if (rand > 9900)
        return SPELL_MISS_MISS;

    SpellSchoolMask schoolMask = GetSpellSchoolMask(spell);

    // PvP - PvE spell misschances per leveldif > 2
    int32 lchance = pVictim->GetTypeId() == TYPEID_PLAYER ? 7 : 11;
    int32 myLevel = int32(getLevelForTarget(pVictim));
    // some spells using items should take another caster level into account ("Unreliable against targets higher than...")
    if(castItem) 
    {
        if(spell->maxLevel != 0 && myLevel > spell->maxLevel)
            myLevel = spell->maxLevel;
        else if(castItem->GetProto()->RequiredLevel && castItem->GetProto()->RequiredLevel < 40) //not sure about this but this is based on wowhead.com/item=1404 and seems probable to me
            myLevel = (myLevel > 60) ? 60: myLevel;
    }
    int32 targetLevel = int32(pVictim->getLevelForTarget(this));
    int32 leveldiff = targetLevel - myLevel;

    // Base hit chance from attacker and victim levels
    int32 modHitChance;
    if(leveldiff < 3)
        modHitChance = 96 - leveldiff;
    else
        modHitChance = 94 - (leveldiff - 2) * lchance;

    // Spellmod from SPELLMOD_RESIST_MISS_CHANCE
    if(Player *modOwner = GetSpellModOwner())
        modOwner->ApplySpellMod(spell->Id, SPELLMOD_RESIST_MISS_CHANCE, modHitChance);

    // Increase from attacker SPELL_AURA_MOD_INCREASES_SPELL_PCT_TO_HIT auras
    modHitChance += GetTotalAuraModifierByMiscMask(SPELL_AURA_MOD_INCREASES_SPELL_PCT_TO_HIT, schoolMask);

    // Chance hit from victim SPELL_AURA_MOD_ATTACKER_SPELL_HIT_CHANCE auras
    modHitChance += pVictim->GetTotalAuraModifierByMiscMask(SPELL_AURA_MOD_ATTACKER_SPELL_HIT_CHANCE, schoolMask);

    // Reduce spell hit chance for Area of effect spells from victim SPELL_AURA_MOD_AOE_AVOIDANCE aura
    if (IsAreaOfEffectSpell(spell))
        modHitChance -= pVictim->GetTotalAuraModifier(SPELL_AURA_MOD_AOE_AVOIDANCE);

    // Reduce spell hit chance for dispel mechanic spells from victim SPELL_AURA_MOD_DISPEL_RESIST
    if (IsDispelSpell(spell))
        modHitChance -= pVictim->GetTotalAuraModifier(SPELL_AURA_MOD_DISPEL_RESIST);

    // Chance resist mechanic (select max value from every mechanic spell effect)
    int32 resist_chance = pVictim->GetMechanicResistChance(spell);
    modHitChance -= resist_chance;

    // Chance resist debuff
    modHitChance -= pVictim->GetTotalAuraModifierByMiscValue(SPELL_AURA_MOD_DEBUFF_RESISTANCE, int32(spell->Dispel));

    int32 HitChance = modHitChance * 100;
    // Increase hit chance from attacker SPELL_AURA_MOD_SPELL_HIT_CHANCE and attacker ratings
    HitChance += int32(m_modSpellHitChance*100.0f);

    // Decrease hit chance from victim rating bonus
    if (pVictim->GetTypeId()==TYPEID_PLAYER)
        HitChance -= int32((pVictim->ToPlayer())->GetRatingBonusValue(CR_HIT_TAKEN_SPELL)*100.0f);

    // Hack - Always have 99% on taunts for Nalorakk & Brutallus.
    if ((spell->EffectApplyAuraName[0] == SPELL_AURA_MOD_TAUNT || spell->EffectApplyAuraName[1] == SPELL_AURA_MOD_TAUNT || spell->EffectApplyAuraName[2] == SPELL_AURA_MOD_TAUNT)
        && (pVictim->GetEntry() == 24882 || pVictim->GetEntry() == 23576)) 
    {
        HitChance = 9900;
    }

    // Always have a minimal 1% chance
    if (HitChance <  100) HitChance =  100;

    // Final Result //
    bool resist = rand > HitChance;
    if (resist)
        return SPELL_MISS_RESIST;

    return SPELL_MISS_NONE;
}

// Calculate spell hit result can be:
// Every spell can: Evade/Immune/Reflect/Sucesful hit
// For melee based spells:
//   Miss
//   Dodge
//   Parry
// For spells
//   Resist
SpellMissInfo Unit::SpellHitResult(Unit *pVictim, SpellEntry const *spell, bool CanReflect, Item* castItem)
{
    if (ToCreature() && ToCreature()->isTotem())
        if (Unit *owner = GetOwner())
            return owner->SpellHitResult(pVictim, spell, CanReflect, castItem);

    // Return evade for units in evade mode
    if (pVictim->GetTypeId()==TYPEID_UNIT && (pVictim->ToCreature())->IsInEvadeMode())
        return SPELL_MISS_EVADE;

    // Check for immune (use charges)
    if (pVictim->IsImmunedToSpell(spell,true))
        return SPELL_MISS_IMMUNE;

    // All positive spells can`t miss
    // TODO: client not show miss log for this spells - so need find info for this in dbc and use it!
    if (IsPositiveSpell(spell->Id,!IsFriendlyTo(pVictim)))
        return SPELL_MISS_NONE;

    // Check for immune (use charges)
    if (!(spell->Attributes & SPELL_ATTR_UNAFFECTED_BY_INVULNERABILITY)
        && pVictim->IsImmunedToDamage(GetSpellSchoolMask(spell),true))
        return SPELL_MISS_IMMUNE;

    if(this == pVictim)
        return SPELL_MISS_NONE;
        
    //sLog.outString("SpellHitResult1 %u", spell->Id);

    // Try victim reflect spell
    if (CanReflect)
    {
        //sLog.outString("SpellHitResult2 %u", spell->Id);
        int32 reflectchance = pVictim->GetTotalAuraModifier(SPELL_AURA_REFLECT_SPELLS);
        //sLog.outString("SpellHitResult3 %u - reflect chance %d", spell->Id, reflectchance);
        Unit::AuraList const& mReflectSpellsSchool = pVictim->GetAurasByType(SPELL_AURA_REFLECT_SPELLS_SCHOOL);
        for(Unit::AuraList::const_iterator i = mReflectSpellsSchool.begin(); i != mReflectSpellsSchool.end(); ++i) {
            //sLog.outString("For1 %u %u", spell->Id, (*i)->GetId());
            if((*i)->GetModifier()->m_miscvalue & GetSpellSchoolMask(spell)) {
                //sLog.outString("For2 %u %u %u %u %d", spell->Id, (*i)->GetId(), (*i)->GetModifier()->m_miscvalue, GetSpellSchoolMask(spell), (*i)->GetModifierValue());
                reflectchance = (*i)->GetModifierValue();
            }
        }
        //sLog.outString("SpellHitResult4 %u - reflect chance %d", spell->Id, reflectchance);
        if (reflectchance > 0 && roll_chance_i(reflectchance))
        {
            // Start triggers for remove charges if need (trigger only for victim, and mark as active spell)
            ProcDamageAndSpell(pVictim, PROC_FLAG_NONE, PROC_FLAG_TAKEN_NEGATIVE_SPELL_HIT, PROC_EX_REFLECT, 1, BASE_ATTACK, spell);
            // FIXME: Add a flag on unit itself, not to setRemoveReflect if unit is already flagged for it (prevent infinite delay on reflect lolz)
            if (Spell* sp = m_currentSpells[CURRENT_CHANNELED_SPELL])
                sp->setRemoveReflect();
            else if (Spell* sp = m_currentSpells[CURRENT_GENERIC_SPELL])
                sp->setRemoveReflect();
            return SPELL_MISS_REFLECT;
        }
    }

    //Check magic resistance for binaries spells (see IsBinaryMagicResistanceSpell(...) for more details). This check is not rolled inside attack table.
    if(    spellmgr.IsBinaryMagicResistanceSpell(spell)
        && !(spell->AttributesEx4 & SPELL_ATTR_EX4_IGNORE_RESISTANCES) 
        && !(spell->AttributesEx3 & SPELL_ATTR_EX3_CANT_MISS)  )
    {
        float random = (float)rand()/(float)RAND_MAX;
        float resistChance = pVictim->GetAverageSpellResistance(this,(SpellSchoolMask)spell->SchoolMask);
        if(resistChance > random)
            return SPELL_MISS_RESIST;
        //no else, the binary spell can still be resisted in the next check
    }

    switch (spell->DmgClass)
    {
        case SPELL_DAMAGE_CLASS_RANGED:
        case SPELL_DAMAGE_CLASS_MELEE:
            return MeleeSpellHitResult(pVictim, spell);
        case SPELL_DAMAGE_CLASS_NONE:
            if (spell->SchoolMask & SPELL_SCHOOL_MASK_SPELL)
                return MagicSpellHitResult(pVictim, spell, castItem);
            else
                return SPELL_MISS_NONE;
        case SPELL_DAMAGE_CLASS_MAGIC:
            return MagicSpellHitResult(pVictim, spell, castItem);
    }

    return SPELL_MISS_NONE;
}

uint32 Unit::GetDefenseSkillValue(Unit const* target) const
{
    if(GetTypeId() == TYPEID_PLAYER)
    {
        // in PvP use full skill instead current skill value
        uint32 value = (target && target->GetTypeId() == TYPEID_PLAYER)
            ? (this->ToPlayer())->GetMaxSkillValue(SKILL_DEFENSE)
            : (this->ToPlayer())->GetSkillValue(SKILL_DEFENSE);
        value += uint32((this->ToPlayer())->GetRatingBonusValue(CR_DEFENSE_SKILL));
        return value;
    }
    else
        return GetUnitMeleeSkill(target);
}

float Unit::GetUnitDodgeChance() const
{
    if (HasUnitState(UNIT_STAT_LOST_CONTROL))
        return 0.0f;
    if( GetTypeId() == TYPEID_PLAYER )
        return GetFloatValue(PLAYER_DODGE_PERCENTAGE);
    else
    {
        if(((Creature const*)this)->isTotem())
            return 0.0f;
        else
        {
            float dodge = 5.0f;
            dodge += GetTotalAuraModifier(SPELL_AURA_MOD_DODGE_PERCENT);
            return dodge > 0.0f ? dodge : 0.0f;
        }
    }
}

float Unit::GetUnitParryChance() const
{
    if (IsNonMeleeSpellCasted(false) || HasUnitState(UNIT_STAT_LOST_CONTROL))
        return 0.0f;

    float chance = 0.0f;

    if(GetTypeId() == TYPEID_PLAYER)
    {
        Player const* player = (Player const*)this;
        if(player->CanParry() )
        {
            Item *tmpitem = player->GetWeaponForAttack(BASE_ATTACK,true);
            if(!tmpitem)
                tmpitem = player->GetWeaponForAttack(OFF_ATTACK,true);

            if(tmpitem)
                chance = GetFloatValue(PLAYER_PARRY_PERCENTAGE);
        }
    }
    else if(GetTypeId() == TYPEID_UNIT)
    {
        if(ToCreature()->GetCreatureInfo()->flags_extra & CREATURE_FLAG_EXTRA_NO_PARRY
           || ToCreature()->isTotem())
            chance = 0.0f;
        else if(ToCreature()->isWorldBoss()) // Add some parry chance for bosses. Nobody seems to knows the exact rule but it's somewhere around 14%.
            chance = 13.0f;
        else if(GetCreatureType() != CREATURE_TYPE_BEAST)
        {
            chance = 5.0f;
            chance += GetTotalAuraModifier(SPELL_AURA_MOD_PARRY_PERCENT);
        }

    }

    return chance > 0.0f ? chance : 0.0f;
}

float Unit::GetUnitBlockChance() const
{
    if ( IsNonMeleeSpellCasted(false) || IsCCed())
        return 0.0f;

    if(GetTypeId() == TYPEID_PLAYER)
    {
        Player const* player = (Player const*)this;
        if(player->CanBlock() )
            return GetFloatValue(PLAYER_BLOCK_PERCENTAGE);

        // is player but has no block ability or no not broken shield equipped
        return 0.0f;
    }
    else
    {
        if(   (ToCreature()->GetCreatureInfo()->flags_extra & CREATURE_FLAG_EXTRA_NO_BLOCK)
           || (ToCreature()->isTotem()) )
            return 0.0f;
        else
        {
            float block = 5.0f;
            block += GetTotalAuraModifier(SPELL_AURA_MOD_BLOCK_PERCENT);
            return block > 0.0f ? block : 0.0f;
        }
    }
}

float Unit::GetUnitCriticalChance(WeaponAttackType attackType, const Unit *pVictim) const
{
    float crit;

    if(GetTypeId() == TYPEID_PLAYER)
    {
        switch(attackType)
        {
            case BASE_ATTACK:
                crit = GetFloatValue( PLAYER_CRIT_PERCENTAGE );
                break;
            case OFF_ATTACK:
                crit = GetFloatValue( PLAYER_OFFHAND_CRIT_PERCENTAGE );
                break;
            case RANGED_ATTACK:
                crit = GetFloatValue( PLAYER_RANGED_CRIT_PERCENTAGE );
                break;
                // Just for good manner
            default:
                crit = 0.0f;
                break;
        }
    }
    else
    {
        crit = 5.0f;
        crit += GetTotalAuraModifier(SPELL_AURA_MOD_CRIT_PERCENT);
    }

    // flat aura mods
    if(attackType == RANGED_ATTACK)
        crit += pVictim->GetTotalAuraModifier(SPELL_AURA_MOD_ATTACKER_RANGED_CRIT_CHANCE);
    else
        crit += pVictim->GetTotalAuraModifier(SPELL_AURA_MOD_ATTACKER_MELEE_CRIT_CHANCE);

    crit += pVictim->GetTotalAuraModifier(SPELL_AURA_MOD_ATTACKER_SPELL_AND_WEAPON_CRIT_CHANCE);

    // reduce crit chance from Rating for players
    if (pVictim->GetTypeId()==TYPEID_PLAYER)
    {
        if (attackType==RANGED_ATTACK)
            crit -= (pVictim->ToPlayer())->GetRatingBonusValue(CR_CRIT_TAKEN_RANGED);
        else
            crit -= (pVictim->ToPlayer())->GetRatingBonusValue(CR_CRIT_TAKEN_MELEE);
    }

    if (crit < 0.0f)
        crit = 0.0f;
    return crit;
}

uint32 Unit::GetWeaponSkillValue (WeaponAttackType attType, Unit const* target) const
{
    uint32 value = 0;
    if(GetTypeId() == TYPEID_PLAYER)
    {
        Item* item = (this->ToPlayer())->GetWeaponForAttack(attType,true);

        // feral or unarmed skill only for base attack
        if(attType != BASE_ATTACK && !item )
        {
            if(attType == RANGED_ATTACK && getClass() == CLASS_PALADIN) //hammer
                return GetMaxSkillValueForLevel();
            return 0;
        }

        if((this->ToPlayer())->IsInFeralForm())
            return GetMaxSkillValueForLevel();              // always maximized SKILL_FERAL_COMBAT in fact

        // weapon skill or (unarmed for base attack)
        uint32  skill = item ? item->GetSkill() : SKILL_UNARMED;

        // in PvP use full skill instead current skill value
        value = (target && target->isCharmedOwnedByPlayerOrPlayer())
            ? (this->ToPlayer())->GetMaxSkillValue(skill)
            : (this->ToPlayer())->GetSkillValue(skill);
        // Modify value from ratings
        value += uint32((this->ToPlayer())->GetRatingBonusValue(CR_WEAPON_SKILL));
        switch (attType)
        {
            case BASE_ATTACK:   value+=uint32((this->ToPlayer())->GetRatingBonusValue(CR_WEAPON_SKILL_MAINHAND));break;
            case OFF_ATTACK:    value+=uint32((this->ToPlayer())->GetRatingBonusValue(CR_WEAPON_SKILL_OFFHAND));break;
            case RANGED_ATTACK: value+=uint32((this->ToPlayer())->GetRatingBonusValue(CR_WEAPON_SKILL_RANGED));break;
        }
    }
    else
        value = GetUnitMeleeSkill(target);
   return value;
}

void Unit::_DeleteAuras()
{
    while(!m_removedAuras.empty())
    {
        delete m_removedAuras.front();
        m_removedAuras.pop_front();
    }
}

void Unit::_UpdateSpells( uint32 time )
{
    if(m_currentSpells[CURRENT_AUTOREPEAT_SPELL])
        _UpdateAutoRepeatSpell();

    // remove finished spells from current pointers
    for (uint32 i = 0; i < CURRENT_MAX_SPELL; i++)
    {
        if (m_currentSpells[i] && m_currentSpells[i]->getState() == SPELL_STATE_FINISHED)
        {
            m_currentSpells[i]->SetReferencedFromCurrent(false);
            m_currentSpells[i] = NULL;                      // remove pointer
        }
    }

    // update auras
    // m_AurasUpdateIterator can be updated in inderect called code at aura remove to skip next planned to update but removed auras
    for (m_AurasUpdateIterator = m_Auras.begin(); m_AurasUpdateIterator != m_Auras.end(); )
    {
        Aura* i_aura = m_AurasUpdateIterator->second;
        ++m_AurasUpdateIterator;                            // need shift to next for allow update if need into aura update
        if (i_aura)
            i_aura->Update(time);
    }

    // remove expired auras
    for (AuraMap::iterator i = m_Auras.begin(); i != m_Auras.end(); )
    {
        if ( i->second->IsExpired() )
            RemoveAura(i);
        else
            ++i;
    }

    _DeleteAuras();

    if(!m_gameObj.empty())
    {
        std::list<GameObject*>::iterator ite1, dnext1;
        for (ite1 = m_gameObj.begin(); ite1 != m_gameObj.end(); ite1 = dnext1)
        {
            dnext1 = ite1;
            //(*i)->Update( difftime );
            if( !(*ite1)->isSpawned() )
            {
                (*ite1)->SetOwnerGUID(0);
                (*ite1)->SetRespawnTime(0);
                (*ite1)->Delete();
                dnext1 = m_gameObj.erase(ite1);
            }
            else
                ++dnext1;
        }
    }
}

void Unit::_UpdateAutoRepeatSpell()
{
    //check "realtime" interrupts
    if ( (GetTypeId() == TYPEID_PLAYER && (this->ToPlayer())->isMoving()) || IsNonMeleeSpellCasted(false,false,true) )
    {
        // cancel wand shoot
        if(m_currentSpells[CURRENT_AUTOREPEAT_SPELL]->m_spellInfo->Category == 351)
            InterruptSpell(CURRENT_AUTOREPEAT_SPELL);
        m_AutoRepeatFirstCast = true;
        return;
    }

    //apply delay
    if ( m_AutoRepeatFirstCast && getAttackTimer(RANGED_ATTACK) < 500 )
        setAttackTimer(RANGED_ATTACK,500);
    m_AutoRepeatFirstCast = false;

    //castroutine
    if (isAttackReady(RANGED_ATTACK))
    {
        // Check if able to cast
        if(m_currentSpells[CURRENT_AUTOREPEAT_SPELL]->CheckCast(true) != SPELL_CAST_OK)
        {
            InterruptSpell(CURRENT_AUTOREPEAT_SPELL);
            return;
        }

        // we want to shoot
        Spell* spell = new Spell(this, m_currentSpells[CURRENT_AUTOREPEAT_SPELL]->m_spellInfo, true, 0);
        spell->prepare(&(m_currentSpells[CURRENT_AUTOREPEAT_SPELL]->m_targets));

        // all went good, reset attack
        resetAttackTimer(RANGED_ATTACK);
    }
}

void Unit::SetCurrentCastedSpell( Spell * pSpell )
{
    assert(pSpell);                                         // NULL may be never passed here, use InterruptSpell or InterruptNonMeleeSpells

    uint32 CSpellType = pSpell->GetCurrentContainer();

    if (pSpell == m_currentSpells[CSpellType]) return;      // avoid breaking self

    // break same type spell if it is not delayed
    InterruptSpell(CSpellType,false);

    // special breakage effects:
    switch (CSpellType)
    {
        case CURRENT_GENERIC_SPELL:
        {
            // generic spells always break channeled not delayed spells
            InterruptSpell(CURRENT_CHANNELED_SPELL,false);

            // break wand autorepeat
            if ( m_currentSpells[CURRENT_AUTOREPEAT_SPELL] &&
                 m_currentSpells[CURRENT_AUTOREPEAT_SPELL]->m_spellInfo->Category == 351) // wand
                    InterruptSpell(CURRENT_AUTOREPEAT_SPELL);

            //delay autoshoot by 0,5s
            if( pSpell->GetCastTime() > 0) // instant spells don't break autoshoot anymore, see 2.4.3 patchnotes)
                m_AutoRepeatFirstCast = true;

            addUnitState(UNIT_STAT_CASTING);
        } break;

        case CURRENT_CHANNELED_SPELL:
        {
            // channel spells always break generic non-delayed and any channeled spells
            InterruptSpell(CURRENT_GENERIC_SPELL,false);
            InterruptSpell(CURRENT_CHANNELED_SPELL);

            // break wand autorepeat
            if ( m_currentSpells[CURRENT_AUTOREPEAT_SPELL] &&
                m_currentSpells[CURRENT_AUTOREPEAT_SPELL]->m_spellInfo->Category == 351 )
                InterruptSpell(CURRENT_AUTOREPEAT_SPELL);

            //delay autoshoot by 0,5s
            m_AutoRepeatFirstCast = true;

            addUnitState(UNIT_STAT_CASTING);
        } break;

        case CURRENT_AUTOREPEAT_SPELL:
        {
            // wand break other spells
            if (pSpell->m_spellInfo->Category == 351)
            {
                // generic autorepeats break generic non-delayed and channeled non-delayed spells
                InterruptSpell(CURRENT_GENERIC_SPELL,false);
                InterruptSpell(CURRENT_CHANNELED_SPELL,false);
            }
            // special action: set first cast flag
            m_AutoRepeatFirstCast = true;
        } break;

        default:
        {
            // other spell types don't break anything now
        } break;
    }

    // current spell (if it is still here) may be safely deleted now
    if (m_currentSpells[CSpellType])
        m_currentSpells[CSpellType]->SetReferencedFromCurrent(false);

    // set new current spell
    m_currentSpells[CSpellType] = pSpell;
    pSpell->SetReferencedFromCurrent(true);
}

void Unit::InterruptSpell(uint32 spellType, bool withDelayed, bool withInstant)
{
    assert(spellType < CURRENT_MAX_SPELL);

    Spell *spell = m_currentSpells[spellType];
    if(spell
        && (withDelayed || spell->getState() != SPELL_STATE_DELAYED)
        && (withInstant || spell->GetCastTime() > 0))
    {
        // for example, do not let self-stun aura interrupt itself
        if(!spell->IsInterruptable())
            return;

        m_currentSpells[spellType] = NULL;

        // send autorepeat cancel message for autorepeat spells
        if (spellType == CURRENT_AUTOREPEAT_SPELL)
        {
            if(GetTypeId()==TYPEID_PLAYER)
                (this->ToPlayer())->SendAutoRepeatCancel();
        }

        if (spell->getState() != SPELL_STATE_FINISHED)
            spell->cancel();
        spell->SetReferencedFromCurrent(false);
    }
}

bool Unit::HasDelayedSpell()
{
    if ( m_currentSpells[CURRENT_GENERIC_SPELL] &&
        (m_currentSpells[CURRENT_GENERIC_SPELL]->getState() == SPELL_STATE_DELAYED) )
        return true;

    return false;
}

bool Unit::IsNonMeleeSpellCasted(bool withDelayed, bool skipChanneled, bool skipAutorepeat) const
{
    // We don't do loop here to explicitly show that melee spell is excluded.
    // Maybe later some special spells will be excluded too.

    // generic spells are casted when they are not finished and not delayed
    if ( m_currentSpells[CURRENT_GENERIC_SPELL] &&
        (m_currentSpells[CURRENT_GENERIC_SPELL]->getState() != SPELL_STATE_FINISHED) &&
        (withDelayed || m_currentSpells[CURRENT_GENERIC_SPELL]->getState() != SPELL_STATE_DELAYED) )
        return(true);

    // channeled spells may be delayed, but they are still considered casted
    else if ( !skipChanneled && m_currentSpells[CURRENT_CHANNELED_SPELL] &&
        (m_currentSpells[CURRENT_CHANNELED_SPELL]->getState() != SPELL_STATE_FINISHED) )
        return(true);

    // autorepeat spells may be finished or delayed, but they are still considered casted
    else if ( !skipAutorepeat && m_currentSpells[CURRENT_AUTOREPEAT_SPELL] )
        return(true);

    return(false);
}

void Unit::InterruptNonMeleeSpells(bool withDelayed, uint32 spell_id, bool withInstant)
{
    // generic spells are interrupted if they are not finished or delayed
    if (m_currentSpells[CURRENT_GENERIC_SPELL] && (!spell_id || m_currentSpells[CURRENT_GENERIC_SPELL]->m_spellInfo->Id==spell_id))
        InterruptSpell(CURRENT_GENERIC_SPELL,withDelayed,withInstant);

    // autorepeat spells are interrupted if they are not finished or delayed
    if (m_currentSpells[CURRENT_AUTOREPEAT_SPELL] && (!spell_id || m_currentSpells[CURRENT_AUTOREPEAT_SPELL]->m_spellInfo->Id==spell_id))
        InterruptSpell(CURRENT_AUTOREPEAT_SPELL,withDelayed,withInstant);

    // channeled spells are interrupted if they are not finished, even if they are delayed
    if (m_currentSpells[CURRENT_CHANNELED_SPELL] && (!spell_id || m_currentSpells[CURRENT_CHANNELED_SPELL]->m_spellInfo->Id==spell_id))
        InterruptSpell(CURRENT_CHANNELED_SPELL,true,true);
}

Spell* Unit::FindCurrentSpellBySpellId(uint32 spell_id) const
{
    for (uint32 i = 0; i < CURRENT_MAX_SPELL; i++)
        if(m_currentSpells[i] && m_currentSpells[i]->m_spellInfo->Id==spell_id)
            return m_currentSpells[i];
    return NULL;
}

bool Unit::isInAccessiblePlaceFor(Creature const* c) const
{
    if(IsInWater())
        return c->canSwim();
    else
        return c->canWalk() || c->canFly();
}

bool Unit::IsInWater() const
{
    if (!Trinity::IsValidMapCoord(GetPositionX(), GetPositionY(), GetPositionZ()))
        return false;

    return MapManager::Instance().GetBaseMap(GetMapId())->IsInWater(GetPositionX(),GetPositionY(), GetPositionZ());
}

bool Unit::IsUnderWater() const
{
    if (!Trinity::IsValidMapCoord(GetPositionX(), GetPositionY(), GetPositionZ()))
        return false;

    return MapManager::Instance().GetBaseMap(GetMapId())->IsUnderWater(GetPositionX(),GetPositionY(),GetPositionZ());
}

void Unit::DeMorph()
{
    SetDisplayId(GetNativeDisplayId());
}

int32 Unit::GetTotalAuraModifier(AuraType auratype) const
{
    int32 modifier = 0;

    AuraList const& mTotalAuraList = GetAurasByType(auratype);
    if(mTotalAuraList.empty())
        return modifier;

    for(AuraList::const_iterator i = mTotalAuraList.begin();i != mTotalAuraList.end(); ++i)
        if ((*i))
            modifier += (*i)->GetModifierValue();

    return modifier;
}

float Unit::GetTotalAuraMultiplier(AuraType auratype) const
{
    float multiplier = 1.0f;

    AuraList const& mTotalAuraList = GetAurasByType(auratype);
    if(mTotalAuraList.empty())
        return multiplier;

    for(AuraList::const_iterator i = mTotalAuraList.begin();i != mTotalAuraList.end(); ++i)
        if ((*i))
            multiplier *= (100.0f + (*i)->GetModifierValue())/100.0f;

    return multiplier;
}

int32 Unit::GetMaxPositiveAuraModifier(AuraType auratype) const
{
    int32 modifier = 0;

    AuraList const& mTotalAuraList = GetAurasByType(auratype);
    if(mTotalAuraList.empty())
        return modifier;

    for(AuraList::const_iterator i = mTotalAuraList.begin();i != mTotalAuraList.end(); ++i)
    {
        if ((*i))
        {
            int32 amount = (*i)->GetModifierValue();
            if (amount > modifier)
                modifier = amount;
        }
    }

    return modifier;
}

int32 Unit::GetMaxNegativeAuraModifier(AuraType auratype) const
{
    int32 modifier = 0;

    AuraList const& mTotalAuraList = GetAurasByType(auratype);
    if(mTotalAuraList.empty())
        return modifier;

    for(AuraList::const_iterator i = mTotalAuraList.begin();i != mTotalAuraList.end(); ++i)
    {
        if ((*i))
        {
            int32 amount = (*i)->GetModifierValue();
            if (amount < modifier)
                modifier = amount;
        }
    }

    return modifier;
}

int32 Unit::GetTotalAuraModifierByMiscMask(AuraType auratype, uint32 misc_mask) const
{
    int32 modifier = 0;

    AuraList const& mTotalAuraList = GetAurasByType(auratype);
    if(mTotalAuraList.empty())
        return modifier;

    for(AuraList::const_iterator i = mTotalAuraList.begin();i != mTotalAuraList.end(); ++i)
    {
        if ((*i))
        {
            Modifier* mod = (*i)->GetModifier();
            if (mod->m_miscvalue & misc_mask)
                modifier += (*i)->GetModifierValue();
        }
    }
    return modifier;
}

float Unit::GetTotalAuraMultiplierByMiscMask(AuraType auratype, uint32 misc_mask) const
{
    float multiplier = 1.0f;

    AuraList const& mTotalAuraList = GetAurasByType(auratype);
    if(mTotalAuraList.empty())
        return multiplier;

    for(AuraList::const_iterator i = mTotalAuraList.begin();i != mTotalAuraList.end(); ++i)
    {
        if ((*i))
        {
            Modifier* mod = (*i)->GetModifier();
            if (mod->m_miscvalue & misc_mask)
                multiplier *= (100.0f + (*i)->GetModifierValue())/100.0f;
        }
    }
    return multiplier;
}

int32 Unit::GetMaxPositiveAuraModifierByMiscMask(AuraType auratype, uint32 misc_mask) const
{
    int32 modifier = 0;

    AuraList const& mTotalAuraList = GetAurasByType(auratype);
    if(mTotalAuraList.empty())
        return modifier;

    for(AuraList::const_iterator i = mTotalAuraList.begin();i != mTotalAuraList.end(); ++i)
    {
        if ((*i))
        {
            Modifier* mod = (*i)->GetModifier();
            int32 amount = (*i)->GetModifierValue();
            if (mod->m_miscvalue & misc_mask && amount > modifier)
                modifier = amount;
        }
    }

    return modifier;
}

int32 Unit::GetMaxNegativeAuraModifierByMiscMask(AuraType auratype, uint32 misc_mask) const
{
    int32 modifier = 0;

    AuraList const& mTotalAuraList = GetAurasByType(auratype);
    if(mTotalAuraList.empty())
        return modifier;

    for(AuraList::const_iterator i = mTotalAuraList.begin();i != mTotalAuraList.end(); ++i)
    {
        if ((*i))
        {
            Modifier* mod = (*i)->GetModifier();
            int32 amount = (*i)->GetModifierValue();
            if (mod->m_miscvalue & misc_mask && amount < modifier)
                modifier = amount;
        }
    }

    return modifier;
}

int32 Unit::GetTotalAuraModifierByMiscValue(AuraType auratype, int32 misc_value) const
{
    int32 modifier = 0;

    AuraList const& mTotalAuraList = GetAurasByType(auratype);
    if(mTotalAuraList.empty())
        return modifier;

    for(AuraList::const_iterator i = mTotalAuraList.begin();i != mTotalAuraList.end(); ++i)
    {
        if ((*i))
        {
            Modifier* mod = (*i)->GetModifier();
            if (mod->m_miscvalue == misc_value)
                modifier += (*i)->GetModifierValue();
        }
    }
    return modifier;
}

float Unit::GetTotalAuraMultiplierByMiscValue(AuraType auratype, int32 misc_value) const
{
    float multiplier = 1.0f;

    AuraList const& mTotalAuraList = GetAurasByType(auratype);
    if(mTotalAuraList.empty())
        return multiplier;

    for(AuraList::const_iterator i = mTotalAuraList.begin();i != mTotalAuraList.end(); ++i)
    {
        if ((*i))
        {
            Modifier* mod = (*i)->GetModifier();
            if (mod->m_miscvalue == misc_value)
                multiplier *= (100.0f + (*i)->GetModifierValue())/100.0f;
        }
    }
    return multiplier;
}

int32 Unit::GetMaxPositiveAuraModifierByMiscValue(AuraType auratype, int32 misc_value) const
{
    int32 modifier = 0;

    AuraList const& mTotalAuraList = GetAurasByType(auratype);
    if(mTotalAuraList.empty())
        return modifier;

    for(AuraList::const_iterator i = mTotalAuraList.begin();i != mTotalAuraList.end(); ++i)
    {
        if ((*i))
        {
            Modifier* mod = (*i)->GetModifier();
            int32 amount = (*i)->GetModifierValue();
            if (mod->m_miscvalue == misc_value && amount > modifier)
                modifier = amount;
        }
    }

    return modifier;
}

int32 Unit::GetMaxNegativeAuraModifierByMiscValue(AuraType auratype, int32 misc_value) const
{
    int32 modifier = 0;

    AuraList const& mTotalAuraList = GetAurasByType(auratype);
    if(mTotalAuraList.empty())
        return modifier;

    for(AuraList::const_iterator i = mTotalAuraList.begin();i != mTotalAuraList.end(); ++i)
    {
        if ((*i))
        {
            Modifier* mod = (*i)->GetModifier();
            int32 amount = (*i)->GetModifierValue();
            if (mod->m_miscvalue == misc_value && amount < modifier)
                modifier = amount;
        }
    }

    return modifier;
}

bool Unit::AddAura(Aura *Aur)
{
    // ghost spell check, allow apply any auras at player loading in ghost mode (will be cleanup after load)
    if( (!IsAlive() && !(Aur->GetSpellProto()->Attributes & SPELL_ATTR_CASTABLE_WHILE_DEAD)) && Aur->GetId() != 20584 && Aur->GetId() != 8326 && Aur->GetId() != 2584 &&
        (GetTypeId()!=TYPEID_PLAYER || !(this->ToPlayer())->GetSession()->PlayerLoading()) )
    {
        delete Aur;
        return false;
    }

    if (IsImmunedToSpell(Aur->GetSpellProto())) {
        delete Aur;
        return false;
    }

    if(Aur->GetTarget() != this)
    {
        sLog.outError("Aura (spell %u eff %u) add to aura list of %s (lowguid: %u) but Aura target is %s (lowguid: %u)",
            Aur->GetId(),Aur->GetEffIndex(),(GetTypeId()==TYPEID_PLAYER?"player":"creature"),GetGUIDLow(),
            (Aur->GetTarget()->GetTypeId()==TYPEID_PLAYER?"player":"creature"),Aur->GetTarget()->GetGUIDLow());
        delete Aur;
        return false;
    }

    if ((Aur->DoesAuraApplyAuraName(SPELL_AURA_MOD_CONFUSE) || Aur->DoesAuraApplyAuraName(SPELL_AURA_MOD_CHARM) ||
        Aur->DoesAuraApplyAuraName(SPELL_AURA_MOD_STUN)) && (Aur->GetSpellProto()->Attributes & SPELL_ATTR_BREAKABLE_BY_DAMAGE))
        m_justCCed = 2;

    SpellEntry const* aurSpellInfo = Aur->GetSpellProto();

    spellEffectPair spair = spellEffectPair(Aur->GetId(), Aur->GetEffIndex());

    bool stackModified=false;
    bool doubleMongoose=false;
    //if (Aur->GetId() == 28093) sLog.outString("Mongoose proc from item "I64FMTD, Aur->GetCastItemGUID());
    // passive and persistent auras can stack with themselves any number of times
    if (!Aur->IsPassive() && !Aur->IsPersistent())
    {
        for(AuraMap::iterator i2 = m_Auras.lower_bound(spair); i2 != m_Auras.upper_bound(spair);)
        {
            if(i2->second->GetCasterGUID()==Aur->GetCasterGUID())
            {
                if (!stackModified)
                {
                    // auras from same caster but different items (mongoose) can stack
                    if(Aur->GetCastItemGUID() != i2->second->GetCastItemGUID() && Aur->GetId() == 28093) {
                        i2++;
                        doubleMongoose = true;
                        //sLog.outString("Mongoose double proc from item "I64FMTD" !", Aur->GetCastItemGUID());
                        continue;
                    }
                    else if(aurSpellInfo->StackAmount) // replace aura if next will > spell StackAmount
                    {
                        // prevent adding stack more than once
                        stackModified=true;
                        Aur->SetStackAmount(i2->second->GetStackAmount());
                        Aur->SetPeriodicTimer(i2->second->GetPeriodicTimer());
                        if(Aur->GetStackAmount() < aurSpellInfo->StackAmount)
                            Aur->SetStackAmount(Aur->GetStackAmount()+1);
                    }
                    //keep old modifier if higher than new aura modifier
                    //(removed for now, this causes problem with some stacking auras)
                    /*
                    if(i2->second->GetModifierValuePerStack() > Aur->GetModifierValuePerStack())
                        Aur->SetModifierValuePerStack(i2->second->GetModifierValuePerStack());
                    if(i2->second->GetBasePoints() > Aur->GetBasePoints())
                        Aur->SetBasePoints(i2->second->GetBasePoints());
                        */
                    RemoveAura(i2,AURA_REMOVE_BY_STACK);
                    i2=m_Auras.lower_bound(spair);
                    continue;
                }
            }
            else if (spellmgr.GetSpellCustomAttr(Aur->GetId()) & SPELL_ATTR_CU_SAME_STACK_DIFF_CASTERS) {
                stackModified=true;
                Aur->SetStackAmount(i2->second->GetStackAmount());
                if(Aur->GetStackAmount() < aurSpellInfo->StackAmount)
                    Aur->SetStackAmount(Aur->GetStackAmount()+1);
            }
            else if (spellmgr.GetSpellCustomAttr(Aur->GetId()) & SPELL_ATTR_CU_ONE_STACK_PER_CASTER_SPECIAL) {
                ++i2;
                continue;
            }
            switch(aurSpellInfo->EffectApplyAuraName[Aur->GetEffIndex()])
            {
                // DOT or HOT from different casters will stack
                case SPELL_AURA_MOD_DECREASE_SPEED:
                    // Mind Flay
                    if(aurSpellInfo->SpellFamilyFlags & 0x0000000000800000LL && aurSpellInfo->SpellFamilyName == SPELLFAMILY_PRIEST)
                    {
                        ++i2;
                        continue;
                    }
                    break;
                case SPELL_AURA_MOD_DAMAGE_PERCENT_DONE:
                    // Ferocious Inspiration
                    if (aurSpellInfo->Id == 34456) {
                        ++i2;
                        continue;
                    }
                    break;
                case SPELL_AURA_DUMMY:
                    /* X don't merge to TC2 - BoL was removed and Mangle changed aura from dummy to 255 */
                    
                    // Blessing of Light exception - only one per target
                    if (aurSpellInfo->SpellVisual == 9180 && aurSpellInfo->SpellFamilyName == SPELLFAMILY_PALADIN)
                        break;
                    // Druid Mangle bear & cat
                    if (aurSpellInfo->SpellFamilyName == SPELLFAMILY_DRUID && aurSpellInfo->SpellIconID == 2312)
                        break;
                case SPELL_AURA_PERIODIC_DAMAGE:
                    if (aurSpellInfo->Id == 45032 || aurSpellInfo->Id == 45034) // Curse of Boundless Agony can only have one stack per target
                        break;
                    if (aurSpellInfo->Id == 44335)      // Vexallus
                        break;
                case SPELL_AURA_PERIODIC_HEAL:
                case SPELL_AURA_PERIODIC_TRIGGER_SPELL:
                    if (aurSpellInfo->Id == 31944) // Doomfire DoT - only one per target
                        break;
                case SPELL_AURA_PERIODIC_ENERGIZE:
                case SPELL_AURA_PERIODIC_MANA_LEECH:
                case SPELL_AURA_PERIODIC_LEECH:
                case SPELL_AURA_POWER_BURN_MANA:
                case SPELL_AURA_OBS_MOD_MANA:
                case SPELL_AURA_OBS_MOD_HEALTH:
                    ++i2;
                    continue;
            }
            /*
            //keep old modifier if higher than new aura modifier
            //(removed for now, this causes problem with some stacking auras)
            if(i2->second->GetModifierValuePerStack() > Aur->GetModifierValuePerStack())
                Aur->SetModifierValuePerStack(i2->second->GetModifierValuePerStack());
            if(i2->second->GetBasePoints() > Aur->GetBasePoints())
                Aur->SetBasePoints(i2->second->GetBasePoints());
            */
            RemoveAura(i2,AURA_REMOVE_BY_STACK);
            i2=m_Auras.lower_bound(spair);
            continue;
        }
    }

    // passive auras stack with all (except passive spell proc auras)
    if ((!Aur->IsPassive() || !IsPassiveStackableSpell(Aur->GetId())) &&
        !(Aur->GetId() == 20584 || Aur->GetId() == 8326 || Aur->GetId() == 28093))
    {
        if (!RemoveNoStackAurasDueToAura(Aur))
        {
            delete Aur;
            return false;                                   // couldn't remove conflicting aura with higher rank
        }
    }

    // update single target auras list (before aura add to aura list, to prevent unexpected remove recently added aura)
    if (Aur->IsSingleTarget() && Aur->GetTarget())
    {
        m_GiantLock.acquire();
        // caster pointer can be deleted in time aura remove, find it by guid at each iteration
        for(;;)
        {
            Unit* caster = Aur->GetCaster();
            if(!caster)                                     // caster deleted and not required adding scAura
                break;

            bool restart = false;
            AuraList& scAuras = caster->GetSingleCastAuras();
            for(AuraList::iterator itr = scAuras.begin(); itr != scAuras.end(); ++itr)
            {
                if( (*itr)->GetTarget() != Aur->GetTarget() &&
                    IsSingleTargetSpells((*itr)->GetSpellProto(),aurSpellInfo) )
                {
                    if ((*itr)->IsInUse())
                    {
                        sLog.outError("Aura (Spell %u Effect %u) is in process but attempt removed at aura (Spell %u Effect %u) adding, need add stack rule for IsSingleTargetSpell", (*itr)->GetId(), (*itr)->GetEffIndex(),Aur->GetId(), Aur->GetEffIndex());
                        continue;
                    }
                    (*itr)->GetTarget()->RemoveAura((*itr)->GetId(), (*itr)->GetEffIndex());
                    restart = true;
                    break;
                }
            }

            if(!restart)
            {
                // done
                scAuras.push_back(Aur);
                break;
            }
        }
        m_GiantLock.release();
    }

    // add aura, register in lists and arrays
    Aur->_AddAura(!(doubleMongoose && Aur->GetEffIndex() == 0));    // We should change slot only while processing the first effect of double mongoose
    m_Auras.insert(AuraMap::value_type(spellEffectPair(Aur->GetId(), Aur->GetEffIndex()), Aur));
    if (Aur->GetModifier()->m_auraname < TOTAL_AURAS)
    {
        m_modAuras[Aur->GetModifier()->m_auraname].push_back(Aur);
        if(Aur->GetSpellProto()->AuraInterruptFlags)
        {
            m_interruptableAuras.push_back(Aur);
            AddInterruptMask(Aur->GetSpellProto()->AuraInterruptFlags);
        }
        if((Aur->GetSpellProto()->Attributes & SPELL_ATTR_BREAKABLE_BY_DAMAGE)
            && (Aur->GetModifier()->m_auraname != SPELL_AURA_MOD_POSSESS)) //only dummy aura is breakable
        {
            m_ccAuras.push_back(Aur);
        }
    }

    Aur->ApplyModifier(true,true);

    uint32 id = Aur->GetId();
    if(spellmgr.GetSpellCustomAttr(id) & SPELL_ATTR_CU_LINK_AURA)
    {
        if(const std::vector<int32> *spell_triggered = spellmgr.GetSpellLinked(id + SPELL_LINK_AURA))
            for(std::vector<int32>::const_iterator itr = spell_triggered->begin(); itr != spell_triggered->end(); ++itr)
                if(*itr < 0)
                    ApplySpellImmune(id, IMMUNITY_ID, -(*itr), true);
                else if(Unit* caster = Aur->GetCaster())
                    caster->AddAura(*itr, this);
    }

    return true;
}

void Unit::RemoveRankAurasDueToSpell(uint32 spellId)
{
    SpellEntry const *spellInfo = spellmgr.LookupSpell(spellId);
    if(!spellInfo)
        return;

    AuraMap::iterator i,next;
    for (i = m_Auras.begin(); i != m_Auras.end(); i = next)
    {
        next = i;
        ++next;
        uint32 i_spellId = (*i).second->GetId();
        if((*i).second && i_spellId && i_spellId != spellId)
        {
            if(spellmgr.IsRankSpellDueToSpell(spellInfo,i_spellId))
            {
                RemoveAurasDueToSpell(i_spellId);

                if( m_Auras.empty() )
                    break;
                else
                    next =  m_Auras.begin();
            }
        }
    }
}

bool Unit::RemoveNoStackAurasDueToAura(Aura *Aur)
{
    if (!Aur)
        return false;

    SpellEntry const* spellProto = Aur->GetSpellProto();
    if (!spellProto)
        return false;

    uint32 spellId = Aur->GetId();
    uint32 effIndex = Aur->GetEffIndex();

    SpellSpecific spellId_spec = GetSpellSpecific(spellId);

    AuraMap::iterator i,next;
    for (i = m_Auras.begin(); i != m_Auras.end(); i = next)
    {
        next = i;
        ++next;
        if (!(*i).second) continue;

        SpellEntry const* i_spellProto = (*i).second->GetSpellProto();

        if (!i_spellProto)
            continue;

        uint32 i_spellId = i_spellProto->Id;

        if (spellId==i_spellId)
            continue;

        if(IsPassiveSpell(i_spellId))
        {
            if(IsPassiveStackableSpell(i_spellId))
                continue;

            // passive non-stackable spells not stackable only with another rank of same spell
            if (!spellmgr.IsRankSpellDueToSpell(spellProto, i_spellId))
                continue;
        }

        uint32 i_effIndex = (*i).second->GetEffIndex();

        bool is_triggered_by_spell = false;
        // prevent triggered aura of removing aura that triggered it
        for(int j = 0; j < 3; ++j)
            if (i_spellProto->EffectTriggerSpell[j] == spellProto->Id)
                is_triggered_by_spell = true;
        if (is_triggered_by_spell) continue;

        for(int j = 0; j < 3; ++j)
        {
            // prevent remove dummy triggered spells at next effect aura add
            switch(spellProto->Effect[j])                   // main spell auras added added after triggered spell
            {
                case SPELL_EFFECT_DUMMY:
                    switch(spellId)
                    {
                        case 5420: if(i_spellId==34123) is_triggered_by_spell = true; break;
                    }
                    break;
            }

            if(is_triggered_by_spell)
                break;

            // prevent remove form main spell by triggered passive spells
            switch(i_spellProto->EffectApplyAuraName[j])    // main aura added before triggered spell
            {
                case SPELL_AURA_MOD_SHAPESHIFT:
                    switch(i_spellId)
                    {
                        case 24858: if(spellId==24905)                  is_triggered_by_spell = true; break;
                        case 33891: if(spellId==5420 || spellId==34123) is_triggered_by_spell = true; break;
                        case 34551: if(spellId==22688)                  is_triggered_by_spell = true; break;
                    }
                    break;
            }
        }

        if(!is_triggered_by_spell)
        {
            bool sameCaster = Aur->GetCasterGUID() == (*i).second->GetCasterGUID();
            if( spellmgr.IsNoStackSpellDueToSpell(spellId, i_spellId, sameCaster) )
            {
                //some spells should be not removed by lower rank of them (totem, paladin aura)
                if (!sameCaster
                    &&(spellProto->Effect[effIndex]==SPELL_EFFECT_APPLY_AREA_AURA_PARTY)
                    &&(spellProto->DurationIndex==21)
                    &&(spellmgr.IsRankSpellDueToSpell(spellProto, i_spellId))
                    &&(CompareAuraRanks(spellId, effIndex, i_spellId, i_effIndex) < 0))
                    return false;

                // Its a parent aura (create this aura in ApplyModifier)
                if ((*i).second->IsInUse())
                {
                    sLog.outError("Aura (Spell %u Effect %u) is in process but attempt removed at aura (Spell %u Effect %u) adding, need add stack rule for Unit::RemoveNoStackAurasDueToAura", i->second->GetId(), i->second->GetEffIndex(),Aur->GetId(), Aur->GetEffIndex());
                    continue;
                }

            uint64 caster = (*i).second->GetCasterGUID();
            // Remove all auras by aura caster
            for (uint8 a=0;a<3;++a)
            {
                spellEffectPair spair = spellEffectPair(i_spellId, a);
                for(AuraMap::iterator iter = m_Auras.lower_bound(spair); iter != m_Auras.upper_bound(spair);)
                {
                    if(iter->second->GetCasterGUID()==caster)
                    {
                        RemoveAura(iter, AURA_REMOVE_BY_STACK);
                        iter = m_Auras.lower_bound(spair);
                    }
                    else
                        ++iter;
                }
            }

                if( m_Auras.empty() )
                    break;
                else
                    next =  m_Auras.begin();
            }
        }
    }
    return true;
}

void Unit::RemoveAura(uint32 spellId, uint32 effindex, Aura* except)
{
    spellEffectPair spair = spellEffectPair(spellId, effindex);
    for(AuraMap::iterator iter = m_Auras.lower_bound(spair); iter != m_Auras.upper_bound(spair);)
    {
        if(iter->second!=except)
        {
            RemoveAura(iter);
            iter = m_Auras.lower_bound(spair);
        }
        else
            ++iter;
    }
}

void Unit::RemoveAurasByCasterSpell(uint32 spellId, uint32 effindex, uint64 casterGUID)
{
    spellEffectPair spair = spellEffectPair(spellId, effindex);
    for (AuraMap::iterator iter = m_Auras.lower_bound(spair); iter != m_Auras.upper_bound(spair);)
    {
        if (iter->second->GetCasterGUID() == casterGUID)
        {
            RemoveAura(iter);
            iter = m_Auras.upper_bound(spair);          // overwrite by more appropriate
        }
        else
            ++iter;
    }
}

void Unit::RemoveAurasByCasterSpell(uint32 spellId, uint64 casterGUID)
{
    for(int k = 0; k < 3; ++k)
    {
        spellEffectPair spair = spellEffectPair(spellId, k);
        for (AuraMap::iterator iter = m_Auras.lower_bound(spair); iter != m_Auras.upper_bound(spair);)
        {
            if (iter->second->GetCasterGUID() == casterGUID)
            {
                RemoveAura(iter);
                iter = m_Auras.upper_bound(spair);          // overwrite by more appropriate
            }
            else
                ++iter;
        }
    }
}

void Unit::SetAurasDurationByCasterSpell(uint32 spellId, uint64 casterGUID, int32 duration)
{
    for(uint8 i = 0; i < 3; ++i)
    {
        spellEffectPair spair = spellEffectPair(spellId, i);
        for(AuraMap::const_iterator itr = m_Auras.lower_bound(spair); itr != m_Auras.upper_bound(spair); ++itr)
        {
            if(itr->second->GetCasterGUID()==casterGUID)
            {
                itr->second->SetAuraDuration(duration);
                break;
            }
        }
    }
}

Aura* Unit::GetAuraByCasterSpell(uint32 spellId, uint64 casterGUID)
{
    // Returns first found aura from spell-use only in cases where effindex of spell doesn't matter!
    for(uint8 i = 0; i < 3; ++i)
    {
        spellEffectPair spair = spellEffectPair(spellId, i);
        for(AuraMap::const_iterator itr = m_Auras.lower_bound(spair); itr != m_Auras.upper_bound(spair); ++itr)
        {
            if(itr->second->GetCasterGUID()==casterGUID)
                return itr->second;
        }
    }
    return NULL;
}

Aura* Unit::GetAuraByCasterSpell(uint32 spellId, uint32 effIndex, uint64 casterGUID)
{
    // Returns first found aura from spell-use only in cases where effindex of spell doesn't matter!
    spellEffectPair spair = spellEffectPair(spellId, effIndex);
    for(AuraMap::const_iterator itr = m_Auras.lower_bound(spair); itr != m_Auras.upper_bound(spair); ++itr)
    {
        if(itr->second->GetCasterGUID() == casterGUID)
            return itr->second;
    }
    return NULL;
}

void Unit::RemoveAurasDueToSpellByDispel(uint32 spellId, uint64 casterGUID, Unit *dispeler)
{
    for (AuraMap::iterator iter = m_Auras.begin(); iter != m_Auras.end(); )
    {
        Aura *aur = iter->second;
        if (aur->GetId() == spellId && aur->GetCasterGUID() == casterGUID)
        {
            // Custom dispel case
            // Unstable Affliction
            if (aur->GetSpellProto()->SpellFamilyName == SPELLFAMILY_WARLOCK && (aur->GetSpellProto()->SpellFamilyFlags & 0x010000000000LL))
            {
                int32 damage = aur->GetModifier()->m_amount*9;
                uint64 caster_guid = aur->GetCasterGUID();

                // Remove aura
                RemoveAura(iter, AURA_REMOVE_BY_DISPEL);

                // backfire damage and silence
                dispeler->CastCustomSpell(dispeler, 31117, &damage, NULL, NULL, true, NULL, NULL,caster_guid);

                iter = m_Auras.begin();                     // iterator can be invalidate at cast if self-dispel
            }
            else
                RemoveAura(iter, AURA_REMOVE_BY_DISPEL);
        }
        else
            ++iter;
    }
}

void Unit::RemoveAurasDueToSpellBySteal(uint32 spellId, uint64 casterGUID, Unit *stealer)
{
    for (AuraMap::iterator iter = m_Auras.begin(); iter != m_Auras.end(); )
    {
        Aura *aur = iter->second;
        if (aur->GetId() == spellId && aur->GetCasterGUID() == casterGUID)
        {
            //int32 basePoints = aur->GetBasePoints();
            // construct the new aura for the attacker
            Aura * new_aur = CreateAura(aur->GetSpellProto(), aur->GetEffIndex(), NULL/*&basePoints*/, stealer);
            if(!new_aur)
                continue;

            // set its duration and maximum duration
            // max duration 2 minutes (in msecs)
            int32 dur = aur->GetAuraDuration();
            const int32 max_dur = 2*MINUTE*1000;
            new_aur->SetAuraMaxDuration( max_dur > dur ? dur : max_dur );
            new_aur->SetAuraDuration( max_dur > dur ? dur : max_dur );

            // Unregister _before_ adding to stealer
            aur->UnregisterSingleCastAura();
            // strange but intended behaviour: Stolen single target auras won't be treated as single targeted
            new_aur->SetIsSingleTarget(false);
            // add the new aura to stealer
            stealer->AddAura(new_aur);
            // Remove aura as dispel
            if (iter->second->GetStackAmount() > 1) {
                // reapply modifier with reduced stack amount
                iter->second->ApplyModifier(false, true);
                iter->second->SetStackAmount(iter->second->GetStackAmount() - 1);
                iter->second->ApplyModifier(true, true);
                iter->second->UpdateSlotCounterAndDuration();
                ++iter;
            }
            else
                RemoveAura(iter, AURA_REMOVE_BY_DISPEL);
        }
        else
            ++iter;
    }
}

void Unit::RemoveAurasDueToSpellByCancel(uint32 spellId)
{
    for (AuraMap::iterator iter = m_Auras.begin(); iter != m_Auras.end(); )
    {
        if (iter->second->GetId() == spellId)
            RemoveAura(iter, AURA_REMOVE_BY_CANCEL);
        else
            ++iter;
    }
}

void Unit::RemoveAurasWithAttribute(uint32 flags)
{
    for (AuraMap::iterator iter = m_Auras.begin(); iter != m_Auras.end();)
    {
        SpellEntry const *spell = iter->second->GetSpellProto();
        if (spell->Attributes & flags)
            RemoveAura(iter);
        else
            ++iter;
    }
}

void Unit::RemoveAurasWithCustomAttribute(uint32 flags)
{
    for (AuraMap::iterator iter = m_Auras.begin(); iter != m_Auras.end();) {
        SpellEntry const *spell = iter->second->GetSpellProto();
        if (spellmgr.GetSpellCustomAttr(spell->Id) & flags)
            RemoveAura(iter);
        else
            ++iter;
    }
}

void Unit::RemoveAurasWithDispelType( DispelType type )
{
    // Create dispel mask by dispel type
    uint32 dispelMask = GetDispellMask(type);
    // Dispel all existing auras vs current dispel type
    for(AuraMap::iterator itr = m_Auras.begin(); itr != m_Auras.end(); )
    {
        SpellEntry const* spell = itr->second->GetSpellProto();
        if( (1<<spell->Dispel) & dispelMask )
        {
            // Dispel aura
            RemoveAurasDueToSpell(spell->Id);
            itr = m_Auras.begin();
        }
        else
            ++itr;
    }
}

bool Unit::RemoveAurasWithSpellFamily(uint32 spellFamilyName, uint8 count, bool withPassive)
{
    uint8 myCount = count;
    bool ret = false;
    for(AuraMap::iterator itr = m_Auras.begin(); itr != m_Auras.end() && myCount > 0; )
    {
        SpellEntry const* spell = itr->second->GetSpellProto();
        if (spell->SpellFamilyName == spellFamilyName && IsPositiveSpell(spell->Id))
        {
            if (IsPassiveSpell(spell->Id) && !withPassive)
                ++itr;
            else {
                RemoveAurasDueToSpell(spell->Id);
                itr = m_Auras.begin();
                myCount--;
                ret = true;
            }
        }
        else
            ++itr;
    }
    
    return ret;
}

void Unit::RemoveSingleAuraFromStackByDispel(uint32 spellId)
{
    for (AuraMap::iterator iter = m_Auras.begin(); iter != m_Auras.end(); )
    {
        Aura *aur = iter->second;
        if (aur->GetId() == spellId)
        {
            if(iter->second->GetStackAmount() > 1)
            {
                // reapply modifier with reduced stack amount
                iter->second->ApplyModifier(false,true);
                iter->second->SetStackAmount(iter->second->GetStackAmount()-1);
                iter->second->ApplyModifier(true,true);

                iter->second->UpdateSlotCounterAndDuration();
                return; // not remove aura if stack amount > 1
            }
            else
                RemoveAura(iter,AURA_REMOVE_BY_DISPEL);
        }
        else
            ++iter;
    }
}

void Unit::RemoveSingleAuraFromStack(uint32 spellId, uint32 effindex)
{
    AuraMap::iterator iter = m_Auras.find(spellEffectPair(spellId, effindex));
    if(iter != m_Auras.end())
    {
        if(iter->second->GetStackAmount() > 1)
        {
            // reapply modifier with reduced stack amount
            iter->second->ApplyModifier(false,true);
            iter->second->SetStackAmount(iter->second->GetStackAmount()-1);
            iter->second->ApplyModifier(true,true);

            iter->second->UpdateSlotCounterAndDuration();
            return; // not remove aura if stack amount > 1
        }
        RemoveAura(iter);
    }
}

void Unit::RemoveAurasDueToSpell(uint32 spellId, Aura* except)
{
    for (int i = 0; i < 3; ++i)
        RemoveAura(spellId,i,except);
}

void Unit::RemoveAurasDueToItemSpell(Item* castItem,uint32 spellId)
{
    for (int k=0; k < 3; ++k)
    {
        spellEffectPair spair = spellEffectPair(spellId, k);
        for (AuraMap::iterator iter = m_Auras.lower_bound(spair); iter != m_Auras.upper_bound(spair);)
        {
            if (iter->second->GetCastItemGUID() == castItem->GetGUID())
            {
                RemoveAura(iter);
                iter = m_Auras.upper_bound(spair);          // overwrite by more appropriate
            }
            else
                ++iter;
        }
    }
}

void Unit::RemoveNotOwnSingleTargetAuras()
{
    m_GiantLock.acquire();
    // single target auras from other casters
    for (AuraMap::iterator iter = m_Auras.begin(); iter != m_Auras.end(); )
    {
        if (iter->second->GetCasterGUID()!=GetGUID() && IsSingleTargetSpell(iter->second->GetSpellProto()))
            RemoveAura(iter);
        else
            ++iter;
    }

    // single target auras at other targets
    AuraList& scAuras = GetSingleCastAuras();
    for (AuraList::iterator iter = scAuras.begin(); iter != scAuras.end(); )
    {
        Aura* aur = *iter;
        ++iter;
        if (aur->GetTarget()!=this)
        {
            uint32 removedAuras = m_removedAurasCount;
            aur->GetTarget()->RemoveAura( aur->GetId(),aur->GetEffIndex() );
            if (m_removedAurasCount > removedAuras + 1)
                iter = scAuras.begin();
        }
    }
    m_GiantLock.release();
}

void Unit::RemoveAura(AuraMap::iterator &i, AuraRemoveMode mode)
{
    Aura* Aur = i->second;

    // if unit currently update aura list then make safe update iterator shift to next
    if (m_AurasUpdateIterator == i)
        ++m_AurasUpdateIterator;

    // some ShapeshiftBoosts at remove trigger removing other auras including parent Shapeshift aura
    // remove aura from list before to prevent deleting it before
    m_Auras.erase(i);
    ++m_removedAurasCount;

    SpellEntry const* AurSpellInfo = Aur->GetSpellProto();
    Unit* caster = NULL;
    Aur->UnregisterSingleCastAura();

    // remove from list before mods removing (prevent cyclic calls, mods added before including to aura list - use reverse order)
    if (Aur->GetModifier()->m_auraname < TOTAL_AURAS)
    {
        m_modAuras[Aur->GetModifier()->m_auraname].remove(Aur);

        if(Aur->GetSpellProto()->AuraInterruptFlags)
        {
            m_interruptableAuras.remove(Aur);
            UpdateInterruptMask();
        }

        if((Aur->GetSpellProto()->Attributes & SPELL_ATTR_BREAKABLE_BY_DAMAGE)
            && (Aur->GetModifier()->m_auraname != SPELL_AURA_MOD_POSSESS)) //only dummy aura is breakable
        {
            m_ccAuras.remove(Aur);
        }
    }
              
    // Set remove mode
    Aur->SetRemoveMode(mode);

    // Statue unsummoned at aura remove
    Totem* statue = NULL;
    bool channeled = false;
    if(Aur->GetAuraDuration() && !Aur->IsPersistent() && IsChanneledSpell(AurSpellInfo))
    {
        if(!caster)                                         // can be already located for IsSingleTargetSpell case
            caster = Aur->GetCaster();

        if(caster && caster->IsAlive())
        {
            // stop caster chanelling state
            if(caster->m_currentSpells[CURRENT_CHANNELED_SPELL]
                //prevent recurential call
                && caster->m_currentSpells[CURRENT_CHANNELED_SPELL]->getState() != SPELL_STATE_FINISHED)
            {
                if (caster==this || !IsAreaOfEffectSpell(AurSpellInfo))
                {
                    // remove auras only for non-aoe spells or when chanelled aura is removed
                    // because aoe spells don't require aura on target to continue
                    if (AurSpellInfo->EffectApplyAuraName[Aur->GetEffIndex()]!=SPELL_AURA_PERIODIC_DUMMY
                        && AurSpellInfo->EffectApplyAuraName[Aur->GetEffIndex()]!= SPELL_AURA_DUMMY)
                        //don't stop channeling of scripted spells (this is actually a hack)
                    {
                        caster->m_currentSpells[CURRENT_CHANNELED_SPELL]->cancel();
                        caster->m_currentSpells[CURRENT_CHANNELED_SPELL]=NULL;
            
                    }
                }

                if(caster->GetTypeId()==TYPEID_UNIT && (caster->ToCreature())->isTotem() && ((Totem*)caster)->GetTotemType()==TOTEM_STATUE)
                    statue = ((Totem*)caster);
            }

            // Unsummon summon as possessed creatures on spell cancel
            if(caster->GetTypeId() == TYPEID_PLAYER)
            {
                for(int i = 0; i < 3; ++i)
                {
                    if(AurSpellInfo->Effect[i] == SPELL_EFFECT_SUMMON &&
                        (AurSpellInfo->EffectMiscValueB[i] == SUMMON_TYPE_POSESSED ||
                         AurSpellInfo->EffectMiscValueB[i] == SUMMON_TYPE_POSESSED2 ||
                         AurSpellInfo->EffectMiscValueB[i] == SUMMON_TYPE_POSESSED3))
                    {
                        (caster->ToPlayer())->StopCastingCharm();
                        break;
                    }
                }
            }
        }
    }

    assert(!Aur->IsInUse());
    Aur->ApplyModifier(false,true);

    Aur->SetStackAmount(0);

    // set aura to be removed during unit::_updatespells
    m_removedAuras.push_back(Aur);

    Aur->_RemoveAura();

    bool stack = false;
    spellEffectPair spair = spellEffectPair(Aur->GetId(), Aur->GetEffIndex());
    for(AuraMap::const_iterator itr = GetAuras().lower_bound(spair); itr != GetAuras().upper_bound(spair); ++itr)
    {
        if (itr->second->GetCasterGUID()==GetGUID())
        {
            stack = true;
        }
    }
    if (!stack)
    {
        // Remove all triggered by aura spells vs unlimited duration
        Aur->CleanupTriggeredSpells();

        // Remove Linked Auras
        uint32 id = Aur->GetId();
        if(spellmgr.GetSpellCustomAttr(id) & SPELL_ATTR_CU_LINK_REMOVE)
        {
            if(const std::vector<int32> *spell_triggered = spellmgr.GetSpellLinked(-(int32)id))
                for(std::vector<int32>::const_iterator itr = spell_triggered->begin(); itr != spell_triggered->end(); ++itr)
                    if(*itr < 0)
                        RemoveAurasDueToSpell(-(*itr));
                    else if(Unit* caster = Aur->GetCaster())
                        CastSpell(this, *itr, true, 0, 0, caster->GetGUID());
        }
        if(spellmgr.GetSpellCustomAttr(id) & SPELL_ATTR_CU_LINK_AURA)
        {
            if(const std::vector<int32> *spell_triggered = spellmgr.GetSpellLinked(id + SPELL_LINK_AURA))
                for(std::vector<int32>::const_iterator itr = spell_triggered->begin(); itr != spell_triggered->end(); ++itr)
                    if(*itr < 0)
                        ApplySpellImmune(id, IMMUNITY_ID, -(*itr), false);
                    else
                        RemoveAurasDueToSpell(*itr);
        }
    }

    if(statue)
        statue->UnSummon();

    i = m_Auras.begin();
}

void Unit::RemoveAllAuras()
{
    while (!m_Auras.empty())
    {
        AuraMap::iterator iter = m_Auras.begin();
        RemoveAura(iter);
    }

    m_Auras.clear();
}

void Unit::RemoveAllAurasExcept(uint32 spellId)
{
    AuraMap::iterator iter = m_Auras.begin();
    while (iter != m_Auras.end())
    {
        if(!iter->second->GetId() == spellId)
            RemoveAura(iter);
        else 
            iter++;
    }
}

void Unit::RemoveArenaAuras(bool onleave)
{
    // in join, remove positive buffs, on end, remove negative
    // used to remove positive visible auras in arenas
    for(AuraMap::iterator iter = m_Auras.begin(); iter != m_Auras.end();)
    {
        if  (  !(iter->second->GetSpellProto()->AttributesEx4 & SPELL_ATTR_EX4_UNK21) // don't remove stances, shadowform, pally/hunter auras
            && !iter->second->IsPassive()                               // don't remove passive auras
            && (!(iter->second->GetSpellProto()->Attributes & SPELL_ATTR_EX2_PRESERVE_ENCHANT_IN_ARENA))
            && (!onleave || !iter->second->IsPositive())                // remove all buffs on enter, negative buffs on leave
            && (iter->second->IsPositive() || !(iter->second->GetSpellProto()->AttributesEx3 & SPELL_ATTR_EX3_DEATH_PERSISTENT)) //dont remove death persistent debuff such as deserter
            )
            RemoveAura(iter);
        else
            ++iter;
    }

    if (Player* plr = ToPlayer()) {
        if (Pet* pet = GetPet())
            pet->RemoveArenaAuras(onleave);
        else
            plr->RemoveAllCurrentPetAuras(); //still remove auras if the players hasnt called his pet yet
    }
}

void Unit::RemoveAllAurasOnDeath()
{
    // used just after dieing to remove all visible auras
    // and disable the mods for the passive ones
    for(AuraMap::iterator iter = m_Auras.begin(); iter != m_Auras.end();)
    {
        if (!iter->second->IsPassive() && !iter->second->IsDeathPersistent())
            RemoveAura(iter, AURA_REMOVE_BY_DEATH);
        else
            ++iter;
    }
}

void Unit::DelayAura(uint32 spellId, uint32 effindex, int32 delaytime)
{
    AuraMap::const_iterator iter = m_Auras.find(spellEffectPair(spellId, effindex));
    if (iter != m_Auras.end())
    {
        if (iter->second->GetAuraDuration() < delaytime)
            iter->second->SetAuraDuration(0);
        else
            iter->second->SetAuraDuration(iter->second->GetAuraDuration() - delaytime);
        iter->second->UpdateAuraDuration();
    }
}

void Unit::_RemoveAllAuraMods()
{
    for (AuraMap::const_iterator i = m_Auras.begin(); i != m_Auras.end(); ++i)
    {
        (*i).second->ApplyModifier(false);
    }
}

void Unit::_ApplyAllAuraMods()
{
    for (AuraMap::const_iterator i = m_Auras.begin(); i != m_Auras.end(); ++i)
    {
        (*i).second->ApplyModifier(true);
    }
}

Aura* Unit::GetAura(uint32 spellId, uint32 effindex)
{
    AuraMap::const_iterator iter = m_Auras.find(spellEffectPair(spellId, effindex));
    if (iter != m_Auras.end())
        return iter->second;
    return NULL;
}

void Unit::AddDynObject(DynamicObject* dynObj)
{
    m_dynObjGUIDs.push_back(dynObj->GetGUID());
}

void Unit::RemoveDynObject(uint32 spellid)
{
    if(m_dynObjGUIDs.empty())
        return;
    for (DynObjectGUIDs::iterator i = m_dynObjGUIDs.begin(); i != m_dynObjGUIDs.end();)
    {
        DynamicObject* dynObj = ObjectAccessor::GetDynamicObject(*this, *i);
        if(!dynObj) // may happen if a dynobj is removed when grid unload
        {
            i = m_dynObjGUIDs.erase(i);
        }
        else if(spellid == 0 || dynObj->GetSpellId() == spellid)
        {
            dynObj->Delete();
            i = m_dynObjGUIDs.erase(i);
        }
        else
            ++i;
    }
}

void Unit::RemoveAllDynObjects()
{
    while(!m_dynObjGUIDs.empty())
    {
        DynamicObject* dynObj = ObjectAccessor::GetDynamicObject(*this,*m_dynObjGUIDs.begin());
        if(dynObj)
            dynObj->Delete();
        m_dynObjGUIDs.erase(m_dynObjGUIDs.begin());
    }
}

DynamicObject * Unit::GetDynObject(uint32 spellId, uint32 effIndex)
{
    for (DynObjectGUIDs::iterator i = m_dynObjGUIDs.begin(); i != m_dynObjGUIDs.end();)
    {
        DynamicObject* dynObj = ObjectAccessor::GetDynamicObject(*this, *i);
        if(!dynObj)
        {
            i = m_dynObjGUIDs.erase(i);
            continue;
        }

        if (dynObj->GetSpellId() == spellId && dynObj->GetEffIndex() == effIndex)
            return dynObj;
        ++i;
    }
    return NULL;
}

DynamicObject * Unit::GetDynObject(uint32 spellId)
{
    for (DynObjectGUIDs::iterator i = m_dynObjGUIDs.begin(); i != m_dynObjGUIDs.end();)
    {
        DynamicObject* dynObj = ObjectAccessor::GetDynamicObject(*this, *i);
        if(!dynObj)
        {
            i = m_dynObjGUIDs.erase(i);
            continue;
        }

        if (dynObj->GetSpellId() == spellId)
            return dynObj;
        ++i;
    }
    return NULL;
}

void Unit::AddGameObject(GameObject* gameObj)
{
    assert(gameObj && gameObj->GetOwnerGUID()==0);
    m_gameObj.push_back(gameObj);
    gameObj->SetOwnerGUID(GetGUID());
}

void Unit::RemoveGameObject(GameObject* gameObj, bool del)
{
    assert(gameObj && gameObj->GetOwnerGUID()==GetGUID());

    // GO created by some spell
    if ( GetTypeId()==TYPEID_PLAYER && gameObj->GetSpellId() )
    {
        SpellEntry const* createBySpell = spellmgr.LookupSpell(gameObj->GetSpellId());
        // Need activate spell use for owner
        if (createBySpell && createBySpell->Attributes & SPELL_ATTR_DISABLED_WHILE_ACTIVE)
            (this->ToPlayer())->SendCooldownEvent(createBySpell);
    }
    gameObj->SetOwnerGUID(0);
    m_gameObj.remove(gameObj);
    if(del)
    {
        gameObj->SetRespawnTime(0);
        gameObj->Delete();
    }
}

void Unit::RemoveGameObject(uint32 spellid, bool del)
{
    if(m_gameObj.empty())
        return;
    std::list<GameObject*>::iterator i, next;
    for (i = m_gameObj.begin(); i != m_gameObj.end(); i = next)
    {
        next = i;
        if(spellid == 0 || (*i)->GetSpellId() == spellid)
        {
            (*i)->SetOwnerGUID(0);
            if(del)
            {
                (*i)->SetRespawnTime(0);
                (*i)->Delete();
            }

            next = m_gameObj.erase(i);
        }
        else
            ++next;
    }
}

void Unit::RemoveAllGameObjects()
{
    // remove references to unit
    for(std::list<GameObject*>::iterator i = m_gameObj.begin(); i != m_gameObj.end();)
    {
        (*i)->SetOwnerGUID(0);
        (*i)->SetRespawnTime(0);
        (*i)->Delete();
        i = m_gameObj.erase(i);
    }
}

void Unit::SendSpellNonMeleeDamageLog(SpellNonMeleeDamage *log)
{
    WorldPacket data(SMSG_SPELLNONMELEEDAMAGELOG, (16+4+4+1+4+4+1+1+4+4+1)); // we guess size
    data.append(log->target->GetPackGUID());
    data.append(log->attacker->GetPackGUID());
    data << uint32(log->SpellID);
    data << uint32(log->damage);                             //damage amount
    data << uint8 (log->schoolMask);                         //damage school
    data << uint32(log->absorb);                             //AbsorbedDamage
    data << uint32(log->resist);                             //resist
    data << uint8 (log->physicalLog);                        // damsge type? flag
    data << uint8 (log->unused);                             //unused
    data << uint32(log->blocked);                            //blocked
    data << uint32(log->HitInfo);
    data << uint8 (0);                                       // flag to use extend data
    SendMessageToSet( &data, true );
}

void Unit::SendSpellNonMeleeDamageLog(Unit *target,uint32 SpellID,uint32 Damage, SpellSchoolMask damageSchoolMask,uint32 AbsorbedDamage, uint32 Resist,bool PhysicalDamage, uint32 Blocked, bool CriticalHit)
{
    WorldPacket data(SMSG_SPELLNONMELEEDAMAGELOG, (16+4+4+1+4+4+1+1+4+4+1)); // we guess size
    data.append(target->GetPackGUID());
    data.append(GetPackGUID());
    data << uint32(SpellID);
    data << uint32(Damage-AbsorbedDamage-Resist-Blocked);
    data << uint8(damageSchoolMask);                        // spell school
    data << uint32(AbsorbedDamage);                         // AbsorbedDamage
    data << uint32(Resist);                                 // resist
    data << uint8(PhysicalDamage);                          // if 1, then client show spell name (example: %s's ranged shot hit %s for %u school or %s suffers %u school damage from %s's spell_name
    data << uint8(0);                                       // unk isFromAura
    data << uint32(Blocked);                                // blocked
    data << uint32(CriticalHit ? 0x27 : 0x25);              // hitType, flags: 0x2 - SPELL_HIT_TYPE_CRIT, 0x10 - replace caster?
    data << uint8(0);                                       // isDebug?
    SendMessageToSet( &data, true );
}

void Unit::ProcDamageAndSpell(Unit *pVictim, uint32 procAttacker, uint32 procVictim, uint32 procExtra, uint32 amount, WeaponAttackType attType, SpellEntry const *procSpell, bool canTrigger)
{
     // Not much to do if no flags are set.
    if (procAttacker && canTrigger)
        ProcDamageAndSpellFor(false,pVictim,procAttacker, procExtra,attType, procSpell, amount);
    // Now go on with a victim's events'n'auras
    // Not much to do if no flags are set or there is no victim
    if(pVictim && pVictim->IsAlive() && procVictim)
        pVictim->ProcDamageAndSpellFor(true,this,procVictim, procExtra, attType, procSpell, amount);
}

void Unit::SendSpellMiss(Unit *target, uint32 spellID, SpellMissInfo missInfo)
{
    WorldPacket data(SMSG_SPELLLOGMISS, (4+8+1+4+8+1));
    data << uint32(spellID);
    data << uint64(GetGUID());
    data << uint8(0);                                       // can be 0 or 1
    data << uint32(1);                                      // target count
    // for(i = 0; i < target count; ++i)
    data << uint64(target->GetGUID());                      // target GUID
    data << uint8(missInfo);
    // end loop
    SendMessageToSet(&data, true);
}

void Unit::SendAttackStateUpdate(CalcDamageInfo *damageInfo)
{
    WorldPacket data(SMSG_ATTACKERSTATEUPDATE, (16+84));    // we guess size
    data << (uint32)damageInfo->HitInfo;
    data.append(GetPackGUID());
    data.append(damageInfo->target->GetPackGUID());
    data << (uint32)(damageInfo->damage);     // Full damage

    data << (uint8)1;                         // Sub damage count
    //===  Sub damage description
    data << (uint32)(damageInfo->damageSchoolMask); // School of sub damage
    data << (float)damageInfo->damage;        // sub damage
    data << (uint32)damageInfo->damage;       // Sub Damage
    data << (uint32)damageInfo->absorb;       // Absorb
    data << (uint32)damageInfo->resist;       // Resist
    //=================================================
    data << (uint32)damageInfo->TargetState;
    data << (uint32)0;
    data << (uint32)0;
    data << (uint32)damageInfo->blocked_amount;
    SendMessageToSet( &data, true );/**/
}

void Unit::SendAttackStateUpdate(uint32 HitInfo, Unit *target, uint8 SwingType, SpellSchoolMask damageSchoolMask, uint32 Damage, uint32 AbsorbDamage, uint32 Resist, VictimState TargetState, uint32 BlockedAmount)
{
    WorldPacket data(SMSG_ATTACKERSTATEUPDATE, (16+45));    // we guess size
    data << (uint32)HitInfo;
    data.append(GetPackGUID());
    data.append(target->GetPackGUID());
    data << (uint32)(Damage-AbsorbDamage-Resist-BlockedAmount);

    data << (uint8)SwingType;                               // count?

    // for(i = 0; i < SwingType; ++i)
    data << (uint32)damageSchoolMask;
    data << (float)(Damage-AbsorbDamage-Resist-BlockedAmount);
    // still need to double check damage
    data << (uint32)(Damage-AbsorbDamage-Resist-BlockedAmount);
    data << (uint32)AbsorbDamage;
    data << (uint32)Resist;
    // end loop

    data << (uint32)TargetState;

    if( AbsorbDamage == 0 )                                 //also 0x3E8 = 0x3E8, check when that happens
        data << (uint32)0;
    else
        data << (uint32)-1;

    data << (uint32)0;
    data << (uint32)BlockedAmount;

    SendMessageToSet( &data, true );
}

bool Unit::HandleHasteAuraProc(Unit *pVictim, uint32 damage, Aura* triggeredByAura, SpellEntry const * procSpell, uint32 /*procFlag*/, uint32 /*procEx*/, uint32 cooldown)
{
    SpellEntry const *hasteSpell = triggeredByAura->GetSpellProto();

    Item* castItem = triggeredByAura->GetCastItemGUID() && GetTypeId()==TYPEID_PLAYER
        ? (this->ToPlayer())->GetItemByGuid(triggeredByAura->GetCastItemGUID()) : NULL;

    uint32 triggered_spell_id = 0;
    Unit* target = pVictim;
    int32 basepoints0 = 0;

    switch(hasteSpell->SpellFamilyName)
    {
        case SPELLFAMILY_ROGUE:
        {
            switch(hasteSpell->Id)
            {
                // Blade Flurry
                case 13877:
                case 33735:
                {
                    target = SelectNearbyTarget();
                    if(!target)
                        return false;
                    basepoints0 = damage;
                    triggered_spell_id = 22482;
                    break;
                }
            }
            break;
        }
    }

    // processed charge only counting case
    if(!triggered_spell_id)
        return true;

    SpellEntry const* triggerEntry = spellmgr.LookupSpell(triggered_spell_id);

    if(!triggerEntry)
    {
        sLog.outError("Unit::HandleHasteAuraProc: Spell %u have not existed triggered spell %u",hasteSpell->Id,triggered_spell_id);
        return false;
    }

    // default case
    if(!target || target!=this && !target->IsAlive())
        return false;

    if( cooldown && GetTypeId()==TYPEID_PLAYER && (this->ToPlayer())->HasSpellCooldown(triggered_spell_id))
        return false;

    if(basepoints0)
        CastCustomSpell(target,triggered_spell_id,&basepoints0,NULL,NULL,true,castItem,triggeredByAura);
    else
        CastSpell(target,triggered_spell_id,true,castItem,triggeredByAura);

    if( cooldown && GetTypeId()==TYPEID_PLAYER )
        (this->ToPlayer())->AddSpellCooldown(triggered_spell_id,0,time(NULL) + cooldown);

    return true;
}

bool Unit::HandleDummyAuraProc(Unit *pVictim, uint32 damage, Aura* triggeredByAura, SpellEntry const * procSpell, uint32 procFlag, uint32 procEx, uint32 cooldown)
{
    SpellEntry const *dummySpell = triggeredByAura->GetSpellProto ();
    uint32 effIndex = triggeredByAura->GetEffIndex ();

    Item* castItem = triggeredByAura->GetCastItemGUID() && GetTypeId()==TYPEID_PLAYER
        ? (this->ToPlayer())->GetItemByGuid(triggeredByAura->GetCastItemGUID()) : NULL;

    uint32 triggered_spell_id = 0;
    Unit* target = pVictim;
    int32 basepoints0 = 0;

    switch(dummySpell->SpellFamilyName)
    {
        case SPELLFAMILY_GENERIC:
        {
            switch (dummySpell->Id)
            {
                // Eye of Eye
                case 9799:
                case 25988:
                {
                    // prevent damage back from weapon special attacks
                    if (!procSpell || procSpell->DmgClass != SPELL_DAMAGE_CLASS_MAGIC )
                        return false;

                    // return damage % to attacker but < 50% own total health
                    basepoints0 = triggeredByAura->GetModifier()->m_amount*int32(damage)/100;
                    if(basepoints0 > GetMaxHealth()/2)
                        basepoints0 = GetMaxHealth()/2;

                    triggered_spell_id = 25997;
                    break;
                }
                // Sweeping Strikes
                case 12328:
                case 18765:
                case 35429:
                {
                    // prevent chain of triggered spell from same triggered spell
                    if(procSpell && (procSpell->Id==12723 || procSpell->Id==1680 || procSpell->Id==25231))
                        return false;
                        
                    if (procSpell && procSpell->SpellFamilyFlags & 0x420400000LL)    // Execute && Whirlwind && Cleave
                        return false;

                    target = SelectNearbyTarget();
                    if(!target)
                        return false;

                    triggered_spell_id = 12723;
                    basepoints0 = damage;
                    break;
                }
                // Sword Specialization: shouldn't proc from same spell or from windfury
                case 12281:
                case 12812:
                case 12813:
                case 12814:
                case 12815:
                case 13960:
                case 13961:
                case 13962:
                case 13963:
                case 13964:
                {
                    // Sword Spec
                    if (procSpell && procSpell->SpellIconID == 1462)
                        return false;
                        
                    // Windfury
                    if (procSpell && procSpell->SpellIconID == 1397)
                        return false;
                }
                // Unstable Power
                case 24658:
                {
                    if (!procSpell || procSpell->Id == 24659)
                        return false;
                    // Need remove one 24659 aura
                    RemoveSingleAuraFromStack(24659, 0);
                    RemoveSingleAuraFromStack(24659, 1);
                    return true;
                }
                // Restless Strength
                case 24661:
                {
                    // Need remove one 24662 aura
                    RemoveSingleAuraFromStack(24662, 0);
                    return true;
                }
                // Adaptive Warding (Frostfire Regalia set)
                case 28764:
                {
                    if(!procSpell)
                        return false;

                    // find Mage Armor
                    bool found = false;
                    AuraList const& mRegenInterupt = GetAurasByType(SPELL_AURA_MOD_MANA_REGEN_INTERRUPT);
                    for(AuraList::const_iterator iter = mRegenInterupt.begin(); iter != mRegenInterupt.end(); ++iter)
                    {
                        if(SpellEntry const* iterSpellProto = (*iter)->GetSpellProto())
                        {
                            if(iterSpellProto->SpellFamilyName==SPELLFAMILY_MAGE && (iterSpellProto->SpellFamilyFlags & 0x10000000))
                            {
                                found=true;
                                break;
                            }
                        }
                    }
                    if(!found)
                        return false;

                    switch(GetFirstSchoolInMask(GetSpellSchoolMask(procSpell)))
                    {
                        case SPELL_SCHOOL_NORMAL:
                        case SPELL_SCHOOL_HOLY:
                            return false;                   // ignored
                        case SPELL_SCHOOL_FIRE:   triggered_spell_id = 28765; break;
                        case SPELL_SCHOOL_NATURE: triggered_spell_id = 28768; break;
                        case SPELL_SCHOOL_FROST:  triggered_spell_id = 28766; break;
                        case SPELL_SCHOOL_SHADOW: triggered_spell_id = 28769; break;
                        case SPELL_SCHOOL_ARCANE: triggered_spell_id = 28770; break;
                        default:
                            return false;
                    }

                    target = this;
                    break;
                }
                // Obsidian Armor (Justice Bearer`s Pauldrons shoulder)
                case 27539:
                {
                    if(!procSpell)
                        return false;

                    switch(GetFirstSchoolInMask(GetSpellSchoolMask(procSpell)))
                    {
                        case SPELL_SCHOOL_NORMAL:
                            return false;                   // ignore
                        case SPELL_SCHOOL_HOLY:   triggered_spell_id = 27536; break;
                        case SPELL_SCHOOL_FIRE:   triggered_spell_id = 27533; break;
                        case SPELL_SCHOOL_NATURE: triggered_spell_id = 27538; break;
                        case SPELL_SCHOOL_FROST:  triggered_spell_id = 27534; break;
                        case SPELL_SCHOOL_SHADOW: triggered_spell_id = 27535; break;
                        case SPELL_SCHOOL_ARCANE: triggered_spell_id = 27540; break;
                        default:
                            return false;
                    }

                    target = this;
                    break;
                }
                // Mana Leech (Passive) (Priest Pet Aura)
                case 28305:
                {
                    // Cast on owner
                    target = GetOwner();
                    if(!target)
                        return false;

                    basepoints0 = int32(damage * 2.5f);     // manaregen
                    triggered_spell_id = 34650;
                    break;
                }
                // Mark of Malice
                case 33493:
                {
                    // Cast finish spell at last charge
                    if (triggeredByAura->m_procCharges > 1)
                        return false;

                    target = this;
                    triggered_spell_id = 33494;
                    break;
                }
                // Twisted Reflection (boss spell)
                case 21063:
                    triggered_spell_id = 21064;
                    break;
                // Vampiric Aura (boss spell)
                case 38196:
                {
                    basepoints0 = 3 * damage;               // 300%
                    if (basepoints0 < 0)
                        return false;

                    triggered_spell_id = 31285;
                    target = this;
                    break;
                }
                // Aura of Madness (Darkmoon Card: Madness trinket)
                //=====================================================
                // 39511 Sociopath: +35 strength (Paladin, Rogue, Druid, Warrior)
                // 40997 Delusional: +70 attack power (Rogue, Hunter, Paladin, Warrior, Druid)
                // 40998 Kleptomania: +35 agility (Warrior, Rogue, Paladin, Hunter, Druid)
                // 40999 Megalomania: +41 damage/healing (Druid, Shaman, Priest, Warlock, Mage, Paladin)
                // 41002 Paranoia: +35 spell/melee/ranged crit strike rating (All classes)
                // 41005 Manic: +35 haste (spell, melee and ranged) (All classes)
                // 41009 Narcissism: +35 intellect (Druid, Shaman, Priest, Warlock, Mage, Paladin, Hunter)
                // 41011 Martyr Complex: +35 stamina (All classes)
                // 41406 Dementia: Every 5 seconds either gives you +5% damage/healing. (Druid, Shaman, Priest, Warlock, Mage, Paladin)
                // 41409 Dementia: Every 5 seconds either gives you -5% damage/healing. (Druid, Shaman, Priest, Warlock, Mage, Paladin)
                case 39446:
                {
                    if(GetTypeId() != TYPEID_PLAYER || !this->IsAlive())
                        return false;

                    // Select class defined buff
                    switch (getClass())
                    {
                        case CLASS_PALADIN:                 // 39511,40997,40998,40999,41002,41005,41009,41011,41409
                        case CLASS_DRUID:                   // 39511,40997,40998,40999,41002,41005,41009,41011,41409
                        {
                            uint32 RandomSpell[]={39511,40997,40998,40999,41002,41005,41009,41011,41409};
                            triggered_spell_id = RandomSpell[ GetMap()->irand(0, sizeof(RandomSpell)/sizeof(uint32) - 1) ];
                            break;
                        }
                        case CLASS_ROGUE:                   // 39511,40997,40998,41002,41005,41011
                        case CLASS_WARRIOR:                 // 39511,40997,40998,41002,41005,41011
                        {
                            uint32 RandomSpell[]={39511,40997,40998,41002,41005,41011};
                            triggered_spell_id = RandomSpell[ GetMap()->irand(0, sizeof(RandomSpell)/sizeof(uint32) - 1) ];
                            break;
                        }
                        case CLASS_PRIEST:                  // 40999,41002,41005,41009,41011,41406,41409
                        case CLASS_SHAMAN:                  // 40999,41002,41005,41009,41011,41406,41409
                        case CLASS_MAGE:                    // 40999,41002,41005,41009,41011,41406,41409
                        case CLASS_WARLOCK:                 // 40999,41002,41005,41009,41011,41406,41409
                        {
                            uint32 RandomSpell[]={40999,41002,41005,41009,41011,41406,41409};
                            triggered_spell_id = RandomSpell[ GetMap()->irand(0, sizeof(RandomSpell)/sizeof(uint32) - 1) ];
                            break;
                        }
                        case CLASS_HUNTER:                  // 40997,40999,41002,41005,41009,41011,41406,41409
                        {
                            uint32 RandomSpell[]={40997,40999,41002,41005,41009,41011,41406,41409};
                            triggered_spell_id = RandomSpell[ GetMap()->irand(0, sizeof(RandomSpell)/sizeof(uint32) - 1) ];
                            break;
                        }
                        default:
                            return false;
                    }

                    target = this;
                    if (roll_chance_i(10))
                        (this->ToPlayer())->Say("This is Madness!", LANG_UNIVERSAL);
                    break;
                }
                /*
                // TODO: need find item for aura and triggered spells
                // Sunwell Exalted Caster Neck (??? neck)
                // cast ??? Light's Wrath if Exalted by Aldor
                // cast ??? Arcane Bolt if Exalted by Scryers*/
                case 46569:
                    return false;                           // disable for while
                /*
                {
                    if(GetTypeId() != TYPEID_PLAYER)
                        return false;

                    // Get Aldor reputation rank
                    if ((this->ToPlayer())->GetReputationRank(932) == REP_EXALTED)
                    {
                        target = this;
                        triggered_spell_id = ???
                        break;
                    }
                    // Get Scryers reputation rank
                    if ((this->ToPlayer())->GetReputationRank(934) == REP_EXALTED)
                    {
                        triggered_spell_id = ???
                        break;
                    }
                    return false;
                }/**/
                // Sunwell Exalted Caster Neck (Shattered Sun Pendant of Acumen neck)
                // cast 45479 Light's Wrath if Exalted by Aldor
                // cast 45429 Arcane Bolt if Exalted by Scryers
                case 45481:
                {
                    if(GetTypeId() != TYPEID_PLAYER)
                        return false;

                    // Get Aldor reputation rank
                    if ((this->ToPlayer())->GetReputationRank(932) == REP_EXALTED)
                    {
                        target = this;
                        triggered_spell_id = 45479;
                        break;
                    }
                    // Get Scryers reputation rank
                    if ((this->ToPlayer())->GetReputationRank(934) == REP_EXALTED)
                    {
                        if(this->IsFriendlyTo(target))
                            return false;

                        triggered_spell_id = 45429;
                        break;
                    }
                    return false;
                }
                // Sunwell Exalted Melee Neck (Shattered Sun Pendant of Might neck)
                // cast 45480 Light's Strength if Exalted by Aldor
                // cast 45428 Arcane Strike if Exalted by Scryers
                case 45482:
                {
                    if(GetTypeId() != TYPEID_PLAYER)
                        return false;

                    // Get Aldor reputation rank
                    if ((this->ToPlayer())->GetReputationRank(932) == REP_EXALTED)
                    {
                        target = this;
                        triggered_spell_id = 45480;
                        break;
                    }
                    // Get Scryers reputation rank
                    if ((this->ToPlayer())->GetReputationRank(934) == REP_EXALTED)
                    {
                        triggered_spell_id = 45428;
                        break;
                    }
                    return false;
                }
                // Sunwell Exalted Tank Neck (Shattered Sun Pendant of Resolve neck)
                // cast 45431 Arcane Insight if Exalted by Aldor
                // cast 45432 Light's Ward if Exalted by Scryers
                case 45483:
                {
                    if(GetTypeId() != TYPEID_PLAYER)
                        return false;

                    // Get Aldor reputation rank
                    if ((this->ToPlayer())->GetReputationRank(932) == REP_EXALTED)
                    {
                        target = this;
                        triggered_spell_id = 45432;
                        break;
                    }
                    // Get Scryers reputation rank
                    if ((this->ToPlayer())->GetReputationRank(934) == REP_EXALTED)
                    {
                        target = this;
                        triggered_spell_id = 45431;
                        break;
                    }
                    return false;
                }
                // Sunwell Exalted Healer Neck (Shattered Sun Pendant of Restoration neck)
                // cast 45478 Light's Salvation if Exalted by Aldor
                // cast 45430 Arcane Surge if Exalted by Scryers
                case 45484:
                {
                    if(GetTypeId() != TYPEID_PLAYER)
                        return false;

                    // Get Aldor reputation rank
                    if ((this->ToPlayer())->GetReputationRank(932) == REP_EXALTED)
                    {
                        target = this;
                        triggered_spell_id = 45478;
                        break;
                    }
                    // Get Scryers reputation rank
                    if ((this->ToPlayer())->GetReputationRank(934) == REP_EXALTED)
                    {
                        triggered_spell_id = 45430;
                        break;
                    }
                    return false;
                }
            }
            break;
        }
        case SPELLFAMILY_MAGE:
        {
            // Magic Absorption
            if (dummySpell->SpellIconID == 459)             // only this spell have SpellIconID == 459 and dummy aura
            {
                if (getPowerType() != POWER_MANA)
                    return false;

                // mana reward
                basepoints0 = (triggeredByAura->GetModifier()->m_amount * GetMaxPower(POWER_MANA) / 100);
                target = this;
                triggered_spell_id = 29442;
                break;
            }
            // Master of Elements
            if (dummySpell->SpellIconID == 1920)
            {
                if(!procSpell)
                    return false;

                // mana cost save
                basepoints0 = procSpell->manaCost * triggeredByAura->GetModifier()->m_amount/100;
                if( basepoints0 <=0 )
                    return false;

                target = this;
                triggered_spell_id = 29077;
                break;
            }
            // Incanter's Regalia set (add trigger chance to Mana Shield)
            if (dummySpell->SpellFamilyFlags & 0x0000000000008000LL)
            {
                if (GetTypeId() != TYPEID_PLAYER)
                    return false;
  
                if (!HasAura(37424))
                    return false;

                target = this;
                triggered_spell_id = 37436;
                break;
            }
            switch(dummySpell->Id)
            {
                // Ignite
                case 11119:
                case 11120:
                case 12846:
                case 12847:
                case 12848:
                {
                    if(procSpell && procSpell->Id == 34913) // No Ignite proc's from Molten Armor
                        return false;
                    
                    switch (dummySpell->Id)
                    {
                        case 11119: basepoints0 = int32(0.04f*damage); break;
                        case 11120: basepoints0 = int32(0.08f*damage); break;
                        case 12846: basepoints0 = int32(0.12f*damage); break;
                        case 12847: basepoints0 = int32(0.16f*damage); break;
                        case 12848: basepoints0 = int32(0.20f*damage); break;
                        default:
                            sLog.outError("Unit::HandleDummyAuraProc: non handled spell id: %u (IG)",dummySpell->Id);
                            return false;
                    }

                    AuraList const &DoT = pVictim->GetAurasByType(SPELL_AURA_PERIODIC_DAMAGE);
                    for (AuraList::const_iterator itr = DoT.begin(); itr != DoT.end(); ++itr)
                        if ((*itr)->GetId() == 12654 && (*itr)->GetCaster() == this)
                            if ((*itr)->GetBasePoints() > 0)
                                basepoints0 += int((*itr)->GetBasePoints()/((*itr)->GetTickNumber() + 1));

                    triggered_spell_id = 12654;
                    break;
                }
                // Combustion
                case 11129:
                {
                    //last charge and crit
                    if (triggeredByAura->m_procCharges <= 1 && (procEx & PROC_EX_CRITICAL_HIT) )
                    {
                        RemoveAurasDueToSpell(28682);       //-> remove Combustion auras
                        return true;                        // charge counting (will removed)
                    }

                    CastSpell(this, 28682, true, castItem, triggeredByAura);
                    return (procEx & PROC_EX_CRITICAL_HIT);// charge update only at crit hits, no hidden cooldowns
                }
            }
            break;
        }
        case SPELLFAMILY_WARRIOR:
        {
            // Retaliation
            if(dummySpell->SpellFamilyFlags==0x0000000800000000LL)
            {
                // check attack comes not from behind
                if (!HasInArc(M_PI, pVictim) || HasUnitState(UNIT_STAT_STUNNED))
                    return false;

                triggered_spell_id = 22858;
                break;
            }
            else if (dummySpell->SpellIconID == 1697)  // Second Wind
            {
                // only for spells and hit/crit (trigger start always) and not start from self casted spells (5530 Mace Stun Effect for example)
                if (procSpell == 0 || !(procEx & (PROC_EX_NORMAL_HIT|PROC_EX_CRITICAL_HIT)) || this == pVictim)
                    return false;
                // Need stun or root mechanic
                if (procSpell->Mechanic != MECHANIC_ROOT && procSpell->Mechanic != MECHANIC_STUN)
                {
                    int32 i;
                    for (i=0; i<3; i++)
                        if (procSpell->EffectMechanic[i] == MECHANIC_ROOT || procSpell->EffectMechanic[i] == MECHANIC_STUN)
                            break;
                    if (i == 3)
                        return false;
                }

                switch (dummySpell->Id)
                {
                    case 29838: triggered_spell_id=29842; break;
                    case 29834: triggered_spell_id=29841; break;
                    default:
                        sLog.outError("Unit::HandleDummyAuraProc: non handled spell id: %u (SW)",dummySpell->Id);
                    return false;
                }

                target = this;
                break;
            }
            break;
        }
        case SPELLFAMILY_WARLOCK:
        {
            // Seed of Corruption
            if (dummySpell->SpellFamilyFlags & 0x0000001000000000LL)
            {
                if(procSpell && procSpell->Id == 27285)
                    return false;
                Modifier* mod = triggeredByAura->GetModifier();
                // if damage is more than need or target die from damage deal finish spell
                if( mod->m_amount <= damage || GetHealth() <= damage )
                {
                    // remember guid before aura delete
                    uint64 casterGuid = triggeredByAura->GetCasterGUID();
                    // Remove our seed aura before casting
                    RemoveAurasByCasterSpell(triggeredByAura->GetId(),casterGuid);
                    // Cast finish spell
                    if(Unit* caster = GetUnit(*this, casterGuid))
                        caster->CastSpell(this, 27285, true, castItem);
                    return true;                            // no hidden cooldown
                }

                // Damage counting
                mod->m_amount-=damage;
                return true;
            }
            // Seed of Corruption (Mobs cast) - no die req
            if (dummySpell->SpellFamilyFlags == 0x00LL && dummySpell->SpellIconID == 1932)
            {
                // No Chain Procs
                if(procSpell && procSpell->Id == 32865 )
                    return false;

                Modifier* mod = triggeredByAura->GetModifier();
                // if damage is more than need deal finish spell
                if( mod->m_amount <= damage )
                {
                    // remember guid before aura delete
                    uint64 casterGuid = triggeredByAura->GetCasterGUID();

                    // Remove aura (before cast for prevent infinite loop handlers)
                    RemoveAurasDueToSpell(triggeredByAura->GetId());

                    // Cast finish spell (triggeredByAura already not exist!)
                    if(Unit* caster = GetUnit(*this, casterGuid))
                        caster->CastSpell(this, 32865, true, castItem);
                    return true;                            // no hidden cooldown
                }
                // Damage counting
                mod->m_amount-=damage;
                return true;
            }
            switch(dummySpell->Id)
            {
                // Nightfall
                case 18094:
                case 18095:
                {
                    target = this;
                    triggered_spell_id = 17941;
                    break;
                }
                //Soul Leech
                case 30293:
                case 30295:
                case 30296:
                {
                    // health
                    basepoints0 = int32(damage*triggeredByAura->GetModifier()->m_amount/100);
                    target = this;
                    triggered_spell_id = 30294;
                    break;
                }
                // Shadowflame (Voidheart Raiment set bonus)
                case 37377:
                {
                    triggered_spell_id = 37379;
                    break;
                }
                // Pet Healing (Corruptor Raiment or Rift Stalker Armor)
                case 37381:
                {
                    target = GetPet();
                    if(!target)
                        return false;

                    // heal amount
                    basepoints0 = damage * triggeredByAura->GetModifier()->m_amount/100;
                    triggered_spell_id = 37382;
                    break;
                }
                // Shadowflame Hellfire (Voidheart Raiment set bonus)
                case 39437:
                {
                    triggered_spell_id = 37378;
                    break;
                }
            }
            break;
        }
        case SPELLFAMILY_PRIEST:
        {
            // Vampiric Touch
            if( dummySpell->SpellFamilyFlags & 0x0000040000000000LL )
            {
                if(!pVictim || !pVictim->IsAlive())
                    return false;

                // pVictim is caster of aura
                if(triggeredByAura->GetCasterGUID() != pVictim->GetGUID())
                    return false;

                // energize amount
                basepoints0 = triggeredByAura->GetModifier()->m_amount*damage/100;
                pVictim->CastCustomSpell(pVictim,34919,&basepoints0,NULL,NULL,true,castItem,triggeredByAura);
                return true;                                // no hidden cooldown
            }
            switch(dummySpell->Id)
            {
                // Vampiric Embrace
                case 15286:
                {
                    if(!pVictim || !pVictim->IsAlive())
                        return false;

                    // pVictim is caster of aura
                    if(triggeredByAura->GetCasterGUID() != pVictim->GetGUID())
                        return false;

                    // heal amount
                    basepoints0 = triggeredByAura->GetModifier()->m_amount*damage/100;
                    pVictim->CastCustomSpell(pVictim,15290,&basepoints0,NULL,NULL,true,castItem,triggeredByAura);
                    return true;                                // no hidden cooldown
                }
                // Priest Tier 6 Trinket (Ashtongue Talisman of Acumen)
                case 40438:
                {
                    // Shadow Word: Pain
                    if( procSpell->SpellFamilyFlags & 0x0000000000008000LL )
                        triggered_spell_id = 40441;
                    // Renew
                    else if( procSpell->SpellFamilyFlags & 0x0000000000000040LL )
                        triggered_spell_id = 40440;
                    else
                        return false;

                    target = this;
                    break;
                }
                // Oracle Healing Bonus ("Garments of the Oracle" set)
                case 26169:
                {
                    // heal amount
                    basepoints0 = int32(damage * 10/100);
                    target = this;
                    triggered_spell_id = 26170;
                    break;
                }
                // Frozen Shadoweave (Shadow's Embrace set) warning! its not only priest set
                case 39372:
                {
                    if(!procSpell || (GetSpellSchoolMask(procSpell) & (SPELL_SCHOOL_MASK_FROST | SPELL_SCHOOL_MASK_SHADOW))==0 )
                        return false;

                    // heal amount
                    basepoints0 = int32(damage * 2 / 100);
                    target = this;
                    triggered_spell_id = 39373;
                    break;
                }
                // Vestments of Faith (Priest Tier 3) - 4 pieces bonus
                case 28809:
                {
                    triggered_spell_id = 28810;
                    break;
                }
            }
            break;
        }
        case SPELLFAMILY_DRUID:
        {
            switch(dummySpell->Id)
            {
                // Healing Touch (Dreamwalker Raiment set)
                case 28719:
                {
                    // mana back
                    basepoints0 = int32(procSpell->manaCost * 30 / 100);
                    target = this;
                    triggered_spell_id = 28742;
                    break;
                }
                // Healing Touch Refund (Idol of Longevity trinket)
                case 28847:
                {
                    target = this;
                    triggered_spell_id = 28848;
                    break;
                }
                // Mana Restore (Malorne Raiment set / Malorne Regalia set)
                case 37288:
                case 37295:
                {
                    target = this;
                    triggered_spell_id = 37238;
                    break;
                }
                // Druid Tier 6 Trinket
                case 40442:
                {
                    float  chance;

                    // Starfire
                    if( procSpell->SpellFamilyFlags & 0x0000000000000004LL )
                    {
                        triggered_spell_id = 40445;
                        chance = 25.f;
                    }
                    // Rejuvenation
                    else if( procSpell->SpellFamilyFlags & 0x0000000000000010LL )
                    {
                        triggered_spell_id = 40446;
                        chance = 25.f;
                    }
                    // Mangle (cat/bear)
                    else if( procSpell->SpellFamilyFlags & 0x0000044000000000LL )
                    {
                        triggered_spell_id = 40452;
                        chance = 40.f;
                    }
                    else
                        return false;

                    if (!roll_chance_f(chance))
                        return false;

                    target = this;
                    break;
                }
                // Maim Interrupt
                /*case 44835:
                {
                    // Deadly Interrupt Effect
                    //triggered_spell_id = 32747;
                    //break;
                }*/
            }
            break;
        }
        case SPELLFAMILY_ROGUE:
        {
            switch(dummySpell->Id)
            {
                // Deadly Throw Interrupt
                case 32748:
                {
                    // Prevent cast Deadly Throw Interrupt on self from last effect (apply dummy) of Deadly Throw
                    if(this == pVictim)
                        return false;

                    triggered_spell_id = 32747;
                    break;
                }
            }
            // Quick Recovery
            if( dummySpell->SpellIconID == 2116 )
            {
                if(!procSpell)
                    return false;

                // only rogue's finishing moves (maybe need additional checks)
                if( procSpell->SpellFamilyName!=SPELLFAMILY_ROGUE ||
                    (procSpell->SpellFamilyFlags & SPELLFAMILYFLAG_ROGUE__FINISHING_MOVE) == 0)
                    return false;

                // energy cost save
                basepoints0 = procSpell->manaCost * triggeredByAura->GetModifier()->m_amount/100;
                if(basepoints0 <= 0)
                    return false;

                target = this;
                triggered_spell_id = 31663;
                break;
            }
            break;
        }
        case SPELLFAMILY_HUNTER:
        {
            // Thrill of the Hunt
            if ( dummySpell->SpellIconID == 2236 )
            {
                if(!procSpell)
                    return false;

                // mana cost save
                basepoints0 = procSpell->manaCost * 40/100;
                if(basepoints0 <= 0)
                    return false;

                target = this;
                triggered_spell_id = 34720;
                break;
            }
            break;
        }
        case SPELLFAMILY_PALADIN:
        {
            // Seal of Righteousness - melee proc dummy
            if (dummySpell->SpellFamilyFlags&0x000000008000000LL && triggeredByAura->GetEffIndex()==0)
            {
                if(GetTypeId() != TYPEID_PLAYER)
                    return false;

                uint32 spellId;
                switch (triggeredByAura->GetId())
                {
                    case 21084: spellId = 25742; break;     // Rank 1
                    case 20287: spellId = 25740; break;     // Rank 2
                    case 20288: spellId = 25739; break;     // Rank 3
                    case 20289: spellId = 25738; break;     // Rank 4
                    case 20290: spellId = 25737; break;     // Rank 5
                    case 20291: spellId = 25736; break;     // Rank 6
                    case 20292: spellId = 25735; break;     // Rank 7
                    case 20293: spellId = 25713; break;     // Rank 8
                    case 27155: spellId = 27156; break;     // Rank 9
                    default:
                        sLog.outError("Unit::HandleDummyAuraProc: non handled possibly SoR (Id = %u)", triggeredByAura->GetId());
                        return false;
                }
                Item *item = (this->ToPlayer())->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
                float speed = (item ? item->GetProto()->Delay : BASE_ATTACK_TIME)/1000.0f;

                float damageBasePoints;
                if(item && item->GetProto()->InventoryType == INVTYPE_2HWEAPON)
                    // two hand weapon
                    damageBasePoints=1.20f*triggeredByAura->GetModifier()->m_amount * 1.2f * 1.03f * speed/100.0f + 1;
                else
                    // one hand weapon/no weapon
                    damageBasePoints=0.85f*ceil(triggeredByAura->GetModifier()->m_amount * 1.2f * 1.03f * speed/100.0f) - 1;

                int32 damagePoint = int32(damageBasePoints + 0.03f * (GetWeaponDamageRange(BASE_ATTACK,MINDAMAGE)+GetWeaponDamageRange(BASE_ATTACK,MAXDAMAGE))/2.0f) + 1;

                // apply damage bonuses manually
                if(damagePoint >= 0)
                    damagePoint = SpellDamageBonus(pVictim, dummySpell, damagePoint, SPELL_DIRECT_DAMAGE);

                CastCustomSpell(pVictim,spellId,&damagePoint,NULL,NULL,true,NULL, triggeredByAura);
                return true;                                // no hidden cooldown
            }
            // Seal of Blood do damage trigger
            if(dummySpell->SpellFamilyFlags & 0x0000040000000000LL)
            {
                switch(triggeredByAura->GetEffIndex())
                {
                    case 0:
                        triggered_spell_id = 31893;
                        break;
                    case 1:
                    {
                        // damage
                        damage += CalculateDamage(BASE_ATTACK, false) * 35 / 100; // add spell damage from prev effect (35%)
                        basepoints0 =  triggeredByAura->GetModifier()->m_amount * damage / 100;

                        target = this;

                        triggered_spell_id = 32221;
                        break;
                    }
                }
            }

            switch(dummySpell->Id)
            {
                // Holy Power (Redemption Armor set)
                case 28789:
                {
                    if(!pVictim)
                        return false;

                    // Set class defined buff
                    switch (pVictim->getClass())
                    {
                        case CLASS_PALADIN:
                        case CLASS_PRIEST:
                        case CLASS_SHAMAN:
                        case CLASS_DRUID:
                            triggered_spell_id = 28795;     // Increases the friendly target's mana regeneration by $s1 per 5 sec. for $d.
                            break;
                        case CLASS_MAGE:
                        case CLASS_WARLOCK:
                            triggered_spell_id = 28793;     // Increases the friendly target's spell damage and healing by up to $s1 for $d.
                            break;
                        case CLASS_HUNTER:
                        case CLASS_ROGUE:
                            triggered_spell_id = 28791;     // Increases the friendly target's attack power by $s1 for $d.
                            break;
                        case CLASS_WARRIOR:
                            triggered_spell_id = 28790;     // Increases the friendly target's armor
                            break;
                        default:
                            return false;
                    }
                    break;
                }
                //Seal of Vengeance
                case 31801:
                {
                    if(effIndex != 0)                       // effect 1,2 used by seal unleashing code
                        return false;

                    triggered_spell_id = 31803;
                    // On target with 5 stacks of Holy Vengeance direct damage is done
                    AuraList const& auras = pVictim->GetAurasByType(SPELL_AURA_PERIODIC_DAMAGE);
                    for(AuraList::const_iterator itr = auras.begin(); itr!=auras.end(); ++itr)
                    {
                        if((*itr)->GetId() == 31803 && (*itr)->GetCasterGUID() == this->GetGUID())
                        {
                            // 10% of tick done as direct damage
                            if ((*itr)->GetStackAmount() == 5)
                            {
                                int32 directDamage = SpellDamageBonus(pVictim,(*itr)->GetSpellProto(),(*itr)->GetModifierValuePerStack(),DOT)/2;
                                CastCustomSpell(pVictim, 42463, &directDamage,NULL,NULL,true,0,triggeredByAura);
                            }
                            break;
                        }
                    }
                    break;
                }
                // Spiritual Att.
                case 31785:
                case 33776:
                {
                    // if healed by another unit (pVictim)
                    if(this == pVictim)
                        return false;

                    // heal amount
                    basepoints0 = triggeredByAura->GetModifier()->m_amount*std::min(damage,GetMaxHealth() - GetHealth())/100;
                    target = this;

                    if(basepoints0)
                        triggered_spell_id = 31786;
                    break;
                }
                // Paladin Tier 6 Trinket (Ashtongue Talisman of Zeal)
                case 40470:
                {
                    if( !procSpell )
                        return false;

                    float  chance;

                    // Flash of light/Holy light
                    if( procSpell->SpellFamilyFlags & 0x00000000C0000000LL)
                    {
                        triggered_spell_id = 40471;
                        chance = 15.f;
                    }
                    // Judgement
                    else if( procSpell->SpellFamilyFlags & 0x0000000000800000LL )
                    {
                        triggered_spell_id = 40472;
                        chance = 50.f;
                    }
                    else
                        return false;

                    if (!roll_chance_f(chance))
                        return false;

                    break;
                }
            }
            break;
        }
        case SPELLFAMILY_SHAMAN:
        {
            switch(dummySpell->Id)
            {
                // Totemic Power (The Earthshatterer set)
                case 28823:
                {
                    if( !pVictim )
                        return false;

                    // Set class defined buff
                    switch (pVictim->getClass())
                    {
                        case CLASS_PALADIN:
                        case CLASS_PRIEST:
                        case CLASS_SHAMAN:
                        case CLASS_DRUID:
                            triggered_spell_id = 28824;     // Increases the friendly target's mana regeneration by $s1 per 5 sec. for $d.
                            break;
                        case CLASS_MAGE:
                        case CLASS_WARLOCK:
                            triggered_spell_id = 28825;     // Increases the friendly target's spell damage and healing by up to $s1 for $d.
                            break;
                        case CLASS_HUNTER:
                        case CLASS_ROGUE:
                            triggered_spell_id = 28826;     // Increases the friendly target's attack power by $s1 for $d.
                            break;
                        case CLASS_WARRIOR:
                            triggered_spell_id = 28827;     // Increases the friendly target's armor
                            break;
                        default:
                            return false;
                    }
                    break;
                }
                // Lesser Healing Wave (Totem of Flowing Water Relic)
                case 28849:
                {
                    target = this;
                    triggered_spell_id = 28850;
                    break;
                }
                // Windfury Weapon (Passive) 1-5 Ranks
                case 33757:
                {
                    if(GetTypeId()!=TYPEID_PLAYER)
                        return false;

                    if(!castItem || !castItem->IsEquipped())
                        return false;

                    if(triggeredByAura && castItem->GetGUID() != triggeredByAura->GetCastItemGUID())
                        return false;

                    // custom cooldown processing case
                    if( cooldown && (this->ToPlayer())->HasSpellCooldown(dummySpell->Id))
                        return false;

                    uint32 spellId;
                    switch (castItem->GetEnchantmentId(EnchantmentSlot(TEMP_ENCHANTMENT_SLOT)))
                    {
                        case 283: spellId = 33757; break;   //1 Rank
                        case 284: spellId = 33756; break;   //2 Rank
                        case 525: spellId = 33755; break;   //3 Rank
                        case 1669:spellId = 33754; break;   //4 Rank
                        case 2636:spellId = 33727; break;   //5 Rank
                        default:
                        {
                            sLog.outError("Unit::HandleDummyAuraProc: non handled item enchantment (rank?) %u for spell id: %u (Windfury)",
                                castItem->GetEnchantmentId(EnchantmentSlot(TEMP_ENCHANTMENT_SLOT)),dummySpell->Id);
                            return false;
                        }
                    }

                    SpellEntry const* windfurySpellEntry = spellmgr.LookupSpell(spellId);
                    if(!windfurySpellEntry)
                    {
                        sLog.outError("Unit::HandleDummyAuraProc: non existed spell id: %u (Windfury)",spellId);
                        return false;
                    }

                    int32 extra_attack_power = CalculateSpellDamage(windfurySpellEntry,0,windfurySpellEntry->EffectBasePoints[0],pVictim);

                    // Off-Hand case
                    if ( castItem->GetSlot() == EQUIPMENT_SLOT_OFFHAND )
                    {
                        // Value gained from additional AP
                        basepoints0 = int32(extra_attack_power/14.0f * GetAttackTime(OFF_ATTACK)/1000/2);
                        triggered_spell_id = 33750;
                    }
                    // Main-Hand case
                    else
                    {
                        // Value gained from additional AP
                        basepoints0 = int32(extra_attack_power/14.0f * GetAttackTime(BASE_ATTACK)/1000);
                        triggered_spell_id = 25504;
                    }

                    // apply cooldown before cast to prevent processing itself
                    if( cooldown )
                        (this->ToPlayer())->AddSpellCooldown(dummySpell->Id,0,time(NULL) + cooldown);

                    // Attack Twice
                    for ( uint32 i = 0; i<2; ++i )
                        CastCustomSpell(pVictim,triggered_spell_id,&basepoints0,NULL,NULL,true,castItem,triggeredByAura);

                    return true;
                }
                // Shaman Tier 6 Trinket
                case 40463:
                {
                    if( !procSpell )
                        return false;

                    float  chance;
                    if (procSpell->SpellFamilyFlags & 0x0000000000000001LL)
                    {
                        triggered_spell_id = 40465;         // Lightning Bolt
                        chance = 15.f;
                    }
                    else if (procSpell->SpellFamilyFlags & 0x0000000000000080LL)
                    {
                        triggered_spell_id = 40465;         // Lesser Healing Wave
                        chance = 10.f;
                    }
                    else if (procSpell->SpellFamilyFlags & 0x0000001000000000LL)
                    {
                        triggered_spell_id = 40466;         // Stormstrike
                        chance = 50.f;
                    }
                    else
                        return false;

                    if (!roll_chance_f(chance))
                        return false;

                    target = this;
                    break;
                }
            }

            // Earth Shield
            if(dummySpell->SpellFamilyFlags==0x40000000000LL)
            {
                // heal
                target = this;
                basepoints0 = triggeredByAura->GetModifier()->m_amount;
                if(Unit* caster = triggeredByAura->GetCaster())
                    basepoints0 = caster->SpellHealingBonus(triggeredByAura->GetSpellProto(), basepoints0, SPELL_DIRECT_DAMAGE, NULL);
                triggered_spell_id = 379;
                break;
            }
            // Lightning Overload
            if (dummySpell->SpellIconID == 2018)            // only this spell have SpellFamily Shaman SpellIconID == 2018 and dummy aura
            {
                if(!procSpell || GetTypeId() != TYPEID_PLAYER || !pVictim )
                    return false;

                // custom cooldown processing case
                if( cooldown && GetTypeId()==TYPEID_PLAYER && (this->ToPlayer())->HasSpellCooldown(dummySpell->Id))
                    return false;

                uint32 spellId = 0;
                // Every Lightning Bolt and Chain Lightning spell have duplicate vs half damage and zero cost
                switch (procSpell->Id)
                {
                    // Lightning Bolt
                    case   403: spellId = 45284; break;     // Rank  1
                    case   529: spellId = 45286; break;     // Rank  2
                    case   548: spellId = 45287; break;     // Rank  3
                    case   915: spellId = 45288; break;     // Rank  4
                    case   943: spellId = 45289; break;     // Rank  5
                    case  6041: spellId = 45290; break;     // Rank  6
                    case 10391: spellId = 45291; break;     // Rank  7
                    case 10392: spellId = 45292; break;     // Rank  8
                    case 15207: spellId = 45293; break;     // Rank  9
                    case 15208: spellId = 45294; break;     // Rank 10
                    case 25448: spellId = 45295; break;     // Rank 11
                    case 25449: spellId = 45296; break;     // Rank 12
                    // Chain Lightning
                    case   421: spellId = 45297; break;     // Rank  1
                    case   930: spellId = 45298; break;     // Rank  2
                    case  2860: spellId = 45299; break;     // Rank  3
                    case 10605: spellId = 45300; break;     // Rank  4
                    case 25439: spellId = 45301; break;     // Rank  5
                    case 25442: spellId = 45302; break;     // Rank  6
                    default:
                        sLog.outError("Unit::HandleDummyAuraProc: non handled spell id: %u (LO)", procSpell->Id);
                        return false;
                }
                // No thread generated mod
                SpellModifier *mod = new SpellModifier;
                mod->op = SPELLMOD_THREAT;
                mod->value = -100;
                mod->type = SPELLMOD_PCT;
                mod->spellId = dummySpell->Id;
                mod->effectId = 0;
                mod->lastAffected = NULL;
                mod->mask = 0x0000000000000003LL;
                mod->charges = 0;
                (this->ToPlayer())->AddSpellMod(mod, true);

                // Remove cooldown (Chain Lightning - have Category Recovery time)
                if (procSpell->SpellFamilyFlags & 0x0000000000000002LL)
                    (this->ToPlayer())->RemoveSpellCooldown(spellId);

                // Hmmm.. in most case spells already set half basepoints but...
                // Lightning Bolt (2-10 rank) have full basepoint and half bonus from level
                // As on wiki:
                // BUG: Rank 2 to 10 (and maybe 11) of Lightning Bolt will proc another Bolt with FULL damage (not halved). This bug is known and will probably be fixed soon.
                // So - no add changes :)
                CastSpell(pVictim, spellId, true, castItem, triggeredByAura);

                (this->ToPlayer())->AddSpellMod(mod, false);

                if( cooldown && GetTypeId()==TYPEID_PLAYER )
                    (this->ToPlayer())->AddSpellCooldown(dummySpell->Id,0,time(NULL) + cooldown);

                return true;
            }
            break;
        }
        case SPELLFAMILY_POTION:
        {
            if (dummySpell->Id == 17619)
            {
                if (procSpell->SpellFamilyName == SPELLFAMILY_POTION)
                {
                    for (uint8 i=0;i<3;i++)
                    {
                        if (procSpell->Effect[i]==SPELL_EFFECT_HEAL)
                        {
                            triggered_spell_id = 21399;
                        }
                        else if (procSpell->Effect[i]==SPELL_EFFECT_ENERGIZE)
                        {
                            triggered_spell_id = 21400;
                        }
                        else continue;
                        basepoints0 = CalculateSpellDamage(procSpell,i,procSpell->EffectBasePoints[i],this) * 0.4f;
                        CastCustomSpell(this,triggered_spell_id,&basepoints0,NULL,NULL,true,castItem,triggeredByAura);
                    }
                    return true;
                }
            }
        }
        default:
            break;
    }

    // processed charge only counting case
    if(!triggered_spell_id)
        return true;

    SpellEntry const* triggerEntry = spellmgr.LookupSpell(triggered_spell_id);

    if(!triggerEntry)
    {
        sLog.outError("Unit::HandleDummyAuraProc: Spell %u have not existed triggered spell %u",dummySpell->Id,triggered_spell_id);
        return false;
    }

    // default case
    if(!target || target!=this && !target->IsAlive())
        return false;

    if( cooldown && GetTypeId()==TYPEID_PLAYER && (this->ToPlayer())->HasSpellCooldown(triggered_spell_id))
        return false;

    if(basepoints0)
        CastCustomSpell(target,triggered_spell_id,&basepoints0,NULL,NULL,true,castItem,triggeredByAura);
    else
        CastSpell(target,triggered_spell_id,true,castItem,triggeredByAura);

    if( cooldown && GetTypeId()==TYPEID_PLAYER )
        (this->ToPlayer())->AddSpellCooldown(triggered_spell_id,0,time(NULL) + cooldown);

    return true;
}

bool Unit::HandleProcTriggerSpell(Unit *pVictim, uint32 damage, Aura* triggeredByAura, SpellEntry const *procSpell, uint32 procFlags, uint32 procEx, uint32 cooldown)
{
    // Get triggered aura spell info
    SpellEntry const* auraSpellInfo = triggeredByAura->GetSpellProto();
    
    //sLog.outString("ProcSpell %u (%s) triggered spell %u (%s)", procSpell->Id, procSpell->SpellName[sWorld.GetDefaultDbcLocale()], auraSpellInfo->Id, auraSpellInfo->SpellName[sWorld.GetDefaultDbcLocale()]);

    // Basepoints of trigger aura
    int32 triggerAmount = triggeredByAura->GetModifier()->m_amount;

    // Set trigger spell id, target, custom basepoints
    uint32 trigger_spell_id = auraSpellInfo->EffectTriggerSpell[triggeredByAura->GetEffIndex()];
    Unit*  target = NULL;
    int32  basepoints0 = 0;

    Item* castItem = triggeredByAura->GetCastItemGUID() && GetTypeId()==TYPEID_PLAYER
        ? (this->ToPlayer())->GetItemByGuid(triggeredByAura->GetCastItemGUID()) : NULL;

    // Try handle unknown trigger spells
    if (spellmgr.LookupSpell(trigger_spell_id)==NULL)
    switch (auraSpellInfo->SpellFamilyName)
    {
     //=====================================================================
     // Generic class
     // ====================================================================
     // .....
     //=====================================================================
     case SPELLFAMILY_GENERIC:
     if (auraSpellInfo->Id==43820)   // Charm of the Witch Doctor (Amani Charm of the Witch Doctor trinket)
     {
          // Pct value stored in dummy
          basepoints0 = pVictim->GetCreateHealth() * auraSpellInfo->EffectBasePoints[1] / 100;
          target = pVictim;
          break;
     }
     else if (auraSpellInfo->Id == 45054) {     // Item 34470: don't proc on positive spells like Health Funnel
        if (procSpell && procSpell->Id == 755)
            return true;
        // Unsure
        if (procSpell && IsPositiveSpell(procSpell->Id))
            return true;
        break;
     }
     else if (auraSpellInfo->Id == 27522 || auraSpellInfo->Id == 46939)   // Black bow of the Betrayer
     {
         // On successful melee or ranged attack gain $29471s1 mana and if possible drain $27526s1 mana from the target.
         if (this && this->IsAlive())
             CastSpell(this, 29471, true, castItem, triggeredByAura);
         if (pVictim && pVictim->IsAlive()) {
             //CastSpell(pVictim, 27526, true, castItem, triggeredByAura);
             if (pVictim->getPowerType() == POWER_MANA && pVictim->GetPower(POWER_MANA) > 8)
                CastSpell(this, 27526, true, castItem, triggeredByAura);
        }
        //RemoveAurasDueToSpell(46939);
         return true;
     }
     break;
     //=====================================================================
     // Mage
     //=====================================================================
     // Blazing Speed (Rank 1,2) trigger = 18350
     //=====================================================================
     case SPELLFAMILY_MAGE:
         //nothing
     break;
     //=====================================================================
     // Warrior
     //=====================================================================
     // Rampage (Rank 1-3) trigger = 18350
     //=====================================================================
     case SPELLFAMILY_WARRIOR:
         //nothing
     break;
     //=====================================================================
     // Warlock
     //=====================================================================
     // Pyroclasm             trigger = 18350
     // Drain Soul (Rank 1-5) trigger = 0
     //=====================================================================
     case SPELLFAMILY_WARLOCK:
     {
         // Pyroclasm
         if (auraSpellInfo->SpellIconID == 1137)
         {
             if(!pVictim || !pVictim->IsAlive() || pVictim == this || procSpell == NULL)
                 return false;
             // Calculate spell tick count for spells
             uint32 tick = 1; // Default tick = 1

             // Hellfire have 15 tick
             if (procSpell->SpellFamilyFlags&0x0000000000000040LL)
                 tick = 1;  // was 15
             // Rain of Fire have 4 tick
             else if (procSpell->SpellFamilyFlags&0x0000000000000020LL)
                 tick = 4;  // was 4
             else
                 return false;

             // Calculate chance = baseChance / tick
             float chance = 0;
             switch (auraSpellInfo->Id)
             {
                 case 18096: chance = 13.0f / tick; break;
                 case 18073: chance = 26.0f / tick; break;
             }
             // Roll chance
             if (!roll_chance_f(chance))
                 return false;

             //triggered_spell_id = 18093;
            CastSpell(pVictim, 18093, true);
         }
         // Drain Soul
         else if (auraSpellInfo->SpellFamilyFlags & 0x0000000000004000LL)
         {
             Unit::AuraList const& mAddFlatModifier = GetAurasByType(SPELL_AURA_ADD_FLAT_MODIFIER);
             for(Unit::AuraList::const_iterator i = mAddFlatModifier.begin(); i != mAddFlatModifier.end(); ++i)
             {
                 if ((*i)->GetModifier()->m_miscvalue == SPELLMOD_CHANCE_OF_SUCCESS && (*i)->GetSpellProto()->SpellIconID == 113)
                 {
                     int32 value2 = CalculateSpellDamage((*i)->GetSpellProto(),2,(*i)->GetSpellProto()->EffectBasePoints[2],this);
                     basepoints0 = value2 * GetMaxPower(POWER_MANA) / 100;
                 }
             }
             if ( basepoints0 == 0 )
                 return false;
             trigger_spell_id = 18371;
         }
         break;
     }
     //=====================================================================
     // Priest
     //=====================================================================
     // Greater Heal Refund         trigger = 18350
     // Blessed Recovery (Rank 1-3) trigger = 18350
     // Shadowguard (1-7)           trigger = 28376
     //=====================================================================
     case SPELLFAMILY_PRIEST:
     {
         // Blessed Recovery
         if (auraSpellInfo->SpellIconID == 1875)
         {
             basepoints0 = damage * triggerAmount / 100 / 3;
             target = this;
         }
         break;
     }
     //=====================================================================
     // Druid
     // ====================================================================
     // Druid Forms Trinket  trigger = 18350
     //=====================================================================
     case SPELLFAMILY_DRUID:
     {
         // Druid Forms Trinket
         if (auraSpellInfo->Id==37336)
         {
             switch(m_form)
             {
                 case 0:              trigger_spell_id = 37344;break;
                 case FORM_CAT:       trigger_spell_id = 37341;break;
                 case FORM_BEAR:
                 case FORM_DIREBEAR:  trigger_spell_id = 37340;break;
                 case FORM_TREE:      trigger_spell_id = 37342;break;
                 case FORM_MOONKIN:   trigger_spell_id = 37343;break;
                 default:
                     return false;
             }
         }
         break;
     }
     //=====================================================================
     // Hunter
     // ====================================================================
     // ......
     //=====================================================================
     case SPELLFAMILY_HUNTER:
     break;
     //=====================================================================
     // Paladin
     // ====================================================================
     // Blessed Life                   trigger = 31934
     // Healing Discount               trigger = 18350
     // Illumination (Rank 1-5)        trigger = 18350
     // Lightning Capacitor            trigger = 18350
     //=====================================================================
     case SPELLFAMILY_PALADIN:
     {
         // Blessed Life
         if (auraSpellInfo->SpellIconID == 2137)
         {
             switch (auraSpellInfo->Id)
             {
                 case 31828: // Rank 1
                 case 31829: // Rank 2
                 case 31830: // Rank 3
                    sLog.outDebug("Blessed life trigger!");
                 break;
                 default:
                     sLog.outError("Unit::HandleProcTriggerSpell: Spell %u miss posibly Blessed Life", auraSpellInfo->Id);
                 return false;
             }
         }
         // Healing Discount
         if (auraSpellInfo->Id==37705)
         {
             // triggers Healing Trance
             switch (getClass())
             {
                 case CLASS_PALADIN: trigger_spell_id = 37723; break;
                 case CLASS_DRUID: trigger_spell_id = 37721; break;
                 case CLASS_PRIEST: trigger_spell_id = 37706; break;
                 case CLASS_SHAMAN: trigger_spell_id= 37722; break;
                 default: return false;
             }
             target = this;
         }
         // Illumination
         else if (auraSpellInfo->SpellIconID==241)
         {
             if(!procSpell)
                 return false;
             // procspell is triggered spell but we need mana cost of original casted spell
             uint32 originalSpellId = procSpell->Id;
             // Holy Shock
             if(procSpell->SpellFamilyFlags & 0x1000000000000LL) // Holy Shock heal
             {
                 switch(procSpell->Id)
                 {
                     case 25914: originalSpellId = 20473; break;
                     case 25913: originalSpellId = 20929; break;
                     case 25903: originalSpellId = 20930; break;
                     case 27175: originalSpellId = 27174; break;
                     case 33074: originalSpellId = 33072; break;
                     default:
                         sLog.outError("Unit::HandleProcTriggerSpell: Spell %u not handled in HShock",procSpell->Id);
                     return false;
                 }
             }
             SpellEntry const *originalSpell = spellmgr.LookupSpell(originalSpellId);
             if(!originalSpell)
             {
                 sLog.outError("Unit::HandleProcTriggerSpell: Spell %u unknown but selected as original in Illu",originalSpellId);
                 return false;
             }
             // percent stored in effect 1 (class scripts) base points
             basepoints0 = originalSpell->manaCost*(auraSpellInfo->EffectBasePoints[1]+1)/100;
             trigger_spell_id = 20272;
             target = this;
         }
         // Lightning Capacitor
         else if (auraSpellInfo->Id==37657)
         {
             if(!pVictim || !pVictim->IsAlive())
                 return false;
             // stacking
             CastSpell(this, 37658, true, NULL, triggeredByAura);
             // counting
             Aura * dummy = GetDummyAura(37658);
             if (!dummy)
                 return false;
             // release at 3 aura in stack (cont contain in basepoint of trigger aura)
             if(dummy->GetStackAmount() <= 2)
                 return false;

             RemoveAurasDueToSpell(37658);
             target = pVictim;
         }
         break;
     }
     //=====================================================================
     // Shaman
     //====================================================================
     // Nature's Guardian (Rank 1-5) trigger = 18350
     //=====================================================================
     case SPELLFAMILY_SHAMAN:
     {
         if (auraSpellInfo->SpellIconID == 2013) //Nature's Guardian
         {
             // Check health condition - should drop to less 30% (damage deal after this!)
             if (!(10*(int32(GetHealth() - damage)) < 3 * GetMaxHealth()))
                 return false;

             if(pVictim && pVictim->IsAlive())
                 pVictim->getThreatManager().modifyThreatPercent(this,-10);

             basepoints0 = triggerAmount * GetMaxHealth() / 100;
             trigger_spell_id = 31616;
             target = this;
         }
         break;
     }
     // default
     default:
         break;
    }

    // All ok. Check current trigger spell
    SpellEntry const* triggerEntry = spellmgr.LookupSpell(trigger_spell_id);
    if ( triggerEntry == NULL )
    {
        // Not cast unknown spell
        // sLog.outError("Unit::HandleProcTriggerSpell: Spell %u have 0 in EffectTriggered[%d], not handled custom case?",auraSpellInfo->Id,triggeredByAura->GetEffIndex());
        return false;
    }

    // check if triggering spell can stack with current target's auras (if not - don't proc)
    // don't check if 
    // aura is passive (talent's aura)
    // trigger_spell_id's aura is already active (allow to refresh triggered auras)
    // trigger_spell_id's triggeredByAura is already active (for example shaman's shields)
    AuraMap::iterator i,next;
    uint32 aura_id = 0;
    for (i = m_Auras.begin(); i != m_Auras.end(); i = next)
    {
        next = i;
        ++next;
        if (!(*i).second) continue;
            aura_id = (*i).second->GetSpellProto()->Id;
            if ( IsPassiveSpell(aura_id) || aura_id == trigger_spell_id || aura_id == triggeredByAura->GetSpellProto()->Id ) continue;
        if (spellmgr.IsNoStackSpellDueToSpell(trigger_spell_id, (*i).second->GetSpellProto()->Id, ((*i).second->GetCasterGUID() == GetGUID())))
            return false;
    }

    // Costum requirements (not listed in procEx) Warning! damage dealing after this
    // Custom triggered spells
    switch (auraSpellInfo->Id)
    {
        case 24905:   // Moonkin Form (Passive)
        {
            basepoints0 = GetTotalAttackPowerValue(BASE_ATTACK, pVictim) * 30 / 100;
            target = this;
            break;
        }
        // Lightning Shield (The Ten Storms set)
        case 23551:
        {
            target = pVictim;
            break;
        }
        // Mana Surge (The Earthfury set)
        case 23572:
        {
            if(!procSpell)
                return false;
            basepoints0 = procSpell->manaCost * 35 / 100;
            target = this;
            break;
        }
        //Leader of the pack
        case 24932:
        {
            if (triggerAmount == 0)
                return false;
            basepoints0 = triggerAmount * GetMaxHealth() / 100;
            break;
        }
        // Blackout
        case 15326:
        {
            if (procSpell->Id == 2096 || procSpell->Id == 10909)
                return false;
            if (IsPositiveSpell(procSpell->Id))
                return false;
            break;
        }
        // Persistent Shield (Scarab Brooch trinket)
        // This spell originally trigger 13567 - Dummy Trigger (vs dummy efect)
        case 26467:
        {
            basepoints0 = damage * 15 / 100;
            target = pVictim;
            trigger_spell_id = 26470;
            break;
        }
        // Cheat Death
        case 28845:
        {
            // When your health drops below 20% ....
            if (GetHealth() - damage > GetMaxHealth() / 5 || GetHealth() < GetMaxHealth() / 5)
                return false;
            break;
        }
        // Deadly Swiftness (Rank 1)
        case 31255:
        {
            // whenever you deal damage to a target who is below 20% health.
            if (pVictim->GetHealth() > pVictim->GetMaxHealth() / 5)
                return false;

            target = this;
            trigger_spell_id = 22588;
        }
        // Greater Heal Refund (Avatar Raiment set)
        case 37594:
        {
            // Not give if target alredy have full health
            if (pVictim->GetHealth() == pVictim->GetMaxHealth())
                return false;
            // If your Greater Heal brings the target to full health, you gain $37595s1 mana.
            if (pVictim->GetHealth() + damage < pVictim->GetMaxHealth())
                return false;
            break;
        }
        // Unyielding Knights
        case 38164:
        {
            if (pVictim->GetEntry() != 19457)
                return false;
            break;
        }
        // Bonus Healing (Crystal Spire of Karabor mace)
        case 40971:
        {
            // If your target is below $s1% health
            if (pVictim->GetHealth() > pVictim->GetMaxHealth() * triggerAmount / 100)
                return false;
            break;
        }
        // Evasive Maneuvers (Commendation of Kael`thas trinket)
        case 45057:
        {
            // reduce you below $s1% health
            if (GetHealth() - damage > GetMaxHealth() * triggerAmount / 100)
                return false;
            break;
        }
        // Warriors Sword spec
        /*case 12281:
        case 12812:
        case 12813:
        case 12814:
        case 12815:*/
        case 16459:
            return false;
    }

    // Costum basepoints/target for exist spell
    // dummy basepoints or other customs
    switch(trigger_spell_id)
    {
        // Cast positive spell on enemy target
        case 7099:  // Curse of Mending
        case 39647: // Curse of Mending
        case 29494: // Temptation
        case 20233: // Improved Lay on Hands (cast on target)
        {
            target = pVictim;
            break;
        }
        // Combo points add triggers (need add combopoint only for main tatget, and after possible combopoints reset)
        case 15250: // Rogue Setup
        {
            /*if(!pVictim || pVictim != GetVictim())   // applied only for main target
                return false;*/
            break;                                   // continue normal case
        }
        // Finish movies that add combo
        case 14189: // Seal Fate (Netherblade set)
        {
            // Need add combopoint AFTER finish movie (or they dropped in finish phase)
            break;
        }
        case 14157: // Ruthlessness
        {
            return false; //prevent adding the combo point BEFORE finish movie. Ruthlessness is handled in Player::ClearComboPoints()  
            // Need add combopoint AFTER finish movie (or they dropped in finish phase)
            break;
        }
        // Shamanistic Rage triggered spell
        case 30824:
        {
            basepoints0 = int32(GetTotalAttackPowerValue(BASE_ATTACK, pVictim) * triggerAmount / 100);
            trigger_spell_id = 30824;
            break;
        }
        // Enlightenment (trigger only from mana cost spells)
        case 35095:
        {
            if(!procSpell || procSpell->powerType!=POWER_MANA || procSpell->manaCost==0 && procSpell->ManaCostPercentage==0 && procSpell->manaCostPerlevel==0)
                return false;
            break;
        }
    }

    switch(auraSpellInfo->SpellFamilyName)
    {
        case SPELLFAMILY_PALADIN:
            // Judgement of Light and Judgement of Wisdom
            if (auraSpellInfo->SpellFamilyFlags & 0x0000000000080000LL)
            {
                pVictim->CastSpell(pVictim, trigger_spell_id, true, castItem, triggeredByAura);
                return true;                        // no hidden cooldown
            }
            break;
        default:
            break;
    }

    if( cooldown && GetTypeId()==TYPEID_PLAYER && (this->ToPlayer())->HasSpellCooldown(trigger_spell_id))
        return false;

    // try detect target manually if not set
    if ( target == NULL && pVictim)
    {
        // Do not allow proc negative spells on self
        if (GetGUID()==pVictim->GetGUID() && !(IsPositiveSpell(trigger_spell_id) || (procFlags & PROC_FLAG_SUCCESSFUL_POSITIVE_SPELL)) && !(procEx & PROC_EX_REFLECT))
            return false;
        target = !(procFlags & PROC_FLAG_SUCCESSFUL_POSITIVE_SPELL) && IsPositiveSpell(trigger_spell_id) ? this : pVictim;
    }

    // default case
    if(!target || target!=this && (!target->IsAlive() || !target->isAttackableByAOE()))
        return false;

    // apply spell cooldown before casting to prevent triggering spells with SPELL_EFFECT_ADD_EXTRA_ATTACKS if spell has hidden cooldown
    if( cooldown && GetTypeId()==TYPEID_PLAYER )
        (this->ToPlayer())->AddSpellCooldown(trigger_spell_id,0,time(NULL) + cooldown);

    if(basepoints0)
        CastCustomSpell(target,trigger_spell_id,&basepoints0,NULL,NULL,true,castItem,triggeredByAura);
    else
        CastSpell(target,trigger_spell_id,true,castItem,triggeredByAura);

    return true;
}

bool Unit::HandleOverrideClassScriptAuraProc(Unit *pVictim, Aura *triggeredByAura, SpellEntry const *procSpell, uint32 cooldown)
{
    int32 scriptId = triggeredByAura->GetModifier()->m_miscvalue;

    if(!pVictim || !pVictim->IsAlive())
        return false;

    Item* castItem = triggeredByAura->GetCastItemGUID() && GetTypeId()==TYPEID_PLAYER
        ? (this->ToPlayer())->GetItemByGuid(triggeredByAura->GetCastItemGUID()) : NULL;

    uint32 triggered_spell_id = 0;

    switch(scriptId)
    {
        case 836:                                           // Improved Blizzard (Rank 1)
        {
            if (!procSpell || procSpell->SpellVisual!=9487)
                return false;
            triggered_spell_id = 12484;
            break;
        }
        case 988:                                           // Improved Blizzard (Rank 2)
        {
            if (!procSpell || procSpell->SpellVisual!=9487)
                return false;
            triggered_spell_id = 12485;
            break;
        }
        case 989:                                           // Improved Blizzard (Rank 3)
        {
            if (!procSpell || procSpell->SpellVisual!=9487)
                return false;
            triggered_spell_id = 12486;
            break;
        }
        case 4086:                                          // Improved Mend Pet (Rank 1)
        case 4087:                                          // Improved Mend Pet (Rank 2)
        {
            int32 chance = triggeredByAura->GetSpellProto()->EffectBasePoints[triggeredByAura->GetEffIndex()];
            if(!roll_chance_i(chance))
                return false;

            triggered_spell_id = 24406;
            break;
        }
        case 4533:                                          // Dreamwalker Raiment 2 pieces bonus
        {
            // Chance 50%
            if (!roll_chance_i(50))
                return false;

            switch (pVictim->getPowerType())
            {
                case POWER_MANA:   triggered_spell_id = 28722; break;
                case POWER_RAGE:   triggered_spell_id = 28723; break;
                case POWER_ENERGY: triggered_spell_id = 28724; break;
                default:
                    return false;
            }
            break;
        }
        case 4537:                                          // Dreamwalker Raiment 6 pieces bonus
            triggered_spell_id = 28750;                     // Blessing of the Claw
            break;
        case 5497:                                          // Improved Mana Gems (Serpent-Coil Braid)
            triggered_spell_id = 37445;                     // Mana Surge
            break;
    }

    // not processed
    if(!triggered_spell_id)
        return false;

    // standard non-dummy case
    SpellEntry const* triggerEntry = spellmgr.LookupSpell(triggered_spell_id);

    if(!triggerEntry)
    {
        sLog.outError("Unit::HandleOverrideClassScriptAuraProc: Spell %u triggering for class script id %u",triggered_spell_id,scriptId);
        return false;
    }

    if( cooldown && GetTypeId()==TYPEID_PLAYER && (this->ToPlayer())->HasSpellCooldown(triggered_spell_id))
        return false;

    CastSpell(pVictim, triggered_spell_id, true, castItem, triggeredByAura);

    if( cooldown && GetTypeId()==TYPEID_PLAYER )
        (this->ToPlayer())->AddSpellCooldown(triggered_spell_id,0,time(NULL) + cooldown);

    return true;
}

void Unit::setPowerType(Powers new_powertype)
{
    SetByteValue(UNIT_FIELD_BYTES_0, 3, new_powertype);

    if(GetTypeId() == TYPEID_PLAYER)
    {
        if((this->ToPlayer())->GetGroup())
            (this->ToPlayer())->SetGroupUpdateFlag(GROUP_UPDATE_FLAG_POWER_TYPE);
    }
    else if((this->ToCreature())->IsPet())
    {
        Pet *pet = ((Pet*)this);
        if(pet->isControlled())
        {
            Unit *owner = GetOwner();
            if(owner && (owner->GetTypeId() == TYPEID_PLAYER) && (owner->ToPlayer())->GetGroup())
                (owner->ToPlayer())->SetGroupUpdateFlag(GROUP_UPDATE_FLAG_PET_POWER_TYPE);
        }
    }

    switch(new_powertype)
    {
        default:
        case POWER_MANA:
            break;
        case POWER_RAGE:
            SetMaxPower(POWER_RAGE,GetCreatePowers(POWER_RAGE));
            SetPower(   POWER_RAGE,0);
            break;
        case POWER_FOCUS:
            SetMaxPower(POWER_FOCUS,GetCreatePowers(POWER_FOCUS));
            SetPower(   POWER_FOCUS,GetCreatePowers(POWER_FOCUS));
            break;
        case POWER_ENERGY:
            SetMaxPower(POWER_ENERGY,GetCreatePowers(POWER_ENERGY));
            SetPower(   POWER_ENERGY,0);
            break;
        case POWER_HAPPINESS:
            SetMaxPower(POWER_HAPPINESS,GetCreatePowers(POWER_HAPPINESS));
            SetPower(POWER_HAPPINESS,GetCreatePowers(POWER_HAPPINESS));
            break;
    }
}

FactionTemplateEntry const* Unit::getFactionTemplateEntry() const
{
    FactionTemplateEntry const* entry = sFactionTemplateStore.LookupEntry(getFaction());
    if(!entry)
    {
        static uint64 guid = 0;                             // prevent repeating spam same faction problem

        if(GetGUID() != guid)
        {
            if(GetTypeId() == TYPEID_PLAYER)
                sLog.outError("Player %s have invalid faction (faction template id) #%u", (this->ToPlayer())->GetName(), getFaction());
            else
                sLog.outError("Creature (template id: %u) have invalid faction (faction template id) #%u", (this->ToCreature())->GetCreatureInfo()->Entry, getFaction());
            guid = GetGUID();
        }
    }
    return entry;
}

bool Unit::IsHostileTo(Unit const* unit) const
{
    PROFILE;
    
    // always non-hostile to self
    if (unit == this)
        return false;

    // always non-hostile to GM in GM mode
    if (unit->GetTypeId() == TYPEID_PLAYER && (((Player const*) unit)->isGameMaster() || ((Player const*) unit)->isSpectator()))
        return false;

    // always hostile to enemy
    if (GetVictim() == unit || unit->GetVictim() == this)
        return true;

    // Karazhan chess exception
    if (getFaction() == 1689 && unit->getFaction() == 1690)
        return true;

    if (getFaction() == 1690 && unit->getFaction() == 1689)
        return true;

    // test pet/charm masters instead pers/charmeds
    Unit const* myOwner = GetCharmerOrOwner();
    Unit const* targetOwner = unit->GetCharmerOrOwner();

    // always hostile to owner's enemy
    if (myOwner && (myOwner->GetVictim() == unit || unit->GetVictim() == myOwner))
        return true;

    // always hostile to enemy owner
    if (targetOwner && (GetVictim() == targetOwner || targetOwner->GetVictim() == this))
        return true;

    // always hostile to owner of owner's enemy
    if (myOwner && targetOwner && (myOwner->GetVictim() == targetOwner || targetOwner->GetVictim() == myOwner))
        return true;

    Unit const* meOrMyOwner = myOwner ? myOwner : this;
    Unit const* target = targetOwner ? targetOwner : unit;

    // always non-hostile to target with common owner, or to owner/pet
    if (meOrMyOwner == target)
        return false;    

    // special cases (Duel, etc)
    if (meOrMyOwner->GetTypeId() == TYPEID_PLAYER && target->GetTypeId() == TYPEID_PLAYER) {
        Player const* pTester = (Player const*) meOrMyOwner;
        Player const* pTarget = (Player const*) target;

        // Duel
        if (pTester->duel && pTester->duel->opponent == pTarget && pTester->duel->startTime != 0)
            return true;

        // PvP Zone
        if( (meOrMyOwner->ToPlayer() && meOrMyOwner->ToPlayer()->isInDuelArea())
            || (pTarget->ToPlayer() && pTarget->ToPlayer()->isInDuelArea())
          )
            return false;

        // Group
        if (pTester->GetGroup() && pTester->GetGroup() == pTarget->GetGroup())
            return false;

        // Sanctuary
        if (pTarget->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_SANCTUARY) && pTester->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_SANCTUARY))
            return false;

        // PvP FFA state
        if (pTester->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_FFA_PVP) && pTarget->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_FFA_PVP))
            return true;

        //= PvP states
        // Green/Blue (can't attack)
        if (pTester->GetTeam() == pTarget->GetTeam())
            return false;

        // Red (can attack) if true, Blue/Yellow (can't attack) in another case
        return pTester->IsPvP() && pTarget->IsPvP();
    }

    // faction base cases
    FactionTemplateEntry const* tester_faction = meOrMyOwner->getFactionTemplateEntry();
    FactionTemplateEntry const* target_faction = target->getFactionTemplateEntry();
    if (!tester_faction || !target_faction)
        return false;

    if (target->isAttackingPlayer() && meOrMyOwner->IsContestedGuard())
        return true;

    // PvC forced reaction and reputation case
    if (meOrMyOwner->GetTypeId() == TYPEID_PLAYER) {
        // forced reaction
        ForcedReactions::const_iterator forceItr = (meOrMyOwner->ToPlayer())->m_forcedReactions.find(target_faction->faction);
        if (forceItr != (meOrMyOwner->ToPlayer())->m_forcedReactions.end())
            return forceItr->second <= REP_HOSTILE;

        // if faction have reputation then hostile state for tester at 100% dependent from at_war state
        if (FactionEntry const* raw_target_faction = sFactionStore.LookupEntry(target_faction->faction))
            if (raw_target_faction->reputationListID >= 0)
                if (FactionState const* factionState = (meOrMyOwner->ToPlayer())->GetFactionState(raw_target_faction))
                    return (factionState->Flags & FACTION_FLAG_AT_WAR);
    }        // CvP forced reaction and reputation case
    else if (target->GetTypeId() == TYPEID_PLAYER) {
        // forced reaction
        ForcedReactions::const_iterator forceItr = ((Player const*) target)->m_forcedReactions.find(tester_faction->faction);
        if (forceItr != ((Player const*) target)->m_forcedReactions.end())
            return forceItr->second <= REP_HOSTILE;

        // apply reputation state
        FactionEntry const* raw_tester_faction = sFactionStore.LookupEntry(tester_faction->faction);
        if (raw_tester_faction && raw_tester_faction->reputationListID >= 0)
            return ((Player const*) target)->GetReputationRank(raw_tester_faction) <= REP_HOSTILE;
    }

    // common faction based case (CvC,PvC,CvP)
    return tester_faction->IsHostileTo(*target_faction);
}

bool Unit::IsFriendlyTo(Unit const* unit) const
{
    // always friendly to self
    if(unit==this)
        return true;

    // always friendly to GM in GM mode
    if(unit->GetTypeId()==TYPEID_PLAYER && (((Player const*)unit)->isGameMaster() || ((Player const*)unit)->isSpectator()))
        return true;

    // always non-friendly to enemy
    if(unit->GetTypeId()==TYPEID_UNIT && (GetVictim()==unit || unit->GetVictim()==this))
        return false;
        
    // Karazhan chess exception
    if (getFaction() == 1689 && unit->getFaction() == 1690)
        return false;
    
    if (getFaction() == 1690 && unit->getFaction() == 1689)
        return false; 

    // test pet/charm masters instead pers/charmeds
    Unit const* testerOwner = GetCharmerOrOwner();
    Unit const* targetOwner = unit->GetCharmerOrOwner();

    // always non-friendly to owner's enemy
    if(testerOwner && (testerOwner->GetVictim()==unit || unit->GetVictim()==testerOwner))
        return false;

    // always non-friendly to enemy owner
    if(targetOwner && (GetVictim()==targetOwner || targetOwner->GetVictim()==this))
        return false;

    // always non-friendly to owner of owner's enemy
    if(testerOwner && targetOwner && (testerOwner->GetVictim()==targetOwner || targetOwner->GetVictim()==testerOwner))
        return false;

    Unit const* tester = testerOwner ? testerOwner : this;
    Unit const* target = targetOwner ? targetOwner : unit;

    // always friendly to target with common owner, or to owner/pet
    if(tester==target)
        return true;

    // special cases (Duel)
    if(tester->GetTypeId()==TYPEID_PLAYER && target->GetTypeId()==TYPEID_PLAYER)
    {
        Player const* pTester = (Player const*)tester;
        Player const* pTarget = (Player const*)target;

        // Duel
        if(pTester->duel && pTester->duel->opponent == target && pTester->duel->startTime != 0)
            return false;

        // Group
        if(pTester->GetGroup() && pTester->GetGroup()==pTarget->GetGroup())
            return true;

        // Sanctuary
        if(pTarget->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_SANCTUARY) && pTester->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_SANCTUARY))
            return true;

        // PvP FFA state
        if(pTester->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_FFA_PVP) && pTarget->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_FFA_PVP))
            return false;

        //= PvP states
        // Green/Blue (non-attackable)
        if(pTester->GetTeam()==pTarget->GetTeam())
            return true;

        // Blue (friendly/non-attackable) if not PVP, or Yellow/Red in another case (attackable)
        return !pTarget->IsPvP();
    }

    // faction base cases
    FactionTemplateEntry const*tester_faction = tester->getFactionTemplateEntry();
    FactionTemplateEntry const*target_faction = target->getFactionTemplateEntry();
    if(!tester_faction || !target_faction)
        return false;

    if(target->isAttackingPlayer() && tester->IsContestedGuard())
        return false;

    // PvC forced reaction and reputation case
    if(tester->GetTypeId()==TYPEID_PLAYER)
    {
        // forced reaction
        ForcedReactions::const_iterator forceItr = ((Player const*)tester)->m_forcedReactions.find(target_faction->faction);
        if(forceItr!=((Player const*)tester)->m_forcedReactions.end())
            return forceItr->second >= REP_FRIENDLY;

        // if faction have reputation then friendly state for tester at 100% dependent from at_war state
        if(FactionEntry const* raw_target_faction = sFactionStore.LookupEntry(target_faction->faction))
            if(raw_target_faction->reputationListID >=0)
                if(FactionState const* FactionState = (tester->ToPlayer())->GetFactionState(raw_target_faction))
                    return !(FactionState->Flags & FACTION_FLAG_AT_WAR);
    }
    // CvP forced reaction and reputation case
    else if(target->GetTypeId()==TYPEID_PLAYER)
    {
        // forced reaction
        ForcedReactions::const_iterator forceItr = ((Player const*)target)->m_forcedReactions.find(tester_faction->faction);
        if(forceItr!=((Player const*)target)->m_forcedReactions.end())
            return forceItr->second >= REP_FRIENDLY;

        // apply reputation state
        if(FactionEntry const* raw_tester_faction = sFactionStore.LookupEntry(tester_faction->faction))
            if(raw_tester_faction->reputationListID >=0 )
                return ((Player const*)target)->GetReputationRank(raw_tester_faction) >= REP_FRIENDLY;
    }

    // common faction based case (CvC,PvC,CvP)
    return tester_faction->IsFriendlyTo(*target_faction);
}

bool Unit::IsHostileToPlayers() const
{
    FactionTemplateEntry const* my_faction = getFactionTemplateEntry();
    if(!my_faction)
        return false;

    FactionEntry const* raw_faction = sFactionStore.LookupEntry(my_faction->faction);
    if(raw_faction && raw_faction->reputationListID >=0 )
        return false;

    return my_faction->IsHostileToPlayers();
}

bool Unit::IsNeutralToAll() const
{
    FactionTemplateEntry const* my_faction = getFactionTemplateEntry();
    if(!my_faction)
        return true;

    FactionEntry const* raw_faction = sFactionStore.LookupEntry(my_faction->faction);
    if(raw_faction && raw_faction->reputationListID >=0 )
        return false;

    return my_faction->IsNeutralToAll();
}

/* return true if we started attacking a new target */
bool Unit::Attack(Unit *victim, bool meleeAttack)
{
    if (!victim || victim == this)
        return false;

    // dead units can neither attack nor be attacked
    if (!IsAlive() || !victim->IsAlive())
        return false;

    // player cannot attack in mount state
    if (GetTypeId() == TYPEID_PLAYER) {
        if (IsMounted())
            return false;
    } else {
        Creature* c = victim->ToCreature();
        if (c && c->IsInEvadeMode())
            return false;
    }

    // nobody can attack GM in GM-mode
    if (victim->GetTypeId() == TYPEID_PLAYER) {
        if ((victim->ToPlayer())->isGameMaster() || (victim->ToPlayer())->isSpectator())
            return false;
    } else {
        if ((victim->ToCreature())->IsInEvadeMode())
            return false;
    }

    // remove SPELL_AURA_MOD_UNATTACKABLE at attack (in case non-interruptible spells stun aura applied also that not let attack)
    if (HasAuraType(SPELL_AURA_MOD_UNATTACKABLE))
        RemoveSpellsCausingAura(SPELL_AURA_MOD_UNATTACKABLE);

    if (GetTypeId() == TYPEID_UNIT && getStandState() == UNIT_STAND_STATE_DEAD)
        SetStandState(UNIT_STAND_STATE_STAND);

    //already attacking
    if (m_attacking) {
        if (m_attacking == victim) {
            // switch to melee attack from ranged/magic
            if (meleeAttack)
            {
                if(!HasUnitState(UNIT_STAT_MELEE_ATTACKING)) 
                {
                    addUnitState(UNIT_STAT_MELEE_ATTACKING);
                    SendAttackStart(victim);
                    return true;
                }
            } else if (HasUnitState(UNIT_STAT_MELEE_ATTACKING)) 
            {
                clearUnitState(UNIT_STAT_MELEE_ATTACKING);
                SendAttackStop(victim); //melee attack stop
                return true;
            }

            return false;
        }

        AttackStop();
    }

    //Set our target
    SetTarget(victim->GetGUID());        

    m_attacking = victim;
    m_attacking->_addAttacker(this);

    //if(m_attacking->GetTypeId()==TYPEID_UNIT && (m->ToCreature()_attacking)->IsAIEnabled)
    //    (m->ToCreature()_attacking)->AI()->AttackedBy(this);

    if (GetTypeId() == TYPEID_UNIT && !(ToCreature()->IsPet())) {
        WorldPacket data(SMSG_AI_REACTION, 12);
        data << uint64(GetGUID());
        data << uint32(AI_REACTION_AGGRO); // Aggro sound
        ((WorldObject*)this)->SendMessageToSet(&data, true);

        (ToCreature())->CallAssistance();

        // should not let player enter combat by right clicking target
        SetInCombatWith(victim);
        if (victim->GetTypeId() == TYPEID_PLAYER)
            victim->SetInCombatWith(this);
        else
            (victim->ToCreature())->AI()->AttackedBy(this);
        AddThreat(victim, 0.0f);
    }

    // delay offhand weapon attack to next attack time
    if (haveOffhandWeapon())
        resetAttackTimer(OFF_ATTACK);

    if (meleeAttack) 
    {
        addUnitState(UNIT_STAT_MELEE_ATTACKING);
        SendAttackStart(victim);
    }

    return true;
}

bool Unit::IsCombatStationary()
{
    return isInRoots();
}

bool Unit::AttackStop()
{
    if (!m_attacking)
        return false;

    Unit* victim = m_attacking;
    getThreatManager().clearCurrentVictim();

    m_attacking->_removeAttacker(this);
    m_attacking = NULL;

    //Clear our target
    SetTarget(0);

    clearUnitState(UNIT_STAT_MELEE_ATTACKING);

    InterruptSpell(CURRENT_MELEE_SPELL);

    if( GetTypeId()==TYPEID_UNIT )
    {
        // reset call assistance
        (this->ToCreature())->SetNoCallAssistance(false);
    }

    SendAttackStop(victim);

    return true;
}

void Unit::CombatStop(bool cast)
{
    if(cast && IsNonMeleeSpellCasted(false))
        InterruptNonMeleeSpells(false);

    AttackStop();
    RemoveAllAttackers();
    if( GetTypeId()==TYPEID_PLAYER )
        (this->ToPlayer())->SendAttackSwingCancelAttack();     // melee and ranged forced attack cancel
    ClearInCombat();
    
    if (ToCreature() && ToCreature()->getAI())
        ToCreature()->getAI()->setAICombat(false);
}

void Unit::CombatStopWithPets(bool cast)
{
    CombatStop(cast);
    if(Pet* pet = GetPet())
        pet->CombatStop(cast);
    if(Unit* charm = GetCharm())
        charm->CombatStop(cast);
    if(GetTypeId()==TYPEID_PLAYER)
    {
        GuardianPetList const& guardians = (this->ToPlayer())->GetGuardians();
        for(GuardianPetList::const_iterator itr = guardians.begin(); itr != guardians.end(); ++itr)
            if(Unit* guardian = Unit::GetUnit(*this,*itr))
                guardian->CombatStop(cast);
    }
}

bool Unit::isAttackingPlayer() const
{
    if(HasUnitState(UNIT_STAT_ATTACK_PLAYER))
        return true;

    Pet* pet = GetPet();
    if(pet && pet->isAttackingPlayer())
        return true;

    Unit* charmed = GetCharm();
    if(charmed && charmed->isAttackingPlayer())
        return true;

    for (int8 i = 0; i < MAX_TOTEM; i++)
    {
        if(m_TotemSlot[i])
        {
            Creature *totem = ObjectAccessor::GetCreature(*this, m_TotemSlot[i]);
            if(totem && totem->isAttackingPlayer())
                return true;
        }
    }

    return false;
}

void Unit::RemoveAllAttackers()
{
    while (!m_attackers.empty())
    {
        AttackerSet::iterator iter = m_attackers.begin();
        if(!(*iter)->AttackStop())
        {
            sLog.outError("WORLD: Unit has an attacker that isn't attacking it!");
            m_attackers.erase(iter);
        }
    }
}

void Unit::ModifyAuraState(AuraState flag, bool apply)
{
    if (apply)
    {
        if (!HasFlag(UNIT_FIELD_AURASTATE, 1<<(flag-1)))
        {
            SetFlag(UNIT_FIELD_AURASTATE, 1<<(flag-1));
            if(GetTypeId() == TYPEID_PLAYER)
            {
                const PlayerSpellMap& sp_list = (this->ToPlayer())->GetSpellMap();
                for (PlayerSpellMap::const_iterator itr = sp_list.begin(); itr != sp_list.end(); ++itr)
                {
                    if(itr->second->state == PLAYERSPELL_REMOVED) continue;
                    SpellEntry const *spellInfo = spellmgr.LookupSpell(itr->first);
                    if (!spellInfo || !IsPassiveSpell(itr->first)) continue;
                    if (spellInfo->CasterAuraState == flag)
                        CastSpell(this, itr->first, true, NULL);
                }
            }
        }
    }
    else
    {
        if (HasFlag(UNIT_FIELD_AURASTATE,1<<(flag-1)))
        {
            RemoveFlag(UNIT_FIELD_AURASTATE, 1<<(flag-1));
            Unit::AuraMap& tAuras = GetAuras();
            for (Unit::AuraMap::iterator itr = tAuras.begin(); itr != tAuras.end();)
            {
                SpellEntry const* spellProto = (*itr).second->GetSpellProto();
                if (spellProto->CasterAuraState == flag)
                {
                    // exceptions (applied at state but not removed at state change)
                    // Rampage
                    if(spellProto->SpellIconID==2006 && spellProto->SpellFamilyName==SPELLFAMILY_WARRIOR && spellProto->SpellFamilyFlags==0x100000)
                    {
                        ++itr;
                        continue;
                    }

                    RemoveAura(itr);
                }
                else
                    ++itr;
            }
        }
    }
}

Unit *Unit::GetOwner() const
{
    uint64 ownerid = GetOwnerGUID();
    if(!ownerid)
        return NULL;
    return ObjectAccessor::GetUnit(*this, ownerid);
}

Unit *Unit::GetCharmer() const
{
    if(uint64 charmerid = GetCharmerGUID())
        return ObjectAccessor::GetUnit(*this, charmerid);
    return NULL;
}

Player* Unit::GetCharmerOrOwnerPlayerOrPlayerItself() const
{
    uint64 guid = GetCharmerOrOwnerGUID();
    if(IS_PLAYER_GUID(guid))
        return ObjectAccessor::GetPlayer(*this, guid);

    Player *p = const_cast<Player*>(ToPlayer());

    return GetTypeId()==TYPEID_PLAYER ? p : NULL;
}

Pet* Unit::GetPet() const
{
    if(uint64 pet_guid = GetPetGUID())
    {
        if(Pet* pet = ObjectAccessor::GetPet(*this,pet_guid))
            return pet;

        sLog.outError("Unit::GetPet: Pet %u not exist.",GUID_LOPART(pet_guid));
        const_cast<Unit*>(this)->SetPet(0);
    }

    return NULL;
}

Unit* Unit::GetCharm() const
{
    if(uint64 charm_guid = GetCharmGUID())
    {
        if(Unit* pet = ObjectAccessor::GetUnit(*this, charm_guid))
            return pet;

        sLog.outError("Unit::GetCharm: Charmed creature %u not exist.",GUID_LOPART(charm_guid));
        const_cast<Unit*>(this)->SetCharm(0);
    }

    return NULL;
}

void Unit::SetPet(Pet* pet)
{
    SetUInt64Value(UNIT_FIELD_SUMMON, pet ? pet->GetGUID() : 0);

    // FIXME: hack, speed must be set only at follow
    if(pet)
        for(int i = 0; i < MAX_MOVE_TYPE; ++i)
            if(m_speed_rate[i] > 1.0f)
                pet->SetSpeed(UnitMoveType(i), m_speed_rate[i], true);
}

void Unit::SetCharm(Unit* pet)
{
    if(GetTypeId() == TYPEID_PLAYER)
        SetUInt64Value(UNIT_FIELD_CHARM, pet ? pet->GetGUID() : 0);
}

void Unit::AddPlayerToVision(Player* plr)
{
    if(m_sharedVision.empty())
    {
        setActive(true);
        SetWorldObject(true);
    }
    m_sharedVision.push_back(plr->GetGUID());
    plr->SetFarsightTarget(this);
}

void Unit::RemovePlayerFromVision(Player* plr)
{
    m_sharedVision.remove(plr->GetGUID());
    if(m_sharedVision.empty())
    {
        setActive(false);
        SetWorldObject(false);
    }
    plr->ClearFarsight();
}

void Unit::RemoveBindSightAuras()
{
    RemoveSpellsCausingAura(SPELL_AURA_BIND_SIGHT);
}

void Unit::RemoveCharmAuras()
{
    RemoveSpellsCausingAura(SPELL_AURA_MOD_CHARM);
    RemoveSpellsCausingAura(SPELL_AURA_MOD_POSSESS_PET);
    RemoveSpellsCausingAura(SPELL_AURA_MOD_POSSESS);
}

void Unit::UnsummonAllTotems()
{
    for (int8 i = 0; i < MAX_TOTEM; ++i)
    {
        if(!m_TotemSlot[i])
            continue;

        Creature *OldTotem = ObjectAccessor::GetCreature(*this, m_TotemSlot[i]);
        if (OldTotem && OldTotem->isTotem())
            ((Totem*)OldTotem)->UnSummon();
    }
}

void Unit::SendHealSpellLog(Unit *pVictim, uint32 SpellID, uint32 Damage, bool critical)
{
    // we guess size
    WorldPacket data(SMSG_SPELLHEALLOG, (8+8+4+4+1));
    data.append(pVictim->GetPackGUID());
    data.append(GetPackGUID());
    data << uint32(SpellID);
    data << uint32(Damage);
    data << uint8(critical ? 1 : 0);
    data << uint8(0);                                       // unused in client?
    SendMessageToSet(&data, true);
}

void Unit::SendEnergizeSpellLog(Unit *pVictim, uint32 SpellID, uint32 Damage, Powers powertype)
{
    WorldPacket data(SMSG_SPELLENERGIZELOG, (8+8+4+4+4+1));
    data.append(pVictim->GetPackGUID());
    data.append(GetPackGUID());
    data << uint32(SpellID);
    data << uint32(powertype);
    data << uint32(Damage);
    SendMessageToSet(&data, true);
}

uint32 Unit::SpellDamageBonus(Unit *pVictim, SpellEntry const *spellProto, uint32 pdamage, DamageEffectType damagetype)
{
    if(!spellProto || !pVictim || damagetype==DIRECT_DAMAGE )
        return pdamage;
        
    if (spellProto->AttributesEx3 & SPELL_ATTR_EX3_NO_DONE_BONUS)
        return pdamage;

    //if(spellProto->SchoolMask == SPELL_SCHOOL_MASK_NORMAL)
    //    return pdamage;
    //damage = CalcArmorReducedDamage(pVictim, damage);

    int32 BonusDamage = 0;
    if( GetTypeId()==TYPEID_UNIT )
    {
        // Pets just add their bonus damage to their spell damage
        // note that their spell damage is just gain of their own auras
        if ((this->ToCreature())->IsPet() && spellProto->DmgClass == SPELL_DAMAGE_CLASS_MAGIC)
        {
            BonusDamage = ((Pet*)this)->GetBonusDamage();
        }
        // For totems get damage bonus from owner (statue isn't totem in fact)
        else if ((this->ToCreature())->isTotem() && ((Totem*)this)->GetTotemType()!=TOTEM_STATUE)
        {
            if(Unit* owner = GetOwner())
                return owner->SpellDamageBonus(pVictim, spellProto, pdamage, damagetype);
        }
    }

    // Damage Done
    uint32 CastingTime = !IsChanneledSpell(spellProto) ? GetSpellCastTime(spellProto) : GetSpellDuration(spellProto);

    // Taken/Done fixed damage bonus auras
    int32 DoneAdvertisedBenefit  = SpellBaseDamageBonus(GetSpellSchoolMask(spellProto))+BonusDamage;
    int32 TakenAdvertisedBenefit = SpellBaseDamageBonusForVictim(GetSpellSchoolMask(spellProto), pVictim);

    // Damage over Time spells bonus calculation
    float DotFactor = 1.0f;
    if(damagetype == DOT)
    {
        int32 DotDuration = GetSpellDuration(spellProto);
        // 200% limit
        if(DotDuration > 0)
        {
            if(DotDuration > 30000) DotDuration = 30000;
            if(!IsChanneledSpell(spellProto)) DotFactor = DotDuration / 15000.0f;
            int x = 0;
            for(int j = 0; j < 3; j++)
            {
                if( spellProto->Effect[j] == SPELL_EFFECT_APPLY_AURA && (
                    spellProto->EffectApplyAuraName[j] == SPELL_AURA_PERIODIC_DAMAGE ||
                    spellProto->EffectApplyAuraName[j] == SPELL_AURA_PERIODIC_LEECH) )
                {
                    x = j;
                    break;
                }
            }
            int DotTicks = 6;
            if(spellProto->EffectAmplitude[x] != 0)
                DotTicks = DotDuration / spellProto->EffectAmplitude[x];
            if(DotTicks)
            {
                DoneAdvertisedBenefit /= DotTicks;
                TakenAdvertisedBenefit /= DotTicks;
            }
        }
    }

    // Taken/Done total percent damage auras
    float DoneTotalMod = 1.0f;
    float TakenTotalMod = 1.0f;

    // ..done
    AuraList const& mModDamagePercentDone = GetAurasByType(SPELL_AURA_MOD_DAMAGE_PERCENT_DONE);
    for(AuraList::const_iterator i = mModDamagePercentDone.begin(); i != mModDamagePercentDone.end(); ++i)
    {
        //Some auras affect only weapons, like wand spec (6057) or 2H spec (12714)
        if((*i)->GetSpellProto()->Attributes & SPELL_ATTR_AFFECT_WEAPON && (*i)->GetSpellProto()->EquippedItemClass != -1) 
            continue;

        if((*i)->GetModifier()->m_miscvalue & GetSpellSchoolMask(spellProto))
            DoneTotalMod *= ((*i)->GetModifierValue() +100.0f)/100.0f;
    }

    uint32 creatureTypeMask = pVictim->GetCreatureTypeMask();
    AuraList const& mDamageDoneVersus = GetAurasByType(SPELL_AURA_MOD_DAMAGE_DONE_VERSUS);
    for(AuraList::const_iterator i = mDamageDoneVersus.begin();i != mDamageDoneVersus.end(); ++i)
        if(creatureTypeMask & uint32((*i)->GetModifier()->m_miscvalue))
            DoneTotalMod *= ((*i)->GetModifierValue() +100.0f)/100.0f;

    // ..taken
    AuraList const& mModDamagePercentTaken = pVictim->GetAurasByType(SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN);
    for(AuraList::const_iterator i = mModDamagePercentTaken.begin(); i != mModDamagePercentTaken.end(); ++i)
        if( (*i)->GetModifier()->m_miscvalue & GetSpellSchoolMask(spellProto) )
            TakenTotalMod *= ((*i)->GetModifierValue() +100.0f)/100.0f;

    // .. taken pct: scripted (increases damage of * against targets *)
    AuraList const& mOverrideClassScript = GetAurasByType(SPELL_AURA_OVERRIDE_CLASS_SCRIPTS);
    for(AuraList::const_iterator i = mOverrideClassScript.begin(); i != mOverrideClassScript.end(); ++i)
    {
        switch((*i)->GetModifier()->m_miscvalue)
        {
            //Molten Fury
            case 4920: case 4919:
                if(pVictim->HasAuraState(AURA_STATE_HEALTHLESS_20_PERCENT))
                    TakenTotalMod *= (100.0f+(*i)->GetModifier()->m_amount)/100.0f; break;
        }
    }

    bool hasmangle=false;
    // .. taken pct: dummy auras
    AuraList const& mDummyAuras = pVictim->GetAurasByType(SPELL_AURA_DUMMY);
    for(AuraList::const_iterator i = mDummyAuras.begin(); i != mDummyAuras.end(); ++i)
    {
        switch((*i)->GetSpellProto()->SpellIconID)
        {
            //Cheat Death
            case 2109:
                if( ((*i)->GetModifier()->m_miscvalue & GetSpellSchoolMask(spellProto)) )
                {
                    if(pVictim->GetTypeId() != TYPEID_PLAYER)
                        continue;
                    float mod = -(pVictim->ToPlayer())->GetRatingBonusValue(CR_CRIT_TAKEN_SPELL)*2*4;
                    if (mod < (*i)->GetModifier()->m_amount)
                        mod = (*i)->GetModifier()->m_amount;
                    TakenTotalMod *= (mod+100.0f)/100.0f;
                }
                break;
            //This is changed in WLK, using aura 255
            //Mangle
            case 2312:
            case 44955:
                // don't apply mod twice
                if (hasmangle)
                    break;
                hasmangle=true;
                for(int j=0;j<3;j++)
                {
                    if(GetEffectMechanic(spellProto, j)==MECHANIC_BLEED)
                    {
                        TakenTotalMod *= (100.0f+(*i)->GetModifier()->m_amount)/100.0f;
                        break;
                    }
                }
                break;

        }
    }

    // Distribute Damage over multiple effects, reduce by AoE
    CastingTime = GetCastingTimeForBonus( spellProto, damagetype, CastingTime );

    // 50% for damage and healing spells for leech spells from damage bonus and 0% from healing
    for(int j = 0; j < 3; ++j)
    {
        if( spellProto->Effect[j] == SPELL_EFFECT_HEALTH_LEECH ||
            spellProto->Effect[j] == SPELL_EFFECT_APPLY_AURA && spellProto->EffectApplyAuraName[j] == SPELL_AURA_PERIODIC_LEECH )
        {
            CastingTime /= 2;
            break;
        }
    }

    switch(spellProto->SpellFamilyName)
    {
        case SPELLFAMILY_GENERIC:
            // Siphon Essence - 0%
            if(spellProto->AttributesEx == 268435456 && spellProto->SpellIconID == 2027)
            {
                CastingTime = 0;
            }
            // Goblin Rocket Launcher - 0%
            else if (spellProto->SpellIconID == 184 && spellProto->Attributes == 4259840)
            {
                CastingTime = 0;
            }
            // Darkmoon Card: Vengeance - 0.1%
            else if (spellProto->SpellVisual == 9850 && spellProto->SpellIconID == 2230)
            {
                CastingTime = 3.5;
            }
        case SPELLFAMILY_MAGE:
            // Ignite - do not modify, it is (8*Rank)% damage of procing Spell
            if(spellProto->Id==12654)
            {
                return pdamage;
            }
            // Ice Lance
            else if((spellProto->SpellFamilyFlags & 0x20000LL) && spellProto->SpellIconID == 186)
            {
                CastingTime /= 3;                           // applied 1/3 bonuses in case generic target
                if(pVictim->isFrozen())                     // and compensate this for frozen target.
                    TakenTotalMod *= 3.0f;
            }
            // Pyroblast - 115% of Fire Damage, DoT - 20% of Fire Damage
            else if((spellProto->SpellFamilyFlags & 0x400000LL) && spellProto->SpellIconID == 184 )
            {
                DotFactor = damagetype == DOT ? 0.2f : 1.0f;
                CastingTime = damagetype == DOT ? 3500 : 4025;
            }
            // Fireball - 100% of Fire Damage, DoT - 0% of Fire Damage
            else if((spellProto->SpellFamilyFlags & 0x1LL) && spellProto->SpellIconID == 185)
            {
                CastingTime = 3500;
                DotFactor = damagetype == DOT ? 0.0f : 1.0f;
            }
            // Molten armor
            else if (spellProto->SpellFamilyFlags & 0x0000000800000000LL)
            {
                CastingTime = 0;
            }
            // Arcane Missiles triggered spell
            else if ((spellProto->SpellFamilyFlags & 0x200000LL) && spellProto->SpellIconID == 225)
            {
                CastingTime = 1000;
            }
            // Blizzard triggered spell
            else if ((spellProto->SpellFamilyFlags & 0x80080LL) && spellProto->SpellIconID == 285)
            {
                CastingTime = 500;
            }
            break;
        case SPELLFAMILY_WARLOCK:
            // Life Tap
            if((spellProto->SpellFamilyFlags & 0x40000LL) && spellProto->SpellIconID == 208)
            {
                CastingTime = 2800;                         // 80% from +shadow damage
                DoneTotalMod = 1.0f;
                TakenTotalMod = 1.0f;
            }
            // Dark Pact
            else if((spellProto->SpellFamilyFlags & 0x80000000LL) && spellProto->SpellIconID == 154 && GetPetGUID())
            {
                CastingTime = 3360;                         // 96% from +shadow damage
                DoneTotalMod = 1.0f;
                TakenTotalMod = 1.0f;
            }
            // Soul Fire - 115% of Fire Damage
            else if((spellProto->SpellFamilyFlags & 0x8000000000LL) && spellProto->SpellIconID == 184)
            {
                CastingTime = 4025;
            }
            // Curse of Agony - 120% of Shadow Damage
            else if((spellProto->SpellFamilyFlags & 0x0000000400LL) && spellProto->SpellIconID == 544)
            {
                DotFactor = 1.2f;
            }
            // Drain Mana - 0% of Shadow Damage
            else if((spellProto->SpellFamilyFlags & 0x10LL) && spellProto->SpellIconID == 548)
            {
                CastingTime = 0;
            }
            // Drain Soul 214.3%
            else if ((spellProto->SpellFamilyFlags & 0x4000LL) && spellProto->SpellIconID == 113 )
            {
                CastingTime = 7500;
            }
            // Hellfire
            else if ((spellProto->SpellFamilyFlags & 0x40LL) && spellProto->SpellIconID == 937)
            {
                CastingTime = damagetype == DOT ? 5000 : 500; // self damage seems to be so
            }
            // Unstable Affliction - 180%
            else if (spellProto->Id == 31117 && spellProto->SpellIconID == 232)
            {
                CastingTime = 6300;
            }
            // Corruption 93%
            else if ((spellProto->SpellFamilyFlags & 0x2LL) && spellProto->SpellIconID == 313)
            {
                DotFactor = 0.93f;
            }
            break;
        case SPELLFAMILY_PALADIN:
            // Consecration - 95% of Holy Damage
            if((spellProto->SpellFamilyFlags & 0x20LL) && spellProto->SpellIconID == 51)
            {
                DotFactor = 0.95f;
                CastingTime = 3500;
            }
            // Seal of Righteousness - 10.2%/9.8% ( based on weapon type ) of Holy Damage, multiplied by weapon speed
            else if((spellProto->SpellFamilyFlags & 0x8000000LL) && spellProto->SpellIconID == 25)
            {
                Item *item = (this->ToPlayer())->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
                float wspeed = GetAttackTime(BASE_ATTACK)/1000.0f;

                if( item && item->GetProto()->InventoryType == INVTYPE_2HWEAPON)
                   CastingTime = uint32(wspeed*3500*0.102f);
                else
                   CastingTime = uint32(wspeed*3500*0.098f);
            }
            // Judgement of Righteousness - 73%
            else if ((spellProto->SpellFamilyFlags & 1024) && spellProto->SpellIconID == 25)
            {
                CastingTime = 2555;
            }
            // Seal of Vengeance - 17% per Fully Stacked Tick - 5 Applications
            else if ((spellProto->SpellFamilyFlags & 0x80000000000LL) && spellProto->SpellIconID == 2292)
            {
                DotFactor = 0.85f;
                CastingTime = 1850;
            }
            // Holy shield - 5% of Holy Damage
            else if ((spellProto->SpellFamilyFlags & 0x4000000000LL) && spellProto->SpellIconID == 453)
            {
                CastingTime = 175;
            }
            // Blessing of Sanctuary - 0%
            else if ((spellProto->SpellFamilyFlags & 0x10000000LL) && spellProto->SpellIconID == 29)
            {
                CastingTime = 0;
            }
            // Seal of Righteousness trigger - already computed for parent spell
            else if ( spellProto->SpellFamilyName==SPELLFAMILY_PALADIN && spellProto->SpellIconID==25 && spellProto->AttributesEx4 & 0x00800000LL )
            {
                return pdamage;
            }
            break;
        case  SPELLFAMILY_SHAMAN:
            // totem attack
            if (spellProto->SpellFamilyFlags & 0x000040000000LL)
            {
                if (spellProto->SpellIconID == 33)          // Fire Nova totem attack must be 21.4%(untested)
                    CastingTime = 749;                      // ignore CastingTime and use as modifier
                else if (spellProto->SpellIconID == 680)    // Searing Totem attack 8%
                    CastingTime = 280;                      // ignore CastingTime and use as modifier
                else if (spellProto->SpellIconID == 37)     // Magma totem attack must be 6.67%(untested)
                    CastingTime = 234;                      // ignore CastingTimePenalty and use as modifier
            }
            // Lightning Shield (and proc shield from T2 8 pieces bonus ) 33% per charge
            else if( (spellProto->SpellFamilyFlags & 0x00000000400LL) || spellProto->Id == 23552)
                CastingTime = 1155;                         // ignore CastingTimePenalty and use as modifier
            break;
        case SPELLFAMILY_PRIEST:
            // Mana Burn - 0% of Shadow Damage
            if((spellProto->SpellFamilyFlags & 0x10LL) && spellProto->SpellIconID == 212)
            {
                CastingTime = 0;
            }
            // Mind Flay - 59% of Shadow Damage
            else if((spellProto->SpellFamilyFlags & 0x800000LL) && spellProto->SpellIconID == 548)
            {
                CastingTime = 2065;
            }
            // Holy Fire - 86.71%, DoT - 16.5%
            else if ((spellProto->SpellFamilyFlags & 0x100000LL) && spellProto->SpellIconID == 156)
            {
                DotFactor = damagetype == DOT ? 0.165f : 1.0f;
                CastingTime = damagetype == DOT ? 3500 : 3035;
            }
            // Shadowguard - 28% per charge
            else if ((spellProto->SpellFamilyFlags & 0x2000000LL) && spellProto->SpellIconID == 19)
            {
                CastingTime = 980;
            }
            // Touch of Weakeness - 10%
            else if ((spellProto->SpellFamilyFlags & 0x80000LL) && spellProto->SpellIconID == 1591)
            {
                CastingTime = 350;
            }
            // Reflective Shield (back damage) - 0% (other spells fit to check not have damage effects/auras)
            else if (spellProto->SpellFamilyFlags == 0 && spellProto->SpellIconID == 566)
            {
                CastingTime = 0;
            }
            // Holy Nova - 14%
            else if ((spellProto->SpellFamilyFlags & 0x400000LL) && spellProto->SpellIconID == 1874)
            {
                CastingTime = 500;
            }
            break;
        case SPELLFAMILY_DRUID:
            // Hurricane triggered spell
            if((spellProto->SpellFamilyFlags & 0x400000LL) && spellProto->SpellIconID == 220)
            {
                CastingTime = 500;
            }
            break;
        case SPELLFAMILY_WARRIOR:
        case SPELLFAMILY_HUNTER:
        case SPELLFAMILY_ROGUE:
            CastingTime = 0;
            break;
        default:
            break;
    }

    float LvlPenalty = CalculateLevelPenalty(spellProto);

    // Spellmod SpellDamage
    //float SpellModSpellDamage = 100.0f;
    float CoefficientPtc = DotFactor * 100.0f;
    if(spellProto->SchoolMask != SPELL_SCHOOL_MASK_NORMAL)
        CoefficientPtc *= ((float)CastingTime/3500.0f);

    if(Player* modOwner = GetSpellModOwner())
        //modOwner->ApplySpellMod(spellProto->Id,SPELLMOD_SPELL_BONUS_DAMAGE,SpellModSpellDamage);
        modOwner->ApplySpellMod(spellProto->Id,SPELLMOD_SPELL_BONUS_DAMAGE,CoefficientPtc);

    //SpellModSpellDamage /= 100.0f;
    CoefficientPtc /= 100.0f;

    //float DoneActualBenefit = DoneAdvertisedBenefit * (CastingTime / 3500.0f) * DotFactor * SpellModSpellDamage * LvlPenalty;

    float DoneActualBenefit = DoneAdvertisedBenefit * CoefficientPtc * LvlPenalty;
    float TakenActualBenefit = TakenAdvertisedBenefit * DotFactor * LvlPenalty;
    if(spellProto->SpellFamilyName && spellProto->SchoolMask != SPELL_SCHOOL_MASK_NORMAL)
        TakenActualBenefit *= ((float)CastingTime / 3500.0f);

    float tmpDamage = (float(pdamage)+DoneActualBenefit)*DoneTotalMod;

    // Add flat bonus from spell damage versus
    tmpDamage += GetTotalAuraModifierByMiscMask(SPELL_AURA_MOD_FLAT_SPELL_DAMAGE_VERSUS, creatureTypeMask);

    // apply spellmod to Done damage
    if(Player* modOwner = GetSpellModOwner())
        modOwner->ApplySpellMod(spellProto->Id, damagetype == DOT ? SPELLMOD_DOT : SPELLMOD_DAMAGE, tmpDamage);

    tmpDamage = (tmpDamage+TakenActualBenefit)*TakenTotalMod;

    if( GetTypeId() == TYPEID_UNIT && !(this->ToCreature())->IsPet() )
        tmpDamage *= (this->ToCreature())->GetSpellDamageMod((this->ToCreature())->GetCreatureInfo()->rank);

    return tmpDamage > 0 ? uint32(tmpDamage) : 0;
}

int32 Unit::SpellBaseDamageBonus(SpellSchoolMask schoolMask, Unit* pVictim)
{
    int32 DoneAdvertisedBenefit = 0;

    // ..done
    AuraList const& mDamageDone = GetAurasByType(SPELL_AURA_MOD_DAMAGE_DONE);
    for(AuraList::const_iterator i = mDamageDone.begin();i != mDamageDone.end(); ++i)
        if(((*i)->GetModifier()->m_miscvalue & schoolMask) != 0 &&
        (*i)->GetSpellProto()->EquippedItemClass == -1 &&
                                                            // -1 == any item class (not wand then)
        (*i)->GetSpellProto()->EquippedItemInventoryTypeMask == 0 )
                                                            // 0 == any inventory type (not wand then)
            DoneAdvertisedBenefit += (*i)->GetModifierValue();

    if (GetTypeId() == TYPEID_PLAYER)
    {
        // Damage bonus from stats
        AuraList const& mDamageDoneOfStatPercent = GetAurasByType(SPELL_AURA_MOD_SPELL_DAMAGE_OF_STAT_PERCENT);
        for(AuraList::const_iterator i = mDamageDoneOfStatPercent.begin();i != mDamageDoneOfStatPercent.end(); ++i)
        {
            if((*i)->GetModifier()->m_miscvalue & schoolMask)
            {
                SpellEntry const* iSpellProto = (*i)->GetSpellProto();
                uint8 eff = (*i)->GetEffIndex();

                // stat used dependent from next effect aura SPELL_AURA_MOD_SPELL_HEALING presence and misc value (stat index)
                Stats usedStat = STAT_INTELLECT;
                if(eff < 2 && iSpellProto->EffectApplyAuraName[eff+1]==SPELL_AURA_MOD_SPELL_HEALING_OF_STAT_PERCENT)
                    usedStat = Stats(iSpellProto->EffectMiscValue[eff+1]);

                DoneAdvertisedBenefit += int32(GetStat(usedStat) * (*i)->GetModifierValue() / 100.0f);
            }
        }
        // ... and attack power
        AuraList const& mDamageDonebyAP = GetAurasByType(SPELL_AURA_MOD_SPELL_DAMAGE_OF_ATTACK_POWER);
        for(AuraList::const_iterator i =mDamageDonebyAP.begin();i != mDamageDonebyAP.end(); ++i)
            if ((*i)->GetModifier()->m_miscvalue & schoolMask)
                DoneAdvertisedBenefit += int32(GetTotalAttackPowerValue(BASE_ATTACK, pVictim) * (*i)->GetModifierValue() / 100.0f);

    }
    return DoneAdvertisedBenefit;
}

int32 Unit::SpellBaseDamageBonusForVictim(SpellSchoolMask schoolMask, Unit *pVictim)
{
    uint32 creatureTypeMask = pVictim->GetCreatureTypeMask();

    int32 TakenAdvertisedBenefit = 0;
    // ..done (for creature type by mask) in taken
    AuraList const& mDamageDoneCreature = GetAurasByType(SPELL_AURA_MOD_DAMAGE_DONE_CREATURE);
    for(AuraList::const_iterator i = mDamageDoneCreature.begin();i != mDamageDoneCreature.end(); ++i)
        if(creatureTypeMask & uint32((*i)->GetModifier()->m_miscvalue))
            TakenAdvertisedBenefit += (*i)->GetModifierValue();

    // ..taken
    AuraList const& mDamageTaken = pVictim->GetAurasByType(SPELL_AURA_MOD_DAMAGE_TAKEN);
    for(AuraList::const_iterator i = mDamageTaken.begin();i != mDamageTaken.end(); ++i)
        if(((*i)->GetModifier()->m_miscvalue & schoolMask) != 0)
            TakenAdvertisedBenefit += (*i)->GetModifierValue();

    return TakenAdvertisedBenefit;
}

bool Unit::isSpellCrit(Unit *pVictim, SpellEntry const *spellProto, SpellSchoolMask schoolMask, WeaponAttackType attackType)
{        
    // Mobs can't crit except for totems
    if (IS_CREATURE_GUID(GetGUID()))
    {
        uint32 owner_guid = GetOwnerGUID();
        if(IS_PLAYER_GUID(owner_guid))
        {
            Player* owner = GetPlayer(owner_guid);
            Creature* c = ToCreature();
            if(owner && c && c->isTotem())
                return owner->isSpellCrit(pVictim,spellProto,schoolMask,attackType);
        }
        return false;
    }

    // not critting spell
    if((spellProto->AttributesEx2 & SPELL_ATTR_EX2_CANT_CRIT))
        return false;
    
    for(int i=0;i<3;++i)
    {
        switch (spellProto->Effect[i])
        {
            // NPCs cannot crit with school damage spells
            case SPELL_EFFECT_SCHOOL_DAMAGE:
            {
                if (!GetCharmerOrOwnerPlayerOrPlayerItself())
                    return false;
                break;
            }
            // Leech spells are not considered as direct spell damage ( they cannot crit )
            case SPELL_EFFECT_HEALTH_LEECH:
                return false;
        }
    }

    float crit_chance = 0.0f;
    switch(spellProto->DmgClass)
    {
        case SPELL_DAMAGE_CLASS_NONE:
            return false;
        case SPELL_DAMAGE_CLASS_MAGIC:
        {
            if (schoolMask & SPELL_SCHOOL_MASK_NORMAL)
                crit_chance = 0.0f;
            // For other schools
            else if (GetTypeId() == TYPEID_PLAYER)
                crit_chance = GetFloatValue( PLAYER_SPELL_CRIT_PERCENTAGE1 + GetFirstSchoolInMask(schoolMask));
            else
            {
                crit_chance = m_baseSpellCritChance;
                crit_chance += GetTotalAuraModifierByMiscMask(SPELL_AURA_MOD_SPELL_CRIT_CHANCE_SCHOOL, schoolMask);
            }
            // taken
            if (pVictim && !IsPositiveSpell(spellProto->Id,!IsFriendlyTo(pVictim)))
            {
                // Modify critical chance by victim SPELL_AURA_MOD_ATTACKER_SPELL_CRIT_CHANCE
                crit_chance += pVictim->GetTotalAuraModifierByMiscMask(SPELL_AURA_MOD_ATTACKER_SPELL_CRIT_CHANCE, schoolMask);
                // Modify critical chance by victim SPELL_AURA_MOD_ATTACKER_SPELL_AND_WEAPON_CRIT_CHANCE
                crit_chance += pVictim->GetTotalAuraModifier(SPELL_AURA_MOD_ATTACKER_SPELL_AND_WEAPON_CRIT_CHANCE);
                // Modify by player victim resilience
                if (pVictim->GetTypeId() == TYPEID_PLAYER)
                    crit_chance -= (pVictim->ToPlayer())->GetRatingBonusValue(CR_CRIT_TAKEN_SPELL);
                // scripted (increase crit chance ... against ... target by x%
                if(pVictim->isFrozen()) // Shatter
                {
                    AuraList const& mOverrideClassScript = GetAurasByType(SPELL_AURA_OVERRIDE_CLASS_SCRIPTS);
                    for(AuraList::const_iterator i = mOverrideClassScript.begin(); i != mOverrideClassScript.end(); ++i)
                    {
                        switch((*i)->GetModifier()->m_miscvalue)
                        {
                            case 849: crit_chance+= 10.0f; break; //Shatter Rank 1
                            case 910: crit_chance+= 20.0f; break; //Shatter Rank 2
                            case 911: crit_chance+= 30.0f; break; //Shatter Rank 3
                            case 912: crit_chance+= 40.0f; break; //Shatter Rank 4
                            case 913: crit_chance+= 50.0f; break; //Shatter Rank 5
                        }
                    }
                }
                // arcane potency
                if (HasAura(12536,0) || HasAura(12043,0)) { // clearcasting or presence of mind
                    if (HasSpell(31571)) crit_chance+= 10.0f;
                    if (HasSpell(31572)) crit_chance+= 20.0f;
                    if (HasSpell(31573)) crit_chance+= 30.0f;
                }
            }
            break;
        }
        case SPELL_DAMAGE_CLASS_MELEE:
        case SPELL_DAMAGE_CLASS_RANGED:
        {
            if (pVictim)
            {
                crit_chance = GetUnitCriticalChance(attackType, pVictim);
                crit_chance+= (int32(GetMaxSkillValueForLevel(pVictim)) - int32(pVictim->GetDefenseSkillValue(this))) * 0.04f;
                crit_chance+= GetTotalAuraModifierByMiscMask(SPELL_AURA_MOD_SPELL_CRIT_CHANCE_SCHOOL, schoolMask);
                // always crit against a sitting target (except 0 crit chance)
                if(crit_chance > 0 && !pVictim->IsStandState())
                {
                   return true;
                }
            }
            break;
        }
        default:
            return false;
    }
    // percent done
    // only players use intelligence for critical chance computations
    if(Player* modOwner = GetSpellModOwner())
        modOwner->ApplySpellMod(spellProto->Id, SPELLMOD_CRITICAL_CHANCE, crit_chance);

    crit_chance = crit_chance > 0.0f ? crit_chance : 0.0f;
    bool success = roll_chance_f(crit_chance);

    return success;
}

uint32 Unit::SpellCriticalBonus(SpellEntry const *spellProto, uint32 damage, Unit *pVictim)
{
    // Calculate critical bonus
    int32 crit_bonus;
    switch(spellProto->DmgClass)
    {
        case SPELL_DAMAGE_CLASS_MELEE:                      // for melee based spells is 100%
        case SPELL_DAMAGE_CLASS_RANGED:
            // TODO: write here full calculation for melee/ranged spells
            crit_bonus = damage;
            break;
        default:
            crit_bonus = damage / 2;                        // for spells is 50%
            break;
    }
    
    int mod_critDamageBonus = GetTotalAuraModifier(SPELL_AURA_MOD_CRIT_DAMAGE_BONUS_MELEE);
    crit_bonus += int32(crit_bonus * mod_critDamageBonus) / 100.0f;

    // adds additional damage to crit_bonus (from talents)
    if(Player* modOwner = GetSpellModOwner())
        modOwner->ApplySpellMod(spellProto->Id, SPELLMOD_CRIT_DAMAGE_BONUS, crit_bonus);

    if(pVictim)
    {
        uint32 creatureTypeMask = pVictim->GetCreatureTypeMask();
        crit_bonus = int32(crit_bonus * GetTotalAuraMultiplierByMiscMask(SPELL_AURA_MOD_CRIT_PERCENT_VERSUS, creatureTypeMask));
    }

    if(crit_bonus > 0)
        damage += crit_bonus;

    return damage;
}

void Unit::ApplySpellHealingCasterModifiers(SpellEntry const *spellProto, DamageEffectType damageType, int& healpower, float& healcoef, int32& flathealbonus)
{
    if (spellProto && spellProto->AttributesEx3 & SPELL_ATTR_EX3_NO_DONE_BONUS)
        return;

    // For totems get healing bonus from owner (statue isn't totem in fact)
    if( GetTypeId()==TYPEID_UNIT && (this->ToCreature())->isTotem() && ((Totem*)this)->GetTotemType()!=TOTEM_STATUE)
        if(Unit* owner = GetOwner())
        {
            if(owner != this)
            {
                owner->ApplySpellHealingCasterModifiers(spellProto, damageType, healpower, healcoef, flathealbonus);
                return;
            }
        }

    healpower += SpellBaseHealingBonus(GetSpellSchoolMask(spellProto));

    AuraList const& mHealingDonePct = GetAurasByType(SPELL_AURA_MOD_HEALING_DONE_PERCENT);
    for(AuraList::const_iterator i = mHealingDonePct.begin();i != mHealingDonePct.end(); ++i)
        healcoef *= (100.0f + (*i)->GetModifierValue()) / 100.0f;

    // apply spellmod to Done amount
    if(Player* modOwner = GetSpellModOwner())
    {
        uint32 percentIncrease = modOwner->GetTotalPctMods(spellProto->Id, damageType == DOT ? SPELLMOD_DOT : SPELLMOD_DAMAGE);
        healcoef *= (100.0f + percentIncrease) / 100.0f;
        flathealbonus += modOwner->GetTotalFlatMods(spellProto->Id, damageType == DOT ? SPELLMOD_DOT : SPELLMOD_DAMAGE);
    }
}

void Unit::ApplySpellHealingTargetModifiers(SpellEntry const *spellProto, DamageEffectType /* damageType */, int& healpower, float& healcoef, int32& flathealbonus, Unit *pVictim)
{
    healpower += SpellBaseHealingBonusForVictim(GetSpellSchoolMask(spellProto),pVictim);

    // Blessing of Light dummy effects healing taken from Holy Light and Flash of Light
    if (spellProto->SpellFamilyName == SPELLFAMILY_PALADIN && (spellProto->SpellFamilyFlags & 0x00000000C0000000LL))
    {
        AuraList const& mDummyAuras = pVictim->GetAurasByType(SPELL_AURA_DUMMY);
        for(AuraList::const_iterator i = mDummyAuras.begin();i != mDummyAuras.end(); ++i)
        {
            if((*i)->GetSpellProto()->SpellVisual == 9180)
            {
                // Flash of Light
                if ((spellProto->SpellFamilyFlags & 0x0000000040000000LL) && (*i)->GetEffIndex() == 1)
                    healpower += (*i)->GetModifier()->m_amount;
                // Holy Light
                else if ((spellProto->SpellFamilyFlags & 0x0000000080000000LL) && (*i)->GetEffIndex() == 0)
                    healpower += (*i)->GetModifier()->m_amount;
            }
            // Libram of the Lightbringer
            else if ((*i)->GetSpellProto()->Id == 34231)
            {
                // Holy Light
                if ((spellProto->SpellFamilyFlags & 0x0000000080000000LL))
                    healpower += (*i)->GetModifier()->m_amount;
            }
            // Blessed Book of Nagrand || Libram of Light || Libram of Divinity
            else if ((*i)->GetSpellProto()->Id == 32403 || (*i)->GetSpellProto()->Id == 28851 || (*i)->GetSpellProto()->Id == 28853)
            {
                // Flash of Light
                if ((spellProto->SpellFamilyFlags & 0x0000000040000000LL))
                    healpower += (*i)->GetModifier()->m_amount;
            }
        }
    }

    // Healing Wave cast (these are dummy auras)
    if (spellProto->SpellFamilyName == SPELLFAMILY_SHAMAN && spellProto->SpellFamilyFlags & 0x0000000000000040LL)
    {
        // Search for Healing Way on Victim (stack up to 3 time)
        Unit::AuraList const& auraDummy = pVictim->GetAurasByType(SPELL_AURA_DUMMY);
        for(Unit::AuraList::const_iterator itr = auraDummy.begin(); itr!=auraDummy.end(); ++itr)
        {
            if((*itr)->GetId() == 29203)
            {
                uint32 percentIncrease = (*itr)->GetModifier()->m_amount * (*itr)->GetStackAmount();
                healcoef *= (100.0f + percentIncrease) /100.0f;
                break;
            }
        }
    }

    // Healing taken percent
    float minval = pVictim->GetMaxNegativeAuraModifier(SPELL_AURA_MOD_HEALING_PCT);
    if(minval)
        healcoef *= (100.0f + minval) / 100.0f;

    float maxval = pVictim->GetMaxPositiveAuraModifier(SPELL_AURA_MOD_HEALING_PCT);
    if(maxval)
        healcoef *= (100.0f + maxval) / 100.0f;
}

uint32 Unit::SpellHealBenefitForHealingPower(SpellEntry const *spellProto, int healpower, DamageEffectType damagetype)
{
    if(healpower == 0) return 0;

    uint32 CastingTime = GetSpellCastTime(spellProto);

    // Healing over Time spells
    float DotFactor = 1.0f;
    if(damagetype == DOT)
    {
        int32 DotDuration = GetSpellDuration(spellProto);
        if(DotDuration > 0)
        {
            // 200% limit
            if(DotDuration > 30000) DotDuration = 30000;
            if(!IsChanneledSpell(spellProto)) DotFactor = DotDuration / 15000.0f;
            int x = 0;
            for(int j = 0; j < 3; j++)
            {
                if( spellProto->Effect[j] == SPELL_EFFECT_APPLY_AURA && (
                    spellProto->EffectApplyAuraName[j] == SPELL_AURA_PERIODIC_HEAL ||
                    spellProto->EffectApplyAuraName[j] == SPELL_AURA_PERIODIC_LEECH) )
                {
                    x = j;
                    break;
                }
            }
            int DotTicks = 6;
            if(spellProto->EffectAmplitude[x] != 0)
                DotTicks = DotDuration / spellProto->EffectAmplitude[x];
            if(DotTicks)
                healpower /= DotTicks;
        }
    }

    // distribute healing to all effects, reduce AoE damage
    CastingTime = GetCastingTimeForBonus( spellProto, damagetype, CastingTime );

    // 0% bonus for damage and healing spells for leech spells from healing bonus
    for(int j = 0; j < 3; ++j)
    {
        if( spellProto->Effect[j] == SPELL_EFFECT_HEALTH_LEECH ||
            spellProto->Effect[j] == SPELL_EFFECT_APPLY_AURA && spellProto->EffectApplyAuraName[j] == SPELL_AURA_PERIODIC_LEECH )
        {
            CastingTime = 0;
            break;
        }
    }

    // Exception
    switch (spellProto->SpellFamilyName)
    {
        case  SPELLFAMILY_SHAMAN:
            // Healing stream from totem (add 6% per tick from hill bonus owner)
            if (spellProto->SpellFamilyFlags & 0x000000002000LL)
                CastingTime = 210;
            // Earth Shield 30% per charge
            else if (spellProto->SpellFamilyFlags & 0x40000000000LL)
                CastingTime = 1050;
            break;
        case  SPELLFAMILY_DRUID:
            // Lifebloom
            if (spellProto->SpellFamilyFlags & 0x1000000000LL)
            {
                CastingTime = damagetype == DOT ? 3500 : 1200;
                DotFactor = damagetype == DOT ? 0.519f : 1.0f;
            }
            // Tranquility triggered spell
            else if (spellProto->SpellFamilyFlags & 0x80LL)
                CastingTime = 667;
            // Rejuvenation
            else if (spellProto->SpellFamilyFlags & 0x10LL)
                DotFactor = 0.845f;
            // Regrowth
            else if (spellProto->SpellFamilyFlags & 0x40LL)
            {
                DotFactor = damagetype == DOT ? 0.705f : 1.0f;
                CastingTime = damagetype == DOT ? 3500 : 1010;
            }
            // Improved Leader of the Pack
            else if (spellProto->AttributesEx2 == 536870912 && spellProto->SpellIconID == 312
                && spellProto->AttributesEx3 == 33554432)
            {
                CastingTime = 0;
            }
            break;
        case SPELLFAMILY_PRIEST:
            // Holy Nova - 14%
            if ((spellProto->SpellFamilyFlags & 0x8000000LL) && spellProto->SpellIconID == 1874)
                CastingTime = 500;
            break;
        case SPELLFAMILY_PALADIN:
            // Seal and Judgement of Light
            if (spellProto->SpellFamilyFlags & 0x100040000LL)
                CastingTime = 0;
            break;
        case SPELLFAMILY_WARRIOR:
        case SPELLFAMILY_ROGUE:
        case SPELLFAMILY_HUNTER:
            CastingTime = 0;
            break;
    }

    float LvlPenalty = CalculateLevelPenalty(spellProto);

    // Spellmod SpellDamage
    //float SpellModSpellDamage = 100.0f;
    float CoefficientPtc = ((float)CastingTime/3500.0f)*DotFactor*100.0f;

    if(Player* modOwner = GetSpellModOwner())
        //modOwner->ApplySpellMod(spellProto->Id,SPELLMOD_SPELL_BONUS_DAMAGE,SpellModSpellDamage);
        modOwner->ApplySpellMod(spellProto->Id,SPELLMOD_SPELL_BONUS_DAMAGE,CoefficientPtc);

    //SpellModSpellDamage /= 100.0f;
    CoefficientPtc /= 100.0f;

    //ActualBenefit = (float)AdvertisedBenefit * ((float)CastingTime / 3500.0f) * DotFactor * SpellModSpellDamage * LvlPenalty;
    return (float)healpower * CoefficientPtc * LvlPenalty;
}

uint32 Unit::SpellHealingBonus(SpellEntry const *spellProto, uint32 healamount, DamageEffectType damageType, Unit *pVictim)
{
    // These Spells are doing fixed amount of healing (TODO found less hack-like check)
    if (spellProto->Id == 15290 || spellProto->Id == 39373 ||
        spellProto->Id == 33778 || spellProto->Id == 379   ||
        spellProto->Id == 38395 || spellProto->Id == 40972 ||
        spellProto->Id == 22845 || spellProto->Id == 33504 ||
        spellProto->Id == 34299 || spellProto->Id == 27813 ||
        spellProto->Id == 27817 || spellProto->Id == 27818 ||
        spellProto->Id == 30294 || spellProto->Id == 18790 ||
        spellProto->Id == 5707 ||
        spellProto->Id == 31616 || spellProto->Id == 37382 ||
        spellProto->Id == 38325 )
    {
        float heal = float(healamount);
        float minval = pVictim->GetMaxNegativeAuraModifier(SPELL_AURA_MOD_HEALING_PCT);
        if(minval)
            heal *= (100.0f + minval) / 100.0f;

        float maxval = pVictim->GetMaxPositiveAuraModifier(SPELL_AURA_MOD_HEALING_PCT);
        if(maxval)
            heal *= (100.0f + maxval) / 100.0f;

        if (heal < 0) heal = 0;

        return uint32(heal);
    }

    int healpower = 0;
    float healcoef = 1.0f;
    int32 flathealbonus = 0;

    ApplySpellHealingCasterModifiers(spellProto,damageType,healpower,healcoef,flathealbonus);
    if(pVictim)
        ApplySpellHealingTargetModifiers(spellProto,damageType,healpower,healcoef,flathealbonus,pVictim);

    healamount += SpellHealBenefitForHealingPower(spellProto,healpower,damageType);
    healamount += flathealbonus;
    healamount *= healcoef;

    return healamount;
}

int32 Unit::SpellBaseHealingBonus(SpellSchoolMask schoolMask)
{
    int32 AdvertisedBenefit = 0;

    AuraList const& mHealingDone = GetAurasByType(SPELL_AURA_MOD_HEALING_DONE);
    for(AuraList::const_iterator i = mHealingDone.begin();i != mHealingDone.end(); ++i)
        if(((*i)->GetModifier()->m_miscvalue & schoolMask) != 0)
            AdvertisedBenefit += (*i)->GetModifierValue();

    // Healing bonus of spirit, intellect and strength
    if (GetTypeId() == TYPEID_PLAYER)
    {
        // Healing bonus from stats
        AuraList const& mHealingDoneOfStatPercent = GetAurasByType(SPELL_AURA_MOD_SPELL_HEALING_OF_STAT_PERCENT);
        for(AuraList::const_iterator i = mHealingDoneOfStatPercent.begin();i != mHealingDoneOfStatPercent.end(); ++i)
        {
            // stat used dependent from misc value (stat index)
            Stats usedStat = Stats((*i)->GetSpellProto()->EffectMiscValue[(*i)->GetEffIndex()]);
            AdvertisedBenefit += int32(GetStat(usedStat) * (*i)->GetModifierValue() / 100.0f);
        }

        // ... and attack power
        AuraList const& mHealingDonebyAP = GetAurasByType(SPELL_AURA_MOD_SPELL_HEALING_OF_ATTACK_POWER);
        for(AuraList::const_iterator i = mHealingDonebyAP.begin();i != mHealingDonebyAP.end(); ++i)
            if ((*i)->GetModifier()->m_miscvalue & schoolMask)
                AdvertisedBenefit += int32(GetTotalAttackPowerValue(BASE_ATTACK) * (*i)->GetModifierValue() / 100.0f);
    }
    return AdvertisedBenefit;
}

int32 Unit::SpellBaseHealingBonusForVictim(SpellSchoolMask schoolMask, Unit *pVictim)
{
    int32 AdvertisedBenefit = 0;
    AuraList const& mDamageTaken = pVictim->GetAurasByType(SPELL_AURA_MOD_HEALING);
    for(AuraList::const_iterator i = mDamageTaken.begin();i != mDamageTaken.end(); ++i)
    {
        if(((*i)->GetModifier()->m_miscvalue & schoolMask) != 0)
            AdvertisedBenefit += (*i)->GetModifierValue();
        if((*i)->GetId() == 34123)
        {
            if((*i)->GetCaster() && (*i)->GetCaster()->GetTypeId() == TYPEID_PLAYER)
                AdvertisedBenefit += int32(0.25f * ((*i)->GetCaster()->ToPlayer())->GetStat(STAT_SPIRIT));
        }
    }
    return AdvertisedBenefit;
}

bool Unit::IsImmunedToDamage(SpellSchoolMask shoolMask, bool useCharges)
{
    //If m_immuneToSchool type contain this school type, IMMUNE damage.
    SpellImmuneList const& schoolList = m_spellImmune[IMMUNITY_SCHOOL];
    for (SpellImmuneList::const_iterator itr = schoolList.begin(); itr != schoolList.end(); ++itr)
        if(itr->type & shoolMask)
            return true;

    //If m_immuneToDamage type contain magic, IMMUNE damage.
    SpellImmuneList const& damageList = m_spellImmune[IMMUNITY_DAMAGE];
    for (SpellImmuneList::const_iterator itr = damageList.begin(); itr != damageList.end(); ++itr)
        if(itr->type & shoolMask)
            return true;

    return false;
}

bool Unit::IsImmunedToSpell(SpellEntry const* spellInfo, bool useCharges)
{
    if (!spellInfo)
        return false;

    // Hack for blue dragon
    switch (spellInfo->Id)
    {
        case 45848:
        case 45838:
            return false;
    }

    //Single spells immunity
    SpellImmuneList const& idList = m_spellImmune[IMMUNITY_ID];
    for(SpellImmuneList::const_iterator itr = idList.begin(); itr != idList.end(); ++itr)
    {
        if(itr->type == spellInfo->Id)
        {
            return true;
        }
    }

    if(spellInfo->Attributes & SPELL_ATTR_UNAFFECTED_BY_INVULNERABILITY)
        return false;

    SpellImmuneList const& dispelList = m_spellImmune[IMMUNITY_DISPEL];
    for(SpellImmuneList::const_iterator itr = dispelList.begin(); itr != dispelList.end(); ++itr)
        if(itr->type == spellInfo->Dispel)
            return true;

    if( !(spellInfo->AttributesEx & SPELL_ATTR_EX_UNAFFECTED_BY_SCHOOL_IMMUNE) &&         // unaffected by school immunity
        !(spellInfo->AttributesEx & SPELL_ATTR_EX_DISPEL_AURAS_ON_IMMUNITY)               // can remove immune (by dispell or immune it)
        && (spellInfo->Id != 42292))
    {
        SpellImmuneList const& schoolList = m_spellImmune[IMMUNITY_SCHOOL];
        for(SpellImmuneList::const_iterator itr = schoolList.begin(); itr != schoolList.end(); ++itr)
            if( !(IsPositiveSpell(itr->spellId) && IsPositiveSpell(spellInfo->Id)) &&
                (itr->type & GetSpellSchoolMask(spellInfo)) )
                return true;
    }

    SpellImmuneList const& mechanicList = m_spellImmune[IMMUNITY_MECHANIC];
    for(SpellImmuneList::const_iterator itr = mechanicList.begin(); itr != mechanicList.end(); ++itr)
    {
        if(itr->type == spellInfo->Mechanic)
        {
            return true;
        }
    }

    if(ToCreature() && ToCreature()->isTotem())
        if(IsChanneledSpell(spellInfo))
            return true;

    return false;
}

bool Unit::IsImmunedToSpellEffect(uint32 effect, uint32 mechanic) const
{
    //If m_immuneToEffect type contain this effect type, IMMUNE effect.
    SpellImmuneList const& effectList = m_spellImmune[IMMUNITY_EFFECT];
    for (SpellImmuneList::const_iterator itr = effectList.begin(); itr != effectList.end(); ++itr)
        if(itr->type == effect)
            return true;

    SpellImmuneList const& mechanicList = m_spellImmune[IMMUNITY_MECHANIC];
    for (SpellImmuneList::const_iterator itr = mechanicList.begin(); itr != mechanicList.end(); ++itr)
        if(itr->type == mechanic)
            return true;

    return false;
}

bool Unit::IsDamageToThreatSpell(SpellEntry const * spellInfo) const
{
    if(!spellInfo)
        return false;

    uint32 family = spellInfo->SpellFamilyName;
    uint64 flags = spellInfo->SpellFamilyFlags;

    if((family == 5 && flags == 256) ||                     //Searing Pain
        (family == SPELLFAMILY_SHAMAN && flags == SPELLFAMILYFLAG_SHAMAN_FROST_SHOCK))
        return true;

    return false;
}

void Unit::MeleeDamageBonus(Unit *pVictim, uint32 *pdamage,WeaponAttackType attType, SpellEntry const *spellProto)
{
    if(!pVictim)
        return;

    if(*pdamage == 0)
        return;

    uint32 creatureTypeMask = pVictim->GetCreatureTypeMask();

    // Taken/Done fixed damage bonus auras
    int32 DoneFlatBenefit = 0;
    int32 TakenFlatBenefit = 0;

    // ..done (for creature type by mask) in taken
    AuraList const& mDamageDoneCreature = GetAurasByType(SPELL_AURA_MOD_DAMAGE_DONE_CREATURE);
    for(AuraList::const_iterator i = mDamageDoneCreature.begin();i != mDamageDoneCreature.end(); ++i)
        if(creatureTypeMask & uint32((*i)->GetModifier()->m_miscvalue))
            DoneFlatBenefit += (*i)->GetModifierValue();
    // ..done
    // SPELL_AURA_MOD_DAMAGE_DONE included in weapon damage

    // ..done (base at attack power for marked target and base at attack power for creature type)
    float APBonus = GetAPBonusVersus(attType, pVictim);
    if (APBonus != 0.0f)                                         // Can be negative
    {
        bool normalized = false;
        if(spellProto)
        {
            for (uint8 i = 0; i<3;i++)
            {
                if (spellProto->Effect[i] == SPELL_EFFECT_NORMALIZED_WEAPON_DMG)
                {
                    normalized = true;
                    break;
                }
            }
        }

        DoneFlatBenefit += int32((APBonus/14.0f) * GetAPMultiplier(attType,normalized));
    }

    // ..taken
    AuraList const& mDamageTaken = pVictim->GetAurasByType(SPELL_AURA_MOD_DAMAGE_TAKEN);
    for(AuraList::const_iterator i = mDamageTaken.begin();i != mDamageTaken.end(); ++i)
        if((*i)->GetModifier()->m_miscvalue & GetMeleeDamageSchoolMask())
            TakenFlatBenefit += (*i)->GetModifierValue();

    if(attType!=RANGED_ATTACK)
        TakenFlatBenefit += pVictim->GetTotalAuraModifier(SPELL_AURA_MOD_MELEE_DAMAGE_TAKEN);
    else
        TakenFlatBenefit += pVictim->GetTotalAuraModifier(SPELL_AURA_MOD_RANGED_DAMAGE_TAKEN);

    // Done/Taken total percent damage auras
    float DoneTotalMod = 1;
    float TakenTotalMod = 1;

    // ..done
    // SPELL_AURA_MOD_DAMAGE_PERCENT_DONE included in weapon damage
    // SPELL_AURA_MOD_OFFHAND_DAMAGE_PCT  included in weapon damage

    AuraList const& mDamageDoneVersus = GetAurasByType(SPELL_AURA_MOD_DAMAGE_DONE_VERSUS);
    for(AuraList::const_iterator i = mDamageDoneVersus.begin();i != mDamageDoneVersus.end(); ++i)
        if(creatureTypeMask & uint32((*i)->GetModifier()->m_miscvalue))
            DoneTotalMod *= ((*i)->GetModifierValue()+100.0f)/100.0f;
    // ..taken
    AuraList const& mModDamagePercentTaken = pVictim->GetAurasByType(SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN);
    for(AuraList::const_iterator i = mModDamagePercentTaken.begin(); i != mModDamagePercentTaken.end(); ++i) {
        if((*i)->GetModifier()->m_miscvalue & GetMeleeDamageSchoolMask()) 
            TakenTotalMod *= ((*i)->GetModifierValue()+100.0f)/100.0f;
    }
        
    // .. taken pct: dummy auras
    AuraList const& mDummyAuras = pVictim->GetAurasByType(SPELL_AURA_DUMMY);
    for(AuraList::const_iterator i = mDummyAuras.begin(); i != mDummyAuras.end(); ++i)
    {
        switch((*i)->GetSpellProto()->SpellIconID)
        {
            //Cheat Death
            case 2109:
                if((*i)->GetModifier()->m_miscvalue & SPELL_SCHOOL_MASK_NORMAL)
                {
                    if(pVictim->GetTypeId() != TYPEID_PLAYER)
                        continue;
                    float mod = (pVictim->ToPlayer())->GetRatingBonusValue(CR_CRIT_TAKEN_MELEE)*(-8.0f);
                    if (mod < (*i)->GetModifier()->m_amount)
                        mod = (*i)->GetModifier()->m_amount;
                    TakenTotalMod *= (mod+100.0f)/100.0f;
                }
                break;
            //Mangle
            case 2312:
                if(spellProto==NULL)
                    break;
                // Should increase Shred (initial Damage of Lacerate and Rake handled in Spell::EffectSchoolDMG)
                if(spellProto->SpellFamilyName==SPELLFAMILY_DRUID && (spellProto->SpellFamilyFlags==0x00008000LL))
                    TakenTotalMod *= (100.0f+(*i)->GetModifier()->m_amount)/100.0f;
                break;
        }
    }

    // .. taken pct: class scripts
    AuraList const& mclassScritAuras = GetAurasByType(SPELL_AURA_OVERRIDE_CLASS_SCRIPTS);
    for(AuraList::const_iterator i = mclassScritAuras.begin(); i != mclassScritAuras.end(); ++i)
    {
        switch((*i)->GetMiscValue())
        {
            case 6427: case 6428:                           // Dirty Deeds
                if(pVictim->HasAuraState(AURA_STATE_HEALTHLESS_35_PERCENT))
                {
                    Aura* eff0 = GetAura((*i)->GetId(),0);
                    if(!eff0 || (*i)->GetEffIndex()!=1)
                    {
                        sLog.outError("Spell structure of DD (%u) changed.",(*i)->GetId());
                        continue;
                    }

                    // effect 0 have expected value but in negative state
                    TakenTotalMod *= (-eff0->GetModifier()->m_amount+100.0f)/100.0f;
                }
                break;
        }
    }

    if(attType != RANGED_ATTACK)
    {
        AuraList const& mModMeleeDamageTakenPercent = pVictim->GetAurasByType(SPELL_AURA_MOD_MELEE_DAMAGE_TAKEN_PCT);
        for(AuraList::const_iterator i = mModMeleeDamageTakenPercent.begin(); i != mModMeleeDamageTakenPercent.end(); ++i)
            TakenTotalMod *= ((*i)->GetModifierValue()+100.0f)/100.0f;
    }
    else
    {
        AuraList const& mModRangedDamageTakenPercent = pVictim->GetAurasByType(SPELL_AURA_MOD_RANGED_DAMAGE_TAKEN_PCT);
        for(AuraList::const_iterator i = mModRangedDamageTakenPercent.begin(); i != mModRangedDamageTakenPercent.end(); ++i)
            TakenTotalMod *= ((*i)->GetModifierValue()+100.0f)/100.0f;
    }

    float tmpDamage = float(int32(*pdamage) + DoneFlatBenefit) * DoneTotalMod;

    // apply spellmod to Done damage
    if(spellProto)
    {
        if(Player* modOwner = GetSpellModOwner())
            modOwner->ApplySpellMod(spellProto->Id, SPELLMOD_DAMAGE, tmpDamage);
    }

    tmpDamage = (tmpDamage + TakenFlatBenefit)*TakenTotalMod;

    // bonus result can be negative
    *pdamage =  tmpDamage > 0 ? uint32(tmpDamage) : 0;
}

void Unit::ApplySpellImmune(uint32 spellId, uint32 op, uint32 type, bool apply)
{
    if (apply)
    {
        for (SpellImmuneList::iterator itr = m_spellImmune[op].begin(), next; itr != m_spellImmune[op].end(); itr = next)
        {
            next = itr; ++next;
            if(itr->type == type)
            {
                m_spellImmune[op].erase(itr);
                next = m_spellImmune[op].begin();
            }
        }
        SpellImmune Immune;
        Immune.spellId = spellId;
        Immune.type = type;
        m_spellImmune[op].push_back(Immune);
    }
    else
    {
        for (SpellImmuneList::iterator itr = m_spellImmune[op].begin(); itr != m_spellImmune[op].end(); ++itr)
        {
            if(itr->spellId == spellId)
            {
                m_spellImmune[op].erase(itr);
                break;
            }
        }
    }

}

void Unit::ApplySpellDispelImmunity(const SpellEntry * spellProto, DispelType type, bool apply)
{
    ApplySpellImmune(spellProto->Id,IMMUNITY_DISPEL, type, apply);

    if (apply && spellProto->AttributesEx & SPELL_ATTR_EX_DISPEL_AURAS_ON_IMMUNITY)
        RemoveAurasWithDispelType(type);
}

float Unit::GetWeaponProcChance() const
{
    // normalized proc chance for weapon attack speed
    // (odd formula...)
    if(isAttackReady(BASE_ATTACK))
        return (GetAttackTime(BASE_ATTACK) * 1.8f / 1000.0f);
    else if (haveOffhandWeapon() && isAttackReady(OFF_ATTACK))
        return (GetAttackTime(OFF_ATTACK) * 1.6f / 1000.0f);
    return 0;
}

float Unit::GetPPMProcChance(uint32 WeaponSpeed, float PPM) const
{
    // proc per minute chance calculation
    if (PPM <= 0) return 0.0f;
    uint32 result = uint32((WeaponSpeed * PPM) / 600.0f);   // result is chance in percents (probability = Speed_in_sec * (PPM / 60))
    return result;
}

void Unit::Mount(uint32 mount, bool flying)
{
    if(!mount)
        return;

    RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_MOUNT);

    SetUInt32Value(UNIT_FIELD_MOUNTDISPLAYID, mount);

    SetFlag( UNIT_FIELD_FLAGS, UNIT_FLAG_MOUNT );

    // unsummon pet
    if(GetTypeId() == TYPEID_PLAYER)
    {
        Pet* pet = GetPet();
        if(pet)
        {
            /*BattleGround *bg = (this->ToPlayer())->GetBattleGround();
            // don't unsummon pet in arena but SetFlag UNIT_FLAG_DISABLE_ROTATE to disable pet's interface*/
            if (!flying)
                pet->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_DISABLE_ROTATE);
            else
            {
                if(pet->isControlled())
                {
                    (this->ToPlayer())->SetTemporaryUnsummonedPetNumber(pet->GetCharmInfo()->GetPetNumber());
                    (this->ToPlayer())->SetOldPetSpell(pet->GetUInt32Value(UNIT_CREATED_BY_SPELL));
                }
                (this->ToPlayer())->RemovePet(NULL, PET_SAVE_NOT_IN_SLOT);
                return;
            }
        }
        (this->ToPlayer())->SetTemporaryUnsummonedPetNumber(0);
    }
}

void Unit::Unmount()
{
    if(!IsMounted())
        return;

    RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_NOT_MOUNTED);

    SetUInt32Value(UNIT_FIELD_MOUNTDISPLAYID, 0);
    RemoveFlag( UNIT_FIELD_FLAGS, UNIT_FLAG_MOUNT );

    // only resummon old pet if the player is already added to a map
    // this prevents adding a pet to a not created map which would otherwise cause a crash
    // (it could probably happen when logging in after a previous crash)
    if(GetTypeId() == TYPEID_PLAYER && IsInWorld() && IsAlive())
    {
        if( (this->ToPlayer())->GetTemporaryUnsummonedPetNumber() )
        {
            Pet* NewPet = new Pet;
            if(!NewPet->LoadPetFromDB(this, 0, (this->ToPlayer())->GetTemporaryUnsummonedPetNumber(), true))
                delete NewPet;
            (this->ToPlayer())->SetTemporaryUnsummonedPetNumber(0);
        }
        else 
           if(Pet *pPet = GetPet())
               if(pPet->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_DISABLE_ROTATE) && !pPet->HasUnitState(UNIT_STAT_STUNNED))
                   pPet->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_DISABLE_ROTATE);
    }
}

void Unit::SetInCombatWith(Unit* enemy)
{
    Unit* eOwner = enemy->GetCharmerOrOwnerOrSelf();
    if(eOwner->IsPvP())
    {
        SetInCombatState(true);
        return;
    }

    //check for duel
    if(eOwner->GetTypeId() == TYPEID_PLAYER && (eOwner->ToPlayer())->duel)
    {
        Unit const* myOwner = GetCharmerOrOwnerOrSelf();
        if(((Player const*)eOwner)->duel->opponent == myOwner)
        {
            SetInCombatState(true);
            return;
        }
    }
    SetInCombatState(false);
}

void Unit::CombatStart(Unit* target, bool updatePvP)
{
    if(!target->IsStandState()/* && !target->HasUnitState(UNIT_STAT_STUNNED)*/)
        target->SetStandState(PLAYER_STATE_NONE);

    if(!target->IsInCombat() && target->GetTypeId() != TYPEID_PLAYER
        && !(target->ToCreature())->HasReactState(REACT_PASSIVE) && (target->ToCreature())->IsAIEnabled)
    {
        (target->ToCreature())->AI()->AttackStart(this);
        if (target->ToCreature()->getAI())
            target->ToCreature()->getAI()->attackStart(this);
        if((target->ToCreature())->GetFormation())
        {   
            (target->ToCreature())->GetFormation()->MemberAttackStart(target->ToCreature(), this);
        }
        
        if (ScriptedInstance* instance = ((ScriptedInstance*)target->GetInstanceData()))
            instance->MonsterPulled(target->ToCreature(), this);
    }
    
    if (IsAIEnabled)
        RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_OOC_NOT_ATTACKABLE);

    SetInCombatWith(target);
    target->SetInCombatWith(this);
    
    // check if currently selected target is reachable
    // NOTE: path already generated from AttackStart()
    if(!GetMotionMaster()->IsReachable())
    {
        //sLog.outString("Not Reachable (%u : %s)", GetGUIDLow(), GetName());
        // remove all taunts
        RemoveSpellsCausingAura(SPELL_AURA_MOD_TAUNT); 

        if(m_ThreatManager.getThreatList().size() < 2)
        {
            // only one target in list, we have to evade after timer
            // TODO: make timer - inside Creature class
            ((Creature*)this)->AI()->EnterEvadeMode();
            if (((Creature*)this)->getAI())
                ((Creature*)this)->getAI()->evade();
        }
        else
        {
            // remove unreachable target from our threat list
            // next iteration we will select next possible target
            m_HostilRefManager.deleteReference(target);
            m_ThreatManager.modifyThreatPercent(target, -101);
                    
            _removeAttacker(target);
        }
    }
    /*else
        sLog.outString("Reachable (%u : %s)", GetGUIDLow(), GetName());*/

    Unit *who = target->GetCharmerOrOwnerOrSelf();
    if(who->GetTypeId() == TYPEID_PLAYER)
        SetContestedPvP(who->ToPlayer());

    Player *me = GetCharmerOrOwnerPlayerOrPlayerItself();
    if(updatePvP 
        && me && who->IsPvP()
        && (who->GetTypeId() != TYPEID_PLAYER
        || !me->duel || me->duel->opponent != who))
    {
        me->UpdatePvP(true);
        me->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_ENTER_PVP_COMBAT);
    }
    
    if (GetTypeId() != TYPEID_PLAYER && ToCreature()->IsPet() && GetOwner() &&
            (ToPet()->getPetType() == HUNTER_PET || GetOwner()->getClass() == CLASS_WARLOCK)) {
        GetOwner()->SetInCombatWith(target);
        target->SetInCombatWith(GetOwner());
    }

}

void Unit::SetInCombatState(bool PvP)
{
    // only alive units can be in combat
    if(!IsAlive())
        return;

    if(PvP)
        m_CombatTimer = 5250;

    if(IsInCombat())
        return;

    SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_IN_COMBAT);
    
    if(m_currentSpells[CURRENT_GENERIC_SPELL] && m_currentSpells[CURRENT_GENERIC_SPELL]->getState() != SPELL_STATE_FINISHED)
    {
        if(IsNonCombatSpell(m_currentSpells[CURRENT_GENERIC_SPELL]->m_spellInfo))
            InterruptSpell(CURRENT_GENERIC_SPELL);
    }

    if(IsNonMeleeSpellCasted(false))
        for (uint32 i = CURRENT_FIRST_NON_MELEE_SPELL; i < CURRENT_MAX_SPELL; i++)
            if(m_currentSpells[i] && IsNonCombatSpell(m_currentSpells[i]->m_spellInfo))
                InterruptSpell(i,false);

    if(GetTypeId() != TYPEID_PLAYER && GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_IDLE) != IDLE_MOTION_TYPE)
        (this->ToCreature())->SetHomePosition(GetPositionX(), GetPositionY(), GetPositionZ(), GetOrientation());

    if(GetTypeId() != TYPEID_PLAYER && (this->ToCreature())->IsPet())
    {
        /*(MOVE_RUN, true);
        UpdateSpeed(MOVE_SWIM, true);
        UpdateSpeed(MOVE_FLIGHT, true);*/
    }
    else if(!isCharmed())
        return;

    SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PET_IN_COMBAT);
}

void Unit::ClearInCombat()
{
    m_CombatTimer = 0;
    RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_IN_COMBAT);

    // Player's state will be cleared in Player::UpdateContestedPvP
    if (GetTypeId()!=TYPEID_PLAYER) {
        Creature* creature = this->ToCreature();
        if (creature->GetCreatureInfo() && creature->GetCreatureInfo()->unit_flags & UNIT_FLAG_OOC_NOT_ATTACKABLE)
            SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_OOC_NOT_ATTACKABLE);
            
        clearUnitState(UNIT_STAT_ATTACK_PLAYER);
    }

    if(GetTypeId() != TYPEID_PLAYER && (this->ToCreature())->IsPet())
    {
        if(Unit *owner = GetOwner())
        {
            for(int i = 0; i < MAX_MOVE_TYPE; ++i)
                if(owner->GetSpeedRate(UnitMoveType(i)) > m_speed_rate[UnitMoveType(i)])
                    SetSpeed(UnitMoveType(i), owner->GetSpeedRate(UnitMoveType(i)), true);
        }
    }
    else if(!isCharmed())
        return;

    RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PET_IN_COMBAT);
}

// force to skip IsFriendlyTo && feign death checks
bool Unit::canAttack(Unit const* target, bool force /*= true*/) const
{
    ASSERT(target);

    if (force) {
        if (IsFriendlyTo(target))
            return false;
    } else if (!IsHostileTo(target))
        return false;

    if(target->HasFlag(UNIT_FIELD_FLAGS,
        UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_OOC_NOT_ATTACKABLE))
        return false;

    if(target->GetTypeId()==TYPEID_PLAYER && ((target->ToPlayer())->isGameMaster() || (target->ToPlayer())->isSpectator())
       || (target->GetTypeId() == TYPEID_UNIT && target->GetEntry() == 10 && GetTypeId() != TYPEID_PLAYER && !IsPet()) //training dummies
      ) 
       return false; 

    // feign death case
    if (!force && target->HasFlag(UNIT_FIELD_FLAGS_2, UNIT_FLAG2_FEIGN_DEATH)) {
        if ((GetTypeId() != TYPEID_PLAYER && !GetOwner()) || (GetOwner() && GetOwner()->GetTypeId() != TYPEID_PLAYER))
            return false;
        // if this == player or owner == player check other conditions
    } else if (!target->IsAlive()) // real dead case ~UNIT_FLAG2_FEIGN_DEATH && UNIT_STAT_DIED
        return false;
    else if (target->getTransForm() == FORM_SPIRITOFREDEMPTION)
        return false;
    
    if (target->GetEntry() == 24892 && IsPet())
        return true;

    if ((m_invisibilityMask || target->m_invisibilityMask) && !canDetectInvisibilityOf(target))
        return false;

    if (target->GetVisibility() == VISIBILITY_GROUP_STEALTH && !canDetectStealthOf(target, GetDistance(target)))
        return false;

    return true;
}

bool Unit::isAttackableByAOE() const
{
    if(!IsAlive())
        return false;

    if(HasFlag(UNIT_FIELD_FLAGS,
        UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_OOC_NOT_ATTACKABLE))
        return false;

    if(GetTypeId()==TYPEID_PLAYER && ((this->ToPlayer())->isGameMaster() || (this->ToPlayer())->isSpectator()))
        return false;

    if(GetTypeId()==TYPEID_UNIT && (ToCreature())->isTotem())
        return false;

    if(isInFlight())
        return false;

    return true;
}

int32 Unit::ModifyHealth(int32 dVal)
{
    int32 gain = 0;

    if(dVal==0)
        return 0;
    
    // Part of Evade mechanics. Only track health lost, not gained.
    if (dVal < 0 && GetTypeId() != TYPEID_PLAYER && !IsPet())
        SetLastDamagedTime(time(NULL));

    int32 curHealth = (int32)GetHealth();

    int32 val = dVal + curHealth;

    if(val <= 0)
    {
        SetHealth(0);
        return -curHealth;
    }

    int32 maxHealth = (int32)GetMaxHealth();

    if(val < maxHealth)
    {
        SetHealth(val);
        gain = val - curHealth;
    }
    else if(curHealth != maxHealth)
    {
        SetHealth(maxHealth);
        gain = maxHealth - curHealth;
    }

    return gain;
}

// used only to calculate channeling time
void Unit::ModSpellCastTime(SpellEntry const* spellProto, int32 & castTime, Spell * spell)
{
    if (!spellProto || castTime<0)
        return;
    //called from caster
    if(Player* modOwner = GetSpellModOwner())
        modOwner->ApplySpellMod(spellProto->Id, SPELLMOD_CASTING_TIME, castTime, spell);

     if (spellProto->Attributes & SPELL_ATTR_RANGED && !(spellProto->AttributesEx2 & SPELL_ATTR_EX2_AUTOREPEAT_FLAG))
            castTime = int32 (float(castTime) * m_modAttackSpeedPct[RANGED_ATTACK]);
     else 
        if(spellProto->SpellFamilyName || (spellProto->AttributesEx5 & SPELL_ATTR_EX5_HASTE_AFFECT_DURATION) )
            castTime = int32( float(castTime) * GetFloatValue(UNIT_MOD_CAST_SPEED));
}

int32 Unit::ModifyPower(Powers power, int32 dVal)
{
    int32 gain = 0;

    if(dVal==0)
        return 0;

    int32 curPower = (int32)GetPower(power);

    int32 val = dVal + curPower;
    if(val <= 0)
    {
        SetPower(power,0);
        return -curPower;
    }

    int32 maxPower = (int32)GetMaxPower(power);

    if(val < maxPower)
    {
        SetPower(power,val);
        gain = val - curPower;
    }
    else if(curPower != maxPower)
    {
        SetPower(power,maxPower);
        gain = maxPower - curPower;
    }

    return gain;
}

bool Unit::isVisibleForOrDetect(Unit const* u, bool detect, bool inVisibleList, bool is3dDistance) const
{
    if(!u || !IsInMap(u))
        return false;

    return u->canSeeOrDetect(this, detect, inVisibleList, is3dDistance);
}

bool Unit::canSeeOrDetect(Unit const* u, bool detect, bool inVisibleList, bool is3dDistance) const
{
    return true;
}

bool Unit::canDetectInvisibilityOf(Unit const* u) const
{
    if (m_invisibilityMask & u->m_invisibilityMask) // same group
        return true;
    
    if (GetTypeId() != TYPEID_PLAYER && u->m_invisibilityMask == 0) // An entity with no invisibility is always detectable, right?
        return true;
    
    AuraList const& auras = u->GetAurasByType(SPELL_AURA_MOD_STALKED); // Hunter mark
    for (AuraList::const_iterator iter = auras.begin(); iter != auras.end(); ++iter)
        if ((*iter)->GetCasterGUID() == GetGUID())
            return true;

    // Common invisibility mask
    Unit::AuraList const& iAuras = u->GetAurasByType(SPELL_AURA_MOD_INVISIBILITY);
    Unit::AuraList const& dAuras = GetAurasByType(SPELL_AURA_MOD_INVISIBILITY_DETECTION);
    if (uint32 mask = (m_detectInvisibilityMask & u->m_invisibilityMask)) {
        for (uint32 i = 0; i < 10; ++i) {
            if (((1 << i) & mask) == 0)
                continue;

            // find invisibility level
            uint32 invLevel = 0;
            for (Unit::AuraList::const_iterator itr = iAuras.begin(); itr != iAuras.end(); ++itr)
                if (((*itr)->GetModifier()->m_miscvalue) == i && invLevel < (*itr)->GetModifier()->m_amount)
                    invLevel = (*itr)->GetModifier()->m_amount;

            // find invisibility detect level
            uint32 detectLevel = 0;
            if (i == 6 && GetTypeId() == TYPEID_PLAYER) // special drunk detection case
                detectLevel = (this->ToPlayer())->GetDrunkValue();
            else {
                for (Unit::AuraList::const_iterator itr = dAuras.begin(); itr != dAuras.end(); ++itr)
                    if (((*itr)->GetModifier()->m_miscvalue) == i && detectLevel < (*itr)->GetModifier()->m_amount)
                        detectLevel = (*itr)->GetModifier()->m_amount;
            }

            if (invLevel <= detectLevel)
                return true;
        }
    }

    return false;
}

bool Unit::canDetectStealthOf(Unit const* target, float distance) const
{
    if(GetTypeId() == TYPEID_PLAYER)
        if (ToPlayer()->isSpectator() && !sWorld.getConfig(CONFIG_ARENA_SPECTATOR_STEALTH))
            return false;
    
    if (!IsAlive())
        return false;

    if (HasAuraType(SPELL_AURA_DETECT_STEALTH))
        return true;

    AuraList const& auras = target->GetAurasByType(SPELL_AURA_MOD_STALKED); // Hunter mark
    for (AuraList::const_iterator iter = auras.begin(); iter != auras.end(); ++iter)
        if ((*iter)->GetCasterGUID() == GetGUID())
            return true;

    if(target->HasAura(18461,0)) //vanish dummy spell, 2.5s duration after vanish
        return false;
    
    if (distance == 0.0f) //collision
        return true;

    if (!HasInArc(M_PI/2.0f*3.0f, target)) // can't see 90� behind
        return false;
    
    //http://wolfendonkane.pagesperso-orange.fr/furtivite.html
    
    float visibleDistance = 17.5f;
    visibleDistance += float(getLevelForTarget(target)) - target->GetTotalAuraModifier(SPELL_AURA_MOD_STEALTH)/5.0f; //max level stealth spell have 350, so if same level and no talent/items boost, this will equal 0
    visibleDistance -= target->GetTotalAuraModifier(SPELL_AURA_MOD_STEALTH_LEVEL); //mainly from talents, improved stealth for rogue and druid add 15 yards in total (15 points). Items with Increases your effective stealth level by 1 have 5.
    visibleDistance += (float)(GetTotalAuraModifierByMiscValue(SPELL_AURA_MOD_DETECT, 0) /2.0f); //spells like Track Hidden have 30 here, so you can see 15 yards further. Spells with miscvalue != 0 aren't meant to detect units but traps only
    
    //min and max caps
    if(visibleDistance > MAX_PLAYER_STEALTH_DETECT_RANGE)
        visibleDistance = MAX_PLAYER_STEALTH_DETECT_RANGE;
    else if (visibleDistance < 2.5f)
        visibleDistance = 2.5f; //this can still be reduced with the following check

    //reduce a bit visibility depending on angle
    if(!HasInArc(M_PI,target)) //not in front (180�)
        visibleDistance = visibleDistance / 2;
    else if(!HasInArc(M_PI/2,target)) //not in 90� cone in front
        visibleDistance = visibleDistance / 1.5;
    
    return distance < visibleDistance;
}

void Unit::DestroyForNearbyPlayers()
{
    if(!IsInWorld())
        return;

    std::list<Unit*> targets;
    Trinity::AnyUnitInObjectRangeCheck check(this, GetMap()->GetVisibilityDistance());
    Trinity::UnitListSearcher<Trinity::AnyUnitInObjectRangeCheck> searcher(targets, check);
    VisitNearbyWorldObject(GetMap()->GetVisibilityDistance(), searcher);
    for(std::list<Unit*>::iterator iter = targets.begin(); iter != targets.end(); ++iter)
        if(*iter != this && (*iter)->GetTypeId() == TYPEID_PLAYER
            && ((*iter)->ToPlayer())->HaveAtClient(this))
        {
            DestroyForPlayer((*iter)->ToPlayer());
            ((*iter)->ToPlayer())->m_clientGUIDs.erase(GetGUID());
        }
}

void Unit::SetVisibility(UnitVisibility x)
{
    m_Visibility = x;

    if(IsInWorld())
        SetToNotify();

    if(x == VISIBILITY_GROUP_STEALTH)
        DestroyForNearbyPlayers();
}

void Unit::UpdateSpeed(UnitMoveType mtype, bool forced, bool withPet /*= true*/)
{
    int32 main_speed_mod  = 0;
    float stack_bonus     = 1.0f;
    float non_stack_bonus = 1.0f;

    switch(mtype)
    {
        case MOVE_WALK:
            return;
        case MOVE_RUN:
        {
            if (IsMounted()) // Use on mount auras
            {
                main_speed_mod  = GetMaxPositiveAuraModifier(SPELL_AURA_MOD_INCREASE_MOUNTED_SPEED);
                stack_bonus     = GetTotalAuraMultiplier(SPELL_AURA_MOD_MOUNTED_SPEED_ALWAYS);
                non_stack_bonus = (100.0f + GetMaxPositiveAuraModifier(SPELL_AURA_MOD_MOUNTED_SPEED_NOT_STACK))/100.0f;
            }
            else
            {
                main_speed_mod  = GetMaxPositiveAuraModifier(SPELL_AURA_MOD_INCREASE_SPEED);
                stack_bonus     = GetTotalAuraMultiplier(SPELL_AURA_MOD_SPEED_ALWAYS);
                non_stack_bonus = (100.0f + GetMaxPositiveAuraModifier(SPELL_AURA_MOD_SPEED_NOT_STACK))/100.0f;
            }
            break;
        }
        case MOVE_RUN_BACK:
            return;
        case MOVE_SWIM:
        {
            main_speed_mod  = GetMaxPositiveAuraModifier(SPELL_AURA_MOD_INCREASE_SWIM_SPEED);
            break;
        }
        case MOVE_SWIM_BACK:
            return;
        case MOVE_FLIGHT:
        {
            if (IsMounted()) // Use on mount auras
                main_speed_mod  = GetMaxPositiveAuraModifier(SPELL_AURA_MOD_INCREASE_FLIGHT_SPEED);
            else             // Use not mount (shapeshift for example) auras (should stack)
                main_speed_mod  = GetTotalAuraModifier(SPELL_AURA_MOD_SPEED_FLIGHT);
            stack_bonus     = GetTotalAuraMultiplier(SPELL_AURA_MOD_FLIGHT_SPEED_ALWAYS);
            non_stack_bonus = (100.0 + GetMaxPositiveAuraModifier(SPELL_AURA_MOD_FLIGHT_SPEED_NOT_STACK))/100.0f;
            break;
        }
        case MOVE_FLIGHT_BACK:
            return;
        default:
            sLog.outError("Unit::UpdateSpeed: Unsupported move type (%d)", mtype);
            return;
    }

    float bonus = non_stack_bonus > stack_bonus ? non_stack_bonus : stack_bonus;
    // now we ready for speed calculation
    float speed  = main_speed_mod ? bonus*(100.0f + main_speed_mod)/100.0f : bonus;

    switch(mtype)
    {
        case MOVE_RUN:
        case MOVE_SWIM:
        case MOVE_FLIGHT:
        {
            // Normalize speed by 191 aura SPELL_AURA_USE_NORMAL_MOVEMENT_SPEED if need
            // TODO: possible affect only on MOVE_RUN
            if(int32 normalization = GetMaxPositiveAuraModifier(SPELL_AURA_USE_NORMAL_MOVEMENT_SPEED))
            {
                // Use speed from aura
                float max_speed = normalization / baseMoveSpeed[mtype];
                if (speed > max_speed)
                    speed = max_speed;
            }
            break;
        }
        default:
            break;
    }

    // Apply strongest slow aura mod to speed
    int32 slow = GetMaxNegativeAuraModifier(SPELL_AURA_MOD_DECREASE_SPEED);
    if (slow)
        speed *=(100.0f + slow)/100.0f;
    SetSpeed(mtype, speed, forced, withPet);
}

/* return true speed */
float Unit::GetSpeed( UnitMoveType mtype ) const
{
    return m_speed_rate[mtype]*baseMoveSpeed[mtype];
}

/* Set speed rate of unit */
void Unit::SetSpeed(UnitMoveType mtype, float rate, bool forced, bool withPet /*= true*/)
{
    if (rate < 0)
        rate = 0.0f;

    // Update speed only on change
    if (m_speed_rate[mtype] == rate)
        return;

    m_speed_rate[mtype] = rate;

    propagateSpeedChange();

    // Send speed change packet only for player
    if (GetTypeId()!=TYPEID_PLAYER)
        return;

    WorldPacket data;
    if(!forced)
    {
        switch(mtype)
        {
            case MOVE_WALK:
                data.Initialize(MSG_MOVE_SET_WALK_SPEED, 8+4+1+4+4+4+4+4+4+4);
                break;
            case MOVE_RUN:
                data.Initialize(MSG_MOVE_SET_RUN_SPEED, 8+4+1+4+4+4+4+4+4+4);
                break;
            case MOVE_RUN_BACK:
                data.Initialize(MSG_MOVE_SET_RUN_BACK_SPEED, 8+4+1+4+4+4+4+4+4+4);
                break;
            case MOVE_SWIM:
                data.Initialize(MSG_MOVE_SET_SWIM_SPEED, 8+4+1+4+4+4+4+4+4+4);
                break;
            case MOVE_SWIM_BACK:
                data.Initialize(MSG_MOVE_SET_SWIM_BACK_SPEED, 8+4+1+4+4+4+4+4+4+4);
                break;
            case MOVE_TURN_RATE:
                data.Initialize(MSG_MOVE_SET_TURN_RATE, 8+4+1+4+4+4+4+4+4+4);
                break;
            case MOVE_FLIGHT:
                data.Initialize(MSG_MOVE_SET_FLIGHT_SPEED, 8+4+1+4+4+4+4+4+4+4);
                break;
            case MOVE_FLIGHT_BACK:
                data.Initialize(MSG_MOVE_SET_FLIGHT_BACK_SPEED, 8+4+1+4+4+4+4+4+4+4);
                break;
            default:
                sLog.outError("Unit::SetSpeed: Unsupported move type (%d), data not sent to client.",mtype);
                return;
        }

        data.append(GetPackGUID());
        data << uint32(0);                                  //movement flags
        data << uint8(0);                                   //unk
        data << uint32(getMSTime());
        data << float(GetPositionX());
        data << float(GetPositionY());
        data << float(GetPositionZ());
        data << float(GetOrientation());
        data << uint32(0);                                  //flag unk
        data << float(GetSpeed(mtype));
        SendMessageToSet( &data, true );
    }
    else
    {
        // register forced speed changes for WorldSession::HandleForceSpeedChangeAck
        // and do it only for real sent packets and use run for run/mounted as client expected
        ++(this->ToPlayer())->m_forced_speed_changes[mtype];
        switch(mtype)
        {
            case MOVE_WALK:
                data.Initialize(SMSG_FORCE_WALK_SPEED_CHANGE, 16);
                break;
            case MOVE_RUN:
                data.Initialize(SMSG_FORCE_RUN_SPEED_CHANGE, 17);
                break;
            case MOVE_RUN_BACK:
                data.Initialize(SMSG_FORCE_RUN_BACK_SPEED_CHANGE, 16);
                break;
            case MOVE_SWIM:
                data.Initialize(SMSG_FORCE_SWIM_SPEED_CHANGE, 16);
                break;
            case MOVE_SWIM_BACK:
                data.Initialize(SMSG_FORCE_SWIM_BACK_SPEED_CHANGE, 16);
                break;
            case MOVE_TURN_RATE:
                data.Initialize(SMSG_FORCE_TURN_RATE_CHANGE, 16);
                break;
            case MOVE_FLIGHT:
                data.Initialize(SMSG_FORCE_FLIGHT_SPEED_CHANGE, 16);
                break;
            case MOVE_FLIGHT_BACK:
                data.Initialize(SMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE, 16);
                break;
            default:
                sLog.outError("Unit::SetSpeed: Unsupported move type (%d), data not sent to client.",mtype);
                return;
        }
        data.append(GetPackGUID());
        data << (uint32)0;                                  // moveEvent, NUM_PMOVE_EVTS = 0x39
        if (mtype == MOVE_RUN)
            data << uint8(0);                               // new 2.1.0
        data << float(GetSpeed(mtype));
        SendMessageToSet( &data, true );
    }
    if (withPet) {
        if(GetPetGUID() && !IsInCombat() && m_speed_rate[mtype] >= 1.0f) {
            if (Pet* pet = GetPet())
                pet->SetSpeed(mtype, m_speed_rate[mtype], forced);
        }
        if (GetTypeId() == TYPEID_PLAYER) {
            if (Pet* minipet = ToPlayer()->GetMiniPet())
                minipet->SetSpeed(mtype, m_speed_rate[mtype], forced);
        }
    }
}

void Unit::SetHover(bool on)
{
    if(on)
        CastSpell(this,11010,true);
    else
        RemoveAurasDueToSpell(11010);
}

void Unit::setDeathState(DeathState s)
{
    if (s != ALIVE && s!= JUST_ALIVED)
    {
        CombatStop();
        DeleteThreatList();
        getHostilRefManager().deleteReferences();
        ClearComboPointHolders();                           // any combo points pointed to unit lost at it death

        if(IsNonMeleeSpellCasted(false))
            InterruptNonMeleeSpells(false);
    }

    if (s == JUST_DIED)
    {
        RemoveAllAurasOnDeath();
        UnsummonAllTotems();

        ModifyAuraState(AURA_STATE_HEALTHLESS_20_PERCENT, false);
        ModifyAuraState(AURA_STATE_HEALTHLESS_35_PERCENT, false);
        // remove aurastates allowing special moves
        ClearAllReactives();
        ClearDiminishings();
        GetMotionMaster()->Clear(false);
        GetMotionMaster()->MoveIdle();
        //without this when removing IncreaseMaxHealth aura player may stuck with 1 hp
        //do not why since in IncreaseMaxHealth currenthealth is checked
        SetHealth(0);
    }
    else if(s == JUST_ALIVED)
    {
        RemoveFlag (UNIT_FIELD_FLAGS, UNIT_FLAG_SKINNABLE); // clear skinnable for creature and player (at battleground)
    }

    if (m_deathState != ALIVE && s == ALIVE)
    {
        //_ApplyAllAuraMods();
    }
    m_deathState = s;
}

/*########################################
########                          ########
########       AGGRO SYSTEM       ########
########                          ########
########################################*/
bool Unit::CanHaveThreatList() const
{
    // only creatures can have threat list
    if( GetTypeId() != TYPEID_UNIT )
        return false;

    // only alive units can have threat list
    if (!IsAlive()/* || isDying()*/)
        return false;

    // totems can not have threat list
    if( (this->ToCreature())->isTotem() )
        return false;

    // pets can not have a threat list, unless they are controlled by a creature
    if( (this->ToCreature())->IsPet() && IS_PLAYER_GUID(((Pet*)this)->GetOwnerGUID()) )
        return false;

    return true;
}

//======================================================================

void Unit::ApplyTotalThreatModifier(float& threat, SpellSchoolMask schoolMask)
{
    if(!HasAuraType(SPELL_AURA_MOD_THREAT))
        return;

    SpellSchools school = GetFirstSchoolInMask(schoolMask);
    threat = threat * m_threatModifier[school];
}

//======================================================================

void Unit::AddThreat(Unit* pVictim, float threat, SpellSchoolMask schoolMask, SpellEntry const *threatSpell)
{
    // Only mobs can manage threat lists
    if(CanHaveThreatList())
        m_ThreatManager.addThreat(pVictim, threat, schoolMask, threatSpell);
        
    if (ToCreature() && pVictim->ToPlayer() && ToCreature()->isWorldBoss())
        ToCreature()->AllowToLoot((pVictim->ToPlayer())->GetGUIDLow());
}

//======================================================================

void Unit::DeleteThreatList()
{
    m_ThreatManager.clearReferences();
}

//======================================================================

void Unit::TauntApply(Unit* taunter)
{
    assert(GetTypeId()== TYPEID_UNIT);

    if(!taunter || (taunter->GetTypeId() == TYPEID_PLAYER && ((taunter->ToPlayer())->isGameMaster() || (taunter->ToPlayer())->isSpectator())))
        return;

    if(!CanHaveThreatList())
        return;

    Unit *target = GetVictim();
    if(target && target == taunter)
        return;

    // Only attack taunter if this is a valid target
    if (!IsCombatStationary() || CanReachWithMeleeAttack(taunter)) {
        SetInFront(taunter);

        if ((this->ToCreature())->IsAIEnabled) {
            (this->ToCreature())->AI()->AttackStart(taunter);
            if (ToCreature()->getAI())
                ToCreature()->getAI()->attackStart(taunter);
        }
    }

    //m_ThreatManager.tauntApply(taunter);
}

//======================================================================

void Unit::TauntFadeOut(Unit *taunter)
{
    assert(GetTypeId()== TYPEID_UNIT);

    if(!taunter || (taunter->GetTypeId() == TYPEID_PLAYER && ((taunter->ToPlayer())->isGameMaster() || (taunter->ToPlayer())->isSpectator())))
        return;

    if(!CanHaveThreatList())
        return;

    Unit *target = GetVictim();
    if(!target || target != taunter)
        return;

    if(m_ThreatManager.isThreatListEmpty())
    {
        if((this->ToCreature())->IsAIEnabled) {
            (this->ToCreature())->AI()->EnterEvadeMode();
            if (this->ToCreature()->getAI())
                this->ToCreature()->getAI()->evade();
        }
        return;
    }

    //m_ThreatManager.tauntFadeOut(taunter);
    target = m_ThreatManager.getHostilTarget();

    if (target && target != taunter)
    {
        SetInFront(target);
        if ((this->ToCreature())->IsAIEnabled) {
            (this->ToCreature())->AI()->AttackStart(target);
            if (ToCreature()->getAI())
                ToCreature()->getAI()->attackStart(target);
        }
    }
}

bool Unit::HasInThreatList(uint64 hostileGUID)
{
    if (!CanHaveThreatList())
        return false;
        
    std::list<HostilReference*>& threatList = m_ThreatManager.getThreatList();
    for (std::list<HostilReference*>::const_iterator itr = threatList.begin(); itr != threatList.end(); ++itr) {
        Unit* current = (*itr)->getTarget();
        if (current && current->GetGUID() == hostileGUID)
            return true;
    }
    
    return false;
}

//======================================================================

Unit* Creature::SelectVictim(bool evade)
{
    //function provides main threat functionality
    //next-victim-selection algorithm and evade mode are called
    //threat list sorting etc.

    //This should not be called by unit who does not have a threatlist
    //or who does not have threat (totem/pet/critter)
    //otherwise enterevademode every update

    Unit* target = NULL;
    //sLog.outString("%s SelectVictim1", GetName());
    if(!m_ThreatManager.isThreatListEmpty())
    {
        //sLog.outString("%s SelectVictim2", GetName());
        if(!HasAuraType(SPELL_AURA_MOD_TAUNT)) {
            //sLog.outString("%s if");
            target = m_ThreatManager.getHostilTarget();
        }
        else {
            //sLog.outString("%s else");
            target = GetVictim();
        }
    }

    if(target)
    {
        //sLog.outString("%s SelectVictim3", GetName());
        SetInFront(target); 
        return target;
    }
    
    // Case where mob is being kited.
    // Mob may not be in range to attack or may have dropped target. In any case,
    //  don't evade if damage received within the last 10 seconds
    // Does not apply to world bosses to prevent kiting to cities
    if (!isWorldBoss() && !GetInstanceId()) {
        if (time(NULL) - GetLastDamagedTime() <= MAX_AGGRO_RESET_TIME)
            return target;
    }

    // last case when creature don't must go to evade mode:
    // it in combat but attacker not make any damage and not enter to aggro radius to have record in threat list
    // for example at owner command to pet attack some far away creature
    // Note: creature not have targeted movement generator but have attacker in this case
    /*if( GetMotionMaster()->GetCurrentMovementGeneratorType() != TARGETED_MOTION_TYPE )
    {
        for(AttackerSet::const_iterator itr = m_attackers.begin(); itr != m_attackers.end(); ++itr)
        {
            if( (*itr)->IsInMap(this) && canAttack(*itr) && (*itr)->isInAccessiblePlaceFor(this->ToCreature()) )
                return NULL;
        }
    }*/

    // search nearby enemy before enter evade mode
    //sLog.outString("%s SelectVictim5", GetName());
    if(HasReactState(REACT_AGGRESSIVE))
    {
        //sLog.outString("%s SelectVictim6", GetName());
        target = SelectNearestTarget();
        if(target && !IsOutOfThreatArea(target))
            return target;
    }
    
    if(m_attackers.size())
        return NULL;

    if(m_invisibilityMask)
    {
        Unit::AuraList const& iAuras = GetAurasByType(SPELL_AURA_MOD_INVISIBILITY);
        for(Unit::AuraList::const_iterator itr = iAuras.begin(); itr != iAuras.end(); ++itr)
            if((*itr)->IsPermanent() && evade)
            {
                AI()->EnterEvadeMode();
                if (getAI())
                    getAI()->evade();
                break;
            }
        return NULL;
    }

    // enter in evade mode in other case
    if (evade) {
        AI()->EnterEvadeMode();
        if (getAI())
            getAI()->evade();
    }
    //sLog.outString("%s: Returning null", GetName());
    return NULL;
}

//======================================================================
//======================================================================
//======================================================================

int32 Unit::CalculateSpellDamage(SpellEntry const* spellProto, uint8 effect_index, int32 effBasePoints, Unit const* /*target*/)
{
    Player* unitPlayer = (GetTypeId() == TYPEID_PLAYER) ? this->ToPlayer() : NULL;
    
    //sLog.outString("CalculateSpellDamage for spell %u, effect index %u", spellProto->Id, effect_index);

    uint8 comboPoints = unitPlayer ? unitPlayer->GetComboPoints() : 0;

    int32 level = int32(getLevel());
    if (level > (int32)spellProto->maxLevel && spellProto->maxLevel > 0)
        level = (int32)spellProto->maxLevel;
    else if (level < (int32)spellProto->baseLevel)
        level = (int32)spellProto->baseLevel;
    level-= (int32)spellProto->spellLevel;

    float basePointsPerLevel = spellProto->EffectRealPointsPerLevel[effect_index];
    float randomPointsPerLevel = spellProto->EffectDicePerLevel[effect_index];
    int32 basePoints = int32(effBasePoints + level * basePointsPerLevel);
    int32 randomPoints = int32(spellProto->EffectDieSides[effect_index] + level * randomPointsPerLevel);
    float comboDamage = spellProto->EffectPointsPerComboPoint[effect_index];

    // prevent random generator from getting confused by spells casted with Unit::CastCustomSpell
    int32 randvalue = spellProto->EffectBaseDice[effect_index] >= randomPoints ? spellProto->EffectBaseDice[effect_index]:GetMap()->irand(spellProto->EffectBaseDice[effect_index], randomPoints);
    int32 value = basePoints + randvalue;
    //random damage
    if(comboDamage != 0 && unitPlayer /*&& target && (target->GetGUID() == unitPlayer->GetComboTarget())*/)
        value += (int32)(comboDamage * comboPoints);

    if(Player* modOwner = GetSpellModOwner())
    {
        modOwner->ApplySpellMod(spellProto->Id,SPELLMOD_ALL_EFFECTS, value);
        switch(effect_index)
        {
            case 0:
                modOwner->ApplySpellMod(spellProto->Id,SPELLMOD_EFFECT1, value);
                break;
            case 1:
                modOwner->ApplySpellMod(spellProto->Id,SPELLMOD_EFFECT2, value);
                break;
            case 2:
                modOwner->ApplySpellMod(spellProto->Id,SPELLMOD_EFFECT3, value);
                break;
        }
    }

    if(!basePointsPerLevel && (spellProto->Attributes & SPELL_ATTR_LEVEL_DAMAGE_CALCULATION && spellProto->spellLevel) &&
            spellProto->Effect[effect_index] != SPELL_EFFECT_WEAPON_PERCENT_DAMAGE &&
            spellProto->Effect[effect_index] != SPELL_EFFECT_KNOCK_BACK)
            //there are many more: slow speed, -healing pct
        //value = int32(value*0.25f*exp(getLevel()*(70-spellProto->spellLevel)/1000.0f));
        value = int32(value * (int32)getLevel() / (int32)(spellProto->spellLevel ? spellProto->spellLevel : 1));

    //sLog.outString("Returning %u", value);

    return value;
}

int32 Unit::CalculateSpellDuration(SpellEntry const* spellProto, uint8 effect_index, Unit const* target)
{
    Player* unitPlayer = (GetTypeId() == TYPEID_PLAYER) ? this->ToPlayer() : NULL;

    uint8 comboPoints = unitPlayer ? unitPlayer->GetComboPoints() : 0;

    int32 minduration = GetSpellDuration(spellProto);
    int32 maxduration = GetSpellMaxDuration(spellProto);

    int32 duration;

    if( minduration != -1 && minduration != maxduration )
        duration = minduration + int32((maxduration - minduration) * comboPoints / 5);
    else
        duration = minduration;

    if (duration > 0)
    {
        int32 mechanic = GetEffectMechanic(spellProto, effect_index);
        // Find total mod value (negative bonus)
        int32 durationMod_always = target->GetTotalAuraModifierByMiscValue(SPELL_AURA_MECHANIC_DURATION_MOD, mechanic);
        // Find max mod (negative bonus)
        int32 durationMod_not_stack = target->GetMaxNegativeAuraModifierByMiscValue(SPELL_AURA_MECHANIC_DURATION_MOD_NOT_STACK, mechanic);

        int32 durationMod = 0;
        // Select strongest negative mod
        if (durationMod_always > durationMod_not_stack)
            durationMod = durationMod_not_stack;
        else
            durationMod = durationMod_always;

        if (durationMod != 0)
            duration = int32(int64(duration) * (100+durationMod) /100);

        if (duration < 0) duration = 0;
    }

    return duration;
}

DiminishingLevels Unit::GetDiminishing(DiminishingGroup group)
{
    for(Diminishing::iterator i = m_Diminishing.begin(); i != m_Diminishing.end(); ++i)
    {
        if(i->DRGroup != group)
            continue;

        if(!i->hitCount)
            return DIMINISHING_LEVEL_1;

        if(!i->hitTime)
            return DIMINISHING_LEVEL_1;

        // If last spell was casted more than 15 seconds ago - reset the count.
        if(i->stack==0 && GetMSTimeDiff(i->hitTime,getMSTime()) > 15000)
        {
            i->hitCount = DIMINISHING_LEVEL_1;
            return DIMINISHING_LEVEL_1;
        }
        // or else increase the count.
        else
        {
            return DiminishingLevels(i->hitCount);
        }
    }
    return DIMINISHING_LEVEL_1;
}

void Unit::IncrDiminishing(DiminishingGroup group)
{
    // Checking for existing in the table
    bool IsExist = false;
    for(Diminishing::iterator i = m_Diminishing.begin(); i != m_Diminishing.end(); ++i)
    {
        if(i->DRGroup != group)
            continue;

        IsExist = true;
        if(i->hitCount < DIMINISHING_LEVEL_IMMUNE)
            i->hitCount += 1;

        break;
    }

    if(!IsExist)
        m_Diminishing.push_back(DiminishingReturn(group,getMSTime(),DIMINISHING_LEVEL_2));
}

void Unit::ApplyDiminishingToDuration(DiminishingGroup group, int32 &duration,Unit* caster,DiminishingLevels Level)
{
    if(duration == -1 || group == DIMINISHING_NONE)/*(caster->IsFriendlyTo(this) && caster != this)*/
        return;

    //Hack to avoid incorrect diminishing on mind control
    if(group == DIMINISHING_CHARM && caster == this)
        return;

    // test pet/charm masters instead pets/charmedsz
    Unit const* targetOwner = GetCharmerOrOwner();
    Unit const* casterOwner = caster->GetCharmerOrOwner();

    // Duration of crowd control abilities on pvp target is limited by 10 sec. (2.2.0)
    if(duration > 10000 && IsDiminishingReturnsGroupDurationLimited(group))
    {
        Unit const* target = targetOwner ? targetOwner : this;
        Unit const* source = casterOwner ? casterOwner : caster;

        if(target->GetTypeId() == TYPEID_PLAYER && source->GetTypeId() == TYPEID_PLAYER)
            duration = 10000;
    }

    float mod = 1.0f;

    // Some diminishings applies to mobs too (for example, Stun)
    if((GetDiminishingReturnsGroupType(group) == DRTYPE_PLAYER && (targetOwner ? targetOwner->GetTypeId():GetTypeId())  == TYPEID_PLAYER) || GetDiminishingReturnsGroupType(group) == DRTYPE_ALL)
    {
        DiminishingLevels diminish = Level;
        switch(diminish)
        {
            case DIMINISHING_LEVEL_1: break;
            case DIMINISHING_LEVEL_2: mod = 0.5f; break;
            case DIMINISHING_LEVEL_3: mod = 0.25f; break;
            case DIMINISHING_LEVEL_IMMUNE: mod = 0.0f;break;
            default: break;
        }
    }

    duration = int32(duration * mod);
}

void Unit::ApplyDiminishingAura( DiminishingGroup group, bool apply )
{
    // Checking for existing in the table
    for(Diminishing::iterator i = m_Diminishing.begin(); i != m_Diminishing.end(); ++i)
    {
        if(i->DRGroup != group)
            continue;

        i->hitTime = getMSTime();

        if(apply)
            i->stack += 1;
        else if(i->stack)
            i->stack -= 1;

        break;
    }
}

Unit* Unit::GetUnit(WorldObject& object, uint64 guid)
{
    return ObjectAccessor::GetUnit(object,guid);
}

Player* Unit::GetPlayer(uint64 guid)
{
    return ObjectAccessor::FindPlayer(guid);
}

Creature* Unit::GetCreature(WorldObject& object, uint64 guid)
{
    return ObjectAccessor::GetCreature(object, guid);
}

bool Unit::isVisibleForInState( Player const* u, bool inVisibleList ) const
{
    return isVisibleForOrDetect(u, false, inVisibleList, false);
}

uint32 Unit::GetCreatureType() const
{
    if(GetTypeId() == TYPEID_PLAYER)
    {
        SpellShapeshiftEntry const* ssEntry = sSpellShapeshiftStore.LookupEntry((this->ToPlayer())->m_form);
        if(ssEntry && ssEntry->creatureType > 0)
            return ssEntry->creatureType;
        else
            return CREATURE_TYPE_HUMANOID;
    }
    else
        return (this->ToCreature())->GetCreatureInfo()->type;
}

/*#######################################
########                         ########
########       STAT SYSTEM       ########
########                         ########
#######################################*/

bool Unit::HandleStatModifier(UnitMods unitMod, UnitModifierType modifierType, float amount, bool apply)
{
    if(unitMod >= UNIT_MOD_END || modifierType >= MODIFIER_TYPE_END)
    {
        sLog.outError("ERROR in HandleStatModifier(): non existed UnitMods or wrong UnitModifierType!");
        return false;
    }

    float val = 1.0f;

    switch(modifierType)
    {
        case BASE_VALUE:
        case TOTAL_VALUE:
            m_auraModifiersGroup[unitMod][modifierType] += apply ? amount : -amount;
            break;
        case BASE_PCT:
        case TOTAL_PCT:
            if(amount <= -100.0f)                           //small hack-fix for -100% modifiers
                amount = -200.0f;

            val = (100.0f + amount) / 100.0f;
            m_auraModifiersGroup[unitMod][modifierType] *= apply ? val : (1.0f/val);
            break;

        default:
            break;
    }

    if(!CanModifyStats())
        return false;

    switch(unitMod)
    {
        case UNIT_MOD_STAT_STRENGTH:
        case UNIT_MOD_STAT_AGILITY:
        case UNIT_MOD_STAT_STAMINA:
        case UNIT_MOD_STAT_INTELLECT:
        case UNIT_MOD_STAT_SPIRIT:         UpdateStats(GetStatByAuraGroup(unitMod));  break;

        case UNIT_MOD_ARMOR:               UpdateArmor();           break;
        case UNIT_MOD_HEALTH:              UpdateMaxHealth();       break;

        case UNIT_MOD_MANA:
        case UNIT_MOD_RAGE:
        case UNIT_MOD_FOCUS:
        case UNIT_MOD_ENERGY:
        case UNIT_MOD_HAPPINESS:           UpdateMaxPower(GetPowerTypeByAuraGroup(unitMod));         break;

        case UNIT_MOD_RESISTANCE_HOLY:
        case UNIT_MOD_RESISTANCE_FIRE:
        case UNIT_MOD_RESISTANCE_NATURE:
        case UNIT_MOD_RESISTANCE_FROST:
        case UNIT_MOD_RESISTANCE_SHADOW:
        case UNIT_MOD_RESISTANCE_ARCANE:   UpdateResistances(GetSpellSchoolByAuraGroup(unitMod));      break;

        case UNIT_MOD_ATTACK_POWER:        UpdateAttackPowerAndDamage();         break;
        case UNIT_MOD_ATTACK_POWER_RANGED: UpdateAttackPowerAndDamage(true);     break;

        case UNIT_MOD_DAMAGE_MAINHAND:     UpdateDamagePhysical(BASE_ATTACK);    break;
        case UNIT_MOD_DAMAGE_OFFHAND:      UpdateDamagePhysical(OFF_ATTACK);     break;
        case UNIT_MOD_DAMAGE_RANGED:       UpdateDamagePhysical(RANGED_ATTACK);  break;

        default:
            break;
    }

    return true;
}

float Unit::GetModifierValue(UnitMods unitMod, UnitModifierType modifierType) const
{
    if( unitMod >= UNIT_MOD_END || modifierType >= MODIFIER_TYPE_END)
    {
        sLog.outError("ERROR: trial to access non existed modifier value from UnitMods!");
        return 0.0f;
    }

    if(modifierType == TOTAL_PCT && m_auraModifiersGroup[unitMod][modifierType] <= 0.0f)
        return 0.0f;

    return m_auraModifiersGroup[unitMod][modifierType];
}

float Unit::GetTotalStatValue(Stats stat) const
{
    UnitMods unitMod = UnitMods(UNIT_MOD_STAT_START + stat);

    if(m_auraModifiersGroup[unitMod][TOTAL_PCT] <= 0.0f)
        return 0.0f;

    // value = ((base_value * base_pct) + total_value) * total_pct
    float value  = m_auraModifiersGroup[unitMod][BASE_VALUE] + GetCreateStat(stat);
    value *= m_auraModifiersGroup[unitMod][BASE_PCT];
    value += m_auraModifiersGroup[unitMod][TOTAL_VALUE];
    value *= m_auraModifiersGroup[unitMod][TOTAL_PCT];

    return value;
}

float Unit::GetTotalAuraModValue(UnitMods unitMod) const
{
    if(unitMod >= UNIT_MOD_END)
    {
        sLog.outError("ERROR: trial to access non existed UnitMods in GetTotalAuraModValue()!");
        return 0.0f;
    }

    if(m_auraModifiersGroup[unitMod][TOTAL_PCT] <= 0.0f)
        return 0.0f;

    float value  = m_auraModifiersGroup[unitMod][BASE_VALUE];
    value *= m_auraModifiersGroup[unitMod][BASE_PCT];
    value += m_auraModifiersGroup[unitMod][TOTAL_VALUE];
    
    //add dynamic flat mods
    if (unitMod == UNIT_MOD_ATTACK_POWER_RANGED && (getClassMask() & CLASSMASK_WAND_USERS) == 0) {
        AuraList const& mRAPbyIntellect = GetAurasByType(SPELL_AURA_MOD_RANGED_ATTACK_POWER_OF_STAT_PERCENT);
        for (AuraList::const_iterator i = mRAPbyIntellect.begin();i != mRAPbyIntellect.end(); ++i)
            value += int32(GetStat(Stats((*i)->GetModifier()->m_miscvalue)) * (*i)->GetModifierValue() / 100.0f);
    }
    
    value *= m_auraModifiersGroup[unitMod][TOTAL_PCT];

    return value;
}

SpellSchools Unit::GetSpellSchoolByAuraGroup(UnitMods unitMod) const
{
    SpellSchools school = SPELL_SCHOOL_NORMAL;

    switch(unitMod)
    {
        case UNIT_MOD_RESISTANCE_HOLY:     school = SPELL_SCHOOL_HOLY;          break;
        case UNIT_MOD_RESISTANCE_FIRE:     school = SPELL_SCHOOL_FIRE;          break;
        case UNIT_MOD_RESISTANCE_NATURE:   school = SPELL_SCHOOL_NATURE;        break;
        case UNIT_MOD_RESISTANCE_FROST:    school = SPELL_SCHOOL_FROST;         break;
        case UNIT_MOD_RESISTANCE_SHADOW:   school = SPELL_SCHOOL_SHADOW;        break;
        case UNIT_MOD_RESISTANCE_ARCANE:   school = SPELL_SCHOOL_ARCANE;        break;

        default:
            break;
    }

    return school;
}

Stats Unit::GetStatByAuraGroup(UnitMods unitMod) const
{
    Stats stat = STAT_STRENGTH;

    switch(unitMod)
    {
        case UNIT_MOD_STAT_STRENGTH:    stat = STAT_STRENGTH;      break;
        case UNIT_MOD_STAT_AGILITY:     stat = STAT_AGILITY;       break;
        case UNIT_MOD_STAT_STAMINA:     stat = STAT_STAMINA;       break;
        case UNIT_MOD_STAT_INTELLECT:   stat = STAT_INTELLECT;     break;
        case UNIT_MOD_STAT_SPIRIT:      stat = STAT_SPIRIT;        break;

        default:
            break;
    }

    return stat;
}

Powers Unit::GetPowerTypeByAuraGroup(UnitMods unitMod) const
{
    Powers power = POWER_MANA;

    switch(unitMod)
    {
        case UNIT_MOD_MANA:       power = POWER_MANA;       break;
        case UNIT_MOD_RAGE:       power = POWER_RAGE;       break;
        case UNIT_MOD_FOCUS:      power = POWER_FOCUS;      break;
        case UNIT_MOD_ENERGY:     power = POWER_ENERGY;     break;
        case UNIT_MOD_HAPPINESS:  power = POWER_HAPPINESS;  break;

        default:
            break;
    }

    return power;
}

float Unit::GetAPBonusVersus(WeaponAttackType attType, Unit* victim) const
{
    if(!victim)
        return 0.0f;

    float bonus = 0.0f;

    //bonus from my own mods
    AuraType versusBonusType = (attType == RANGED_ATTACK) ? SPELL_AURA_MOD_RANGED_ATTACK_POWER_VERSUS : SPELL_AURA_MOD_MELEE_ATTACK_POWER_VERSUS;
    uint32 creatureTypeMask = victim->GetCreatureTypeMask();
    AuraList const& mCreatureAttackPower = GetAurasByType(versusBonusType);
    for(auto itr : mCreatureAttackPower)
        if(creatureTypeMask & uint32(itr->GetModifier()->m_miscvalue))
            bonus += itr->GetModifierValue();

    //bonus from target mods
    AuraType attackerBonusType = (attType == RANGED_ATTACK) ? SPELL_AURA_RANGED_ATTACK_POWER_ATTACKER_BONUS : SPELL_AURA_MELEE_ATTACK_POWER_ATTACKER_BONUS;
    bonus += victim->GetTotalAuraModifier(attackerBonusType);

    return bonus;
}

float Unit::GetTotalAttackPowerValue(WeaponAttackType attType, Unit* victim) const
{
    UnitMods unitMod = (attType == RANGED_ATTACK) ? UNIT_MOD_ATTACK_POWER_RANGED : UNIT_MOD_ATTACK_POWER;
    float val = GetTotalAuraModValue(unitMod);
    if (victim) 
        val += GetAPBonusVersus(attType,victim);

    if(val < 0.0f)
        val = 0.0f;

    return val;
}

float Unit::GetWeaponDamageRange(WeaponAttackType attType ,WeaponDamageRange type) const
{
    if (attType == OFF_ATTACK && !haveOffhandWeapon())
        return 0.0f;

    return m_weaponDamage[attType][type];
}

void Unit::SetLevel(uint32 lvl)
{
    SetUInt32Value(UNIT_FIELD_LEVEL, lvl);

    // group update
    if ((GetTypeId() == TYPEID_PLAYER) && (this->ToPlayer())->GetGroup())
        (this->ToPlayer())->SetGroupUpdateFlag(GROUP_UPDATE_FLAG_LEVEL);
}

void Unit::SetHealth(uint32 val)
{
    if(getDeathState() == JUST_DIED)
        val = 0;
    else
    {
        uint32 maxHealth = GetMaxHealth();
        if(maxHealth < val)
            val = maxHealth;
    }

    SetUInt32Value(UNIT_FIELD_HEALTH, val);

    // group update
    if (Player* player = ToPlayer())
    {
        if (player->HaveSpectators())
        {
            SpectatorAddonMsg msg;
            msg.SetPlayer(player->GetName());
            msg.SetCurrentHP(val);
            player->SendSpectatorAddonMsgToBG(msg);
        }

        if(player->GetGroup())
            player->SetGroupUpdateFlag(GROUP_UPDATE_FLAG_CUR_HP);
    }
    else if((this->ToCreature())->IsPet())
    {
        Pet *pet = ((Pet*)this);
        if(pet->isControlled())
        {
            Unit *owner = GetOwner();
            if(owner && (owner->GetTypeId() == TYPEID_PLAYER) && (owner->ToPlayer())->GetGroup())
                (owner->ToPlayer())->SetGroupUpdateFlag(GROUP_UPDATE_FLAG_PET_CUR_HP);
        }
    }
}

void Unit::SetMaxHealth(uint32 val)
{
    uint32 health = GetHealth();
    SetUInt32Value(UNIT_FIELD_MAXHEALTH, val);

    // group update
    if (GetTypeId() == TYPEID_PLAYER)
    {
        if (ToPlayer()->HaveSpectators())
        {
            SpectatorAddonMsg msg;
            msg.SetPlayer(ToPlayer()->GetName());
            msg.SetMaxHP(val);
            ToPlayer()->SendSpectatorAddonMsgToBG(msg);
        }

        if((this->ToPlayer())->GetGroup())
            (this->ToPlayer())->SetGroupUpdateFlag(GROUP_UPDATE_FLAG_MAX_HP);
    }
    else if((this->ToCreature())->IsPet())
    {
        Pet *pet = ((Pet*)this);
        if(pet->isControlled())
        {
            Unit *owner = GetOwner();
            if(owner && (owner->GetTypeId() == TYPEID_PLAYER) && (owner->ToPlayer())->GetGroup())
                (owner->ToPlayer())->SetGroupUpdateFlag(GROUP_UPDATE_FLAG_PET_MAX_HP);
        }
    }

    if(val < health)
        SetHealth(val);
}

void Unit::SetPower(Powers power, uint32 val)
{
    if(GetPower(power) == val)
        return;

    uint32 maxPower = GetMaxPower(power);
    if(maxPower < val)
        val = maxPower;

    SetStatInt32Value(UNIT_FIELD_POWER1 + power, val);

    // group update
    if (Player* player = ToPlayer())
    {
        if (player->HaveSpectators())
        {
            SpectatorAddonMsg msg;
            msg.SetPlayer(player->GetName());
            msg.SetCurrentPower(val);
            msg.SetPowerType(power);
            player->SendSpectatorAddonMsgToBG(msg);
        }

        if(player->GetGroup())
            player->SetGroupUpdateFlag(GROUP_UPDATE_FLAG_CUR_POWER);
    }
    else if((this->ToCreature())->IsPet())
    {
        Pet *pet = ((Pet*)this);
        if(pet->isControlled())
        {
            Unit *owner = GetOwner();
            if(owner && (owner->GetTypeId() == TYPEID_PLAYER) && (owner->ToPlayer())->GetGroup())
                (owner->ToPlayer())->SetGroupUpdateFlag(GROUP_UPDATE_FLAG_PET_CUR_POWER);
        }

        // Update the pet's character sheet with happiness damage bonus
        if(pet->getPetType() == HUNTER_PET && power == POWER_HAPPINESS)
        {
            pet->UpdateDamagePhysical(BASE_ATTACK);
        }
    }
}

void Unit::SetMaxPower(Powers power, uint32 val)
{
    uint32 cur_power = GetPower(power);
    SetStatInt32Value(UNIT_FIELD_MAXPOWER1 + power, val);

    // group update
    if(GetTypeId() == TYPEID_PLAYER)
    {
        if (ToPlayer()->HaveSpectators())
        {
            SpectatorAddonMsg msg;
            msg.SetPlayer(ToPlayer()->GetName());
            msg.SetMaxPower(val);
            msg.SetPowerType(power);
            ToPlayer()->SendSpectatorAddonMsgToBG(msg);
        }

        if((this->ToPlayer())->GetGroup())
            (this->ToPlayer())->SetGroupUpdateFlag(GROUP_UPDATE_FLAG_MAX_POWER);
    }
    else if((this->ToCreature())->IsPet())
    {
        Pet *pet = ((Pet*)this);
        if(pet->isControlled())
        {
            Unit *owner = GetOwner();
            if(owner && (owner->GetTypeId() == TYPEID_PLAYER) && (owner->ToPlayer())->GetGroup())
                (owner->ToPlayer())->SetGroupUpdateFlag(GROUP_UPDATE_FLAG_PET_MAX_POWER);
        }
    }

    if(val < cur_power)
        SetPower(power, val);
}

void Unit::ApplyPowerMod(Powers power, uint32 val, bool apply)
{
    ApplyModUInt32Value(UNIT_FIELD_POWER1+power, val, apply);

    // group update
    if(GetTypeId() == TYPEID_PLAYER)
    {
        if((this->ToPlayer())->GetGroup())
            (this->ToPlayer())->SetGroupUpdateFlag(GROUP_UPDATE_FLAG_CUR_POWER);
    }
    else if((this->ToCreature())->IsPet())
    {
        Pet *pet = ((Pet*)this);
        if(pet->isControlled())
        {
            Unit *owner = GetOwner();
            if(owner && (owner->GetTypeId() == TYPEID_PLAYER) && (owner->ToPlayer())->GetGroup())
                (owner->ToPlayer())->SetGroupUpdateFlag(GROUP_UPDATE_FLAG_PET_CUR_POWER);
        }
    }
}

void Unit::ApplyMaxPowerMod(Powers power, uint32 val, bool apply)
{
    ApplyModUInt32Value(UNIT_FIELD_MAXPOWER1+power, val, apply);

    // group update
    if(GetTypeId() == TYPEID_PLAYER)
    {
        if((this->ToPlayer())->GetGroup())
            (this->ToPlayer())->SetGroupUpdateFlag(GROUP_UPDATE_FLAG_MAX_POWER);
    }
    else if((this->ToCreature())->IsPet())
    {
        Pet *pet = ((Pet*)this);
        if(pet->isControlled())
        {
            Unit *owner = GetOwner();
            if(owner && (owner->GetTypeId() == TYPEID_PLAYER) && (owner->ToPlayer())->GetGroup())
                (owner->ToPlayer())->SetGroupUpdateFlag(GROUP_UPDATE_FLAG_PET_MAX_POWER);
        }
    }
}

void Unit::ApplyAuraProcTriggerDamage( Aura* aura, bool apply )
{
    AuraList& tAuraProcTriggerDamage = m_modAuras[SPELL_AURA_PROC_TRIGGER_DAMAGE];
    if(apply)
        tAuraProcTriggerDamage.push_back(aura);
    else
        tAuraProcTriggerDamage.remove(aura);
}

uint32 Unit::GetCreatePowers( Powers power ) const
{
    // POWER_FOCUS and POWER_HAPPINESS only have hunter pet
    switch(power)
    {
        case POWER_MANA:      return GetCreateMana();
        case POWER_RAGE:      return 1000;
        case POWER_FOCUS:     return (GetTypeId()==TYPEID_PLAYER || !((Creature const*)this)->IsPet() || ((Pet const*)this)->getPetType()!=HUNTER_PET ? 0 : 100);
        case POWER_ENERGY:    return 100;
        case POWER_HAPPINESS: return (GetTypeId()==TYPEID_PLAYER || !((Creature const*)this)->IsPet() || ((Pet const*)this)->getPetType()!=HUNTER_PET ? 0 : 1050000);
    }

    return 0;
}

void Unit::AddToWorld()
{
    if(!IsInWorld())
    {
        WorldObject::AddToWorld();
        m_Notified = false;
        m_IsInNotifyList = false;
        SetToNotify();
    }
}

void Unit::RemoveFromWorld()
{
    // cleanup
    if(IsInWorld())
    {
        RemoveCharmAuras();
        RemoveBindSightAuras();
        RemoveNotOwnSingleTargetAuras();
        WorldObject::RemoveFromWorld();
    }
}

void Unit::CleanupsBeforeDelete()
{
    if(GetTypeId()==TYPEID_UNIT && (this->ToCreature())->IsAIEnabled) {
        (this->ToCreature())->AI()->OnRemove();
        if ((this->ToCreature())->getAI())
            (this->ToCreature())->getAI()->onRemove();
    }
    // This needs to be before RemoveFromWorld to make GetCaster() return a valid pointer on aura removal
    InterruptNonMeleeSpells(true);

    if(IsInWorld())
        RemoveFromWorld();

    assert(m_uint32Values);

    //A unit may be in removelist and not in world, but it is still in grid
    //and may have some references during delete
    RemoveAllAuras();
    m_Events.KillAllEvents(false);                      // non-delatable (currently casted spells) will not deleted now but it will deleted at call in Map::RemoveAllObjectsInRemoveList
    CombatStop();
    ClearComboPointHolders();
    DeleteThreatList();
    getHostilRefManager().setOnlineOfflineState(false);
    RemoveAllGameObjects();
    RemoveAllDynObjects();
    GetMotionMaster()->Clear(false);                    // remove different non-standard movement generators.
}

void Unit::UpdateCharmAI()
{
    if(GetTypeId() == TYPEID_PLAYER)
        return;

    if(i_disabledAI) // disabled AI must be primary AI
    {
        if(!isCharmed())
        {
            if(i_AI) delete i_AI;
            i_AI = i_disabledAI;
            i_disabledAI = NULL;
        }
    }
    else
    {
        if(isCharmed())
        {
            i_disabledAI = i_AI;
            if(isPossessed())
                i_AI = new PossessedAI(this->ToCreature());
            else
                i_AI = new PetAI(this->ToCreature());
        }
    }
}

CharmInfo* Unit::InitCharmInfo()
{
    if(!m_charmInfo)
        m_charmInfo = new CharmInfo(this);

    return m_charmInfo;
}

void Unit::DeleteCharmInfo()
{
    if(!m_charmInfo)
        return;

    delete m_charmInfo;
    m_charmInfo = NULL;
}

CharmInfo::CharmInfo(Unit* unit)
: m_unit(unit), m_CommandState(COMMAND_FOLLOW), m_petnumber(0), m_barInit(false)
{
    for(int i =0; i<4; ++i)
    {
        m_charmspells[i].spellId = 0;
        m_charmspells[i].active = ACT_DISABLED;
    }
    if(m_unit->GetTypeId() == TYPEID_UNIT)
    {
        m_oldReactState = (m_unit->ToCreature())->GetReactState();
        (m_unit->ToCreature())->SetReactState(REACT_PASSIVE);
    }
}

CharmInfo::~CharmInfo()
{
    if(m_unit->GetTypeId() == TYPEID_UNIT)
    {
        (m_unit->ToCreature())->SetReactState(m_oldReactState);
    }
}

void CharmInfo::InitPetActionBar()
{
    if (m_barInit)
        return;

    // the first 3 SpellOrActions are attack, follow and stay
    for(uint32 i = 0; i < 3; i++)
    {
        PetActionBar[i].Type = ACT_COMMAND;
        PetActionBar[i].SpellOrAction = COMMAND_ATTACK - i;

        PetActionBar[i + 7].Type = ACT_REACTION;
        PetActionBar[i + 7].SpellOrAction = COMMAND_ATTACK - i;
    }
    for(uint32 i=0; i < 4; i++)
    {
        PetActionBar[i + 3].Type = ACT_DISABLED;
        PetActionBar[i + 3].SpellOrAction = 0;
    }
    m_barInit = true;
}

void CharmInfo::InitEmptyActionBar(bool withAttack)
{
    if (m_barInit)
        return;

    for(uint32 x = 0; x < 10; ++x)
    {
        PetActionBar[x].Type = ACT_CAST;
        PetActionBar[x].SpellOrAction = 0;
    }
    if (m_unit->ToCreature()) {
        if ((m_unit->ToCreature())->GetEntry() == 23109) {
            PetActionBar[0].Type = ACT_CAST;
            PetActionBar[0].SpellOrAction = 40325;
            m_barInit = true;
            return;
        }
    }
    if (withAttack)
    {
        PetActionBar[0].Type = ACT_COMMAND;
        PetActionBar[0].SpellOrAction = COMMAND_ATTACK;
    }
    m_barInit = true;
}

void CharmInfo::InitPossessCreateSpells()
{
    if (m_unit->GetEntry() == 25653)
        InitEmptyActionBar(false);
    else
        InitEmptyActionBar();

    if(m_unit->GetTypeId() == TYPEID_UNIT)
    {
        for(uint32 i = 0; i < CREATURE_MAX_SPELLS; ++i)
        {
            uint32 spellid = (m_unit->ToCreature())->m_spells[i];
            if(IsPassiveSpell(spellid))
                m_unit->CastSpell(m_unit, spellid, true);
            else
                AddSpellToAB(0, spellid, i, ACT_CAST);
        }
    }
}

void CharmInfo::InitCharmCreateSpells()
{
    if(m_unit->GetTypeId() == TYPEID_PLAYER)                //charmed players don't have spells
    {
        InitEmptyActionBar();
        return;
    }

    InitEmptyActionBar(false);

    for(uint32 x = 0; x < CREATURE_MAX_SPELLS; ++x)
    {
        uint32 spellId = (m_unit->ToCreature())->m_spells[x];
        m_charmspells[x].spellId = spellId;

        if(!spellId)
            continue;

        if (IsPassiveSpell(spellId))
        {
            m_unit->CastSpell(m_unit, spellId, true);
            m_charmspells[x].active = ACT_PASSIVE;
        }
        else
        {
            ActiveStates newstate;
            bool onlyselfcast = true;
            SpellEntry const *spellInfo = spellmgr.LookupSpell(spellId);

            if(!spellInfo) onlyselfcast = false;
            for(uint32 i = 0;i<3 && onlyselfcast;++i)       //non existent spell will not make any problems as onlyselfcast would be false -> break right away
            {
                if(spellInfo->EffectImplicitTargetA[i] != TARGET_UNIT_CASTER && spellInfo->EffectImplicitTargetA[i] != 0)
                    onlyselfcast = false;
            }

            if(onlyselfcast || !IsPositiveSpell(spellId))   //only self cast and spells versus enemies are autocastable
                newstate = ACT_DISABLED;
            else
                newstate = ACT_CAST;

            AddSpellToAB(0, spellId, x, newstate);
        }
    }
}

bool CharmInfo::AddSpellToAB(uint32 oldid, uint32 newid, uint8 index, ActiveStates newstate)
{
    if((PetActionBar[index].Type == ACT_DISABLED || PetActionBar[index].Type == ACT_ENABLED || PetActionBar[index].Type == ACT_CAST) && PetActionBar[index].SpellOrAction == oldid)
    {
        PetActionBar[index].SpellOrAction = newid;
        if(!oldid)
        {
            if(newstate == ACT_DECIDE)
                PetActionBar[index].Type = ACT_DISABLED;
            else
                PetActionBar[index].Type = newstate;
        }
        return true;
    }

    return false;
}

void CharmInfo::ToggleCreatureAutocast(uint32 spellid, bool apply)
{
    if(IsPassiveSpell(spellid))
        return;

    for(uint32 x = 0; x < CREATURE_MAX_SPELLS; ++x)
    {
        if(spellid == m_charmspells[x].spellId)
        {
            m_charmspells[x].active = apply ? ACT_ENABLED : ACT_DISABLED;
        }
    }
}

void CharmInfo::SetPetNumber(uint32 petnumber, bool statwindow)
{
    m_petnumber = petnumber;
    if(statwindow)
        m_unit->SetUInt32Value(UNIT_FIELD_PETNUMBER, m_petnumber);
    else
        m_unit->SetUInt32Value(UNIT_FIELD_PETNUMBER, 0);
}

bool Unit::isFrozen() const
{
    AuraList const& mRoot = GetAurasByType(SPELL_AURA_MOD_ROOT);
    for(AuraList::const_iterator i = mRoot.begin(); i != mRoot.end(); ++i)
        if( GetSpellSchoolMask((*i)->GetSpellProto()) & SPELL_SCHOOL_MASK_FROST)
            return true;
    return false;
}

struct ProcTriggeredData
{
    ProcTriggeredData(SpellProcEventEntry const * _spellProcEvent, Aura* _triggeredByAura)
        : spellProcEvent(_spellProcEvent), triggeredByAura(_triggeredByAura),
        triggeredByAura_SpellPair(Unit::spellEffectPair(triggeredByAura->GetId(),triggeredByAura->GetEffIndex()))
        {}
    SpellProcEventEntry const *spellProcEvent;
    Aura* triggeredByAura;
    Unit::spellEffectPair triggeredByAura_SpellPair;
};

typedef std::list< ProcTriggeredData > ProcTriggeredList;
typedef std::list< uint32> RemoveSpellList;

// List of auras that CAN be trigger but may not exist in spell_proc_event
// in most case need for drop charges
// in some types of aura need do additional check
// for example SPELL_AURA_MECHANIC_IMMUNITY - need check for mechanic
static bool isTriggerAura[TOTAL_AURAS];
static bool isNonTriggerAura[TOTAL_AURAS];
void InitTriggerAuraData()
{
    for (int i=0;i<TOTAL_AURAS;i++)
    {
      isTriggerAura[i]=false;
      isNonTriggerAura[i] = false;
    }
    isTriggerAura[SPELL_AURA_DUMMY] = true;
    isTriggerAura[SPELL_AURA_MOD_CONFUSE] = true;
    isTriggerAura[SPELL_AURA_MOD_THREAT] = true;
    isTriggerAura[SPELL_AURA_MOD_STUN] = true; // Aura not have charges but need remove him on trigger
    isTriggerAura[SPELL_AURA_MOD_DAMAGE_DONE] = true;
    isTriggerAura[SPELL_AURA_MOD_DAMAGE_TAKEN] = true;
    isTriggerAura[SPELL_AURA_MOD_RESISTANCE] = true;
    isTriggerAura[SPELL_AURA_MOD_ROOT] = true;
    isTriggerAura[SPELL_AURA_REFLECT_SPELLS] = true;
    isTriggerAura[SPELL_AURA_DAMAGE_IMMUNITY] = true;
    isTriggerAura[SPELL_AURA_PROC_TRIGGER_SPELL] = true;
    isTriggerAura[SPELL_AURA_PROC_TRIGGER_DAMAGE] = true;
    isTriggerAura[SPELL_AURA_MOD_CASTING_SPEED] = true;
    isTriggerAura[SPELL_AURA_MOD_POWER_COST_SCHOOL_PCT] = true;
    isTriggerAura[SPELL_AURA_MOD_POWER_COST_SCHOOL] = true;
    isTriggerAura[SPELL_AURA_REFLECT_SPELLS_SCHOOL] = true;
    isTriggerAura[SPELL_AURA_MECHANIC_IMMUNITY] = true;
    isTriggerAura[SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN] = true;
    isTriggerAura[SPELL_AURA_SPELL_MAGNET] = true;
    isTriggerAura[SPELL_AURA_MOD_ATTACK_POWER] = true;
    isTriggerAura[SPELL_AURA_ADD_CASTER_HIT_TRIGGER] = true;
    isTriggerAura[SPELL_AURA_OVERRIDE_CLASS_SCRIPTS] = true;
    isTriggerAura[SPELL_AURA_MOD_MECHANIC_RESISTANCE] = true;
    isTriggerAura[SPELL_AURA_MELEE_ATTACK_POWER_ATTACKER_BONUS] = true;
    isTriggerAura[SPELL_AURA_RANGED_ATTACK_POWER_ATTACKER_BONUS] = true;
    isTriggerAura[SPELL_AURA_MOD_HASTE] = true;
    isTriggerAura[SPELL_AURA_MOD_ATTACKER_MELEE_HIT_CHANCE]=true;
    isTriggerAura[SPELL_AURA_PRAYER_OF_MENDING] = true;

    isNonTriggerAura[SPELL_AURA_MOD_POWER_REGEN]=true;
    isNonTriggerAura[SPELL_AURA_RESIST_PUSHBACK]=true;
}

uint32 createProcExtendMask(SpellNonMeleeDamage *damageInfo, SpellMissInfo missCondition)
{
    uint32 procEx = PROC_EX_NONE;
    // Check victim state
    if (missCondition!=SPELL_MISS_NONE)
    switch (missCondition)
    {
        case SPELL_MISS_MISS:    procEx|=PROC_EX_MISS;   break;
        case SPELL_MISS_RESIST:  procEx|=PROC_EX_RESIST; break;
        case SPELL_MISS_DODGE:   procEx|=PROC_EX_DODGE;  break;
        case SPELL_MISS_PARRY:   procEx|=PROC_EX_PARRY;  break;
        case SPELL_MISS_BLOCK:   procEx|=PROC_EX_BLOCK;  break;
        case SPELL_MISS_EVADE:   procEx|=PROC_EX_EVADE;  break;
        case SPELL_MISS_IMMUNE:  procEx|=PROC_EX_IMMUNE; break;
        case SPELL_MISS_IMMUNE2: procEx|=PROC_EX_IMMUNE; break;
        case SPELL_MISS_DEFLECT: procEx|=PROC_EX_DEFLECT;break;
        case SPELL_MISS_ABSORB:  procEx|=PROC_EX_ABSORB; break;
        case SPELL_MISS_REFLECT: procEx|=PROC_EX_REFLECT;break;
        default:
            break;
    }
    else
    {
        // On block
        if (damageInfo->blocked)
            procEx|=PROC_EX_BLOCK;
        // On absorb
        if (damageInfo->absorb)
            procEx|=PROC_EX_ABSORB;
        // On crit
        if (damageInfo->HitInfo & SPELL_HIT_TYPE_CRIT)
            procEx|=PROC_EX_CRITICAL_HIT;
        else
            procEx|=PROC_EX_NORMAL_HIT;
    }
    return procEx;
}

void Unit::ProcDamageAndSpellFor( bool isVictim, Unit * pTarget, uint32 procFlag, uint32 procExtra, WeaponAttackType attType, SpellEntry const * procSpell, uint32 damage )
{
    ++m_procDeep;
    if (m_procDeep > 5)
    {
        sLog.outError("Prevent possible stack owerflow in Unit::ProcDamageAndSpellFor");
        if (procSpell)
            sLog.outError("  Spell %u", procSpell->Id);
        --m_procDeep;
        return;
    }
    
    // For melee/ranged based attack need update skills and set some Aura states
    if (procFlag & MELEE_BASED_TRIGGER_MASK)
    {
        // Update skills here for players
        if (GetTypeId() == TYPEID_PLAYER)
        {
            // On melee based hit/miss/resist need update skill (for victim and attacker)
            if (procExtra&(PROC_EX_NORMAL_HIT|PROC_EX_MISS|PROC_EX_RESIST))
            {
                if (pTarget->GetTypeId() != TYPEID_PLAYER && pTarget->GetCreatureType() != CREATURE_TYPE_CRITTER)
                    (this->ToPlayer())->UpdateCombatSkills(pTarget, attType, MELEE_HIT_MISS, isVictim);
            }
            // Update defence if player is victim and parry/dodge/block
            if (isVictim && procExtra&(PROC_EX_DODGE|PROC_EX_PARRY|PROC_EX_BLOCK))
                (this->ToPlayer())->UpdateDefense();
        }
        // If exist crit/parry/dodge/block need update aura state (for victim and attacker)
        if (procExtra & (PROC_EX_CRITICAL_HIT|PROC_EX_PARRY|PROC_EX_DODGE|PROC_EX_BLOCK))
        {
            // for victim
            if (isVictim)
            {
                // if victim and dodge attack
                if (procExtra&PROC_EX_DODGE)
                {
                    //Update AURA_STATE on dodge
                    if (getClass() != CLASS_ROGUE) // skip Rogue Riposte
                    {
                        ModifyAuraState(AURA_STATE_DEFENSE, true);
                        StartReactiveTimer( REACTIVE_DEFENSE );
                    }
                }
                // if victim and parry attack
                if (procExtra & PROC_EX_PARRY)
                {
                    // For Hunters only Counterattack (skip Mongoose bite)
                    if (getClass() == CLASS_HUNTER)
                    {
                        ModifyAuraState(AURA_STATE_HUNTER_PARRY, true);
                        StartReactiveTimer( REACTIVE_HUNTER_PARRY );
                    }
                    else
                    {
                        ModifyAuraState(AURA_STATE_DEFENSE, true);
                        StartReactiveTimer( REACTIVE_DEFENSE );
                    }
                }
                // if and victim block attack
                if (procExtra & PROC_EX_BLOCK)
                {
                    ModifyAuraState(AURA_STATE_DEFENSE,true);
                    StartReactiveTimer( REACTIVE_DEFENSE );
                }
            }
            else //For attacker
            {
                // Overpower on victim dodge
                if (procExtra&PROC_EX_DODGE && GetTypeId() == TYPEID_PLAYER && getClass() == CLASS_WARRIOR)
                {
                    (this->ToPlayer())->AddComboPoints(pTarget, 1);
                    StartReactiveTimer( REACTIVE_OVERPOWER );
                }
                // Enable AURA_STATE_CRIT on crit
                if (procExtra & PROC_EX_CRITICAL_HIT)
                {
                    ModifyAuraState(AURA_STATE_CRIT, true);
                    StartReactiveTimer( REACTIVE_CRIT );
                    if(getClass()==CLASS_HUNTER)
                    {
                        ModifyAuraState(AURA_STATE_HUNTER_CRIT_STRIKE, true);
                        StartReactiveTimer( REACTIVE_HUNTER_CRIT );
                    }
                }
            }
        }
    }

    RemoveSpellList removedSpells;
    ProcTriggeredList procTriggered;
    // Fill procTriggered list
    
    for(AuraMap::const_iterator itr = GetAuras().begin(); itr!= GetAuras().end(); ++itr)
    {
        SpellProcEventEntry const* spellProcEvent = NULL;
        //sLog.outString("IsTriggeredAtSpellProcEvent: %u %u %x %x %u %s %s", itr->second->GetId(), procSpell ? procSpell->Id : 0, procFlag, procExtra, attType, isVictim ? "victim" : "attacker", (damage > 0) ? "damage > 0" : "damage < 0");
        if(!IsTriggeredAtSpellProcEvent(itr->second, procSpell, procFlag, procExtra, attType, isVictim, (damage > 0), spellProcEvent)) {
            //sLog.outString("No.");
            continue;
        }

        procTriggered.push_back( ProcTriggeredData(spellProcEvent, itr->second) );
    }
    // Handle effects proceed this time
    for(ProcTriggeredList::iterator i = procTriggered.begin(); i != procTriggered.end(); ++i)
    {
        // Some auras can be deleted in function called in this loop (except first, ofc)
        // Until storing auars in std::multimap to hard check deleting by another way
        if(i != procTriggered.begin())
        {
            bool found = false;
            AuraMap::const_iterator lower = GetAuras().lower_bound(i->triggeredByAura_SpellPair);
            AuraMap::const_iterator upper = GetAuras().upper_bound(i->triggeredByAura_SpellPair);
            for(AuraMap::const_iterator itr = lower; itr!= upper; ++itr)
            {
                if(itr->second==i->triggeredByAura)
                {
                    found = true;
                    break;
                }
            }
            if(!found)
            {
//                sLog.outDebug("Spell aura %u (id:%u effect:%u) has been deleted before call spell proc event handler", i->triggeredByAura->GetModifier()->m_auraname, i->triggeredByAura_SpellPair.first, i->triggeredByAura_SpellPair.second);
//                sLog.outDebug("It can be deleted one from early proccesed auras:");
//                for(ProcTriggeredList::iterator i2 = procTriggered.begin(); i != i2; ++i2)
//                    sLog.outDebug("     Spell aura %u (id:%u effect:%u)", i->triggeredByAura->GetModifier()->m_auraname,i2->triggeredByAura_SpellPair.first,i2->triggeredByAura_SpellPair.second);
//                    sLog.outDebug("     <end of list>");
                continue;
            }
        }

        SpellProcEventEntry const *spellProcEvent = i->spellProcEvent;
        Aura *triggeredByAura = i->triggeredByAura;
        Modifier *auraModifier = triggeredByAura->GetModifier();
        SpellEntry const *spellInfo = triggeredByAura->GetSpellProto();
        uint32 effIndex = triggeredByAura->GetEffIndex();
        bool useCharges = triggeredByAura->m_procCharges > 0;
        // For players set spell cooldown if need
        uint32 cooldown = 0;
        if (GetTypeId() == TYPEID_PLAYER && spellProcEvent && spellProcEvent->cooldown)
            cooldown = spellProcEvent->cooldown;

        switch(auraModifier->m_auraname)
        {
            case SPELL_AURA_PROC_TRIGGER_SPELL:
            {
                //sLog.outString("ProcDamageAndSpell: casting spell %u (triggered by %s aura of spell %u)", spellInfo->Id,(isVictim?"a victim's":"an attacker's"), triggeredByAura->GetId());
                // Don`t drop charge or add cooldown for not started trigger
                if (!HandleProcTriggerSpell(pTarget, damage, triggeredByAura, procSpell, procFlag, procExtra, cooldown))
                    continue;

                break;
            }
            case SPELL_AURA_PROC_TRIGGER_DAMAGE:
            {
                SpellNonMeleeDamage damageInfo(this, pTarget, spellInfo->Id, spellInfo->SchoolMask);
                uint32 damage = SpellDamageBonus(pTarget, spellInfo, auraModifier->m_amount, SPELL_DIRECT_DAMAGE);
                CalculateSpellDamageTaken(&damageInfo, damage, spellInfo);
                SendSpellNonMeleeDamageLog(&damageInfo);
                DealSpellDamage(&damageInfo, true);
                break;
            }
            case SPELL_AURA_MANA_SHIELD:
            case SPELL_AURA_DUMMY:
            {
                if (!HandleDummyAuraProc(pTarget, damage, triggeredByAura, procSpell, procFlag, procExtra, cooldown))
                    continue;
                break;
            }
            case SPELL_AURA_MOD_HASTE:
            {
                if (!HandleHasteAuraProc(pTarget, damage, triggeredByAura, procSpell, procFlag, procExtra, cooldown))
                    continue;
                
                if (triggeredByAura->GetSpellProto()->SpellVisual == 2759 && triggeredByAura->GetSpellProto()->SpellIconID == 108) { // Shaman and Warrior Flurry
                    if (procExtra & PROC_EX_CRITICAL_HIT)
                        useCharges = false;
                }
                
                break;
            }
            case SPELL_AURA_OVERRIDE_CLASS_SCRIPTS:
            {
                if (!HandleOverrideClassScriptAuraProc(pTarget, triggeredByAura, procSpell, cooldown))
                    continue;
                break;
            }
            case SPELL_AURA_PRAYER_OF_MENDING:
            {
                HandleMendingAuraProc(triggeredByAura);
                break;
            }
            case SPELL_AURA_MOD_STUN:
                // Remove by default, but if charge exist drop it
                if (triggeredByAura->m_procCharges == 0)
                   removedSpells.push_back(triggeredByAura->GetId());
                break;
            //case SPELL_AURA_MELEE_ATTACK_POWER_ATTACKER_BONUS:
            case SPELL_AURA_RANGED_ATTACK_POWER_ATTACKER_BONUS:
                // Hunter's Mark (1-4 Rangs) increase AP with every hit
                if (spellInfo->SpellFamilyName == SPELLFAMILY_HUNTER && (spellInfo->SpellFamilyFlags&0x0000000000000400LL))
                {
                    uint32 basevalue = triggeredByAura->GetBasePoints();
                    auraModifier->m_amount += (basevalue+1)/10;
                    if (auraModifier->m_amount > (basevalue+1)*4)
                        auraModifier->m_amount = (basevalue+1)*4;
                }
                break;
            case SPELL_AURA_MOD_CASTING_SPEED:
                // Skip melee hits or instant cast spells
                if (procSpell == NULL || GetSpellCastTime(procSpell) == 0)
                    continue;
                break;
            case SPELL_AURA_REFLECT_SPELLS_SCHOOL:
                // Skip Melee hits and spells ws wrong school
                if (procSpell == NULL || (auraModifier->m_miscvalue & procSpell->SchoolMask) == 0)
                    continue;
                break;
            case SPELL_AURA_MOD_POWER_COST_SCHOOL_PCT:
            case SPELL_AURA_MOD_POWER_COST_SCHOOL:
                // Skip melee hits and spells ws wrong school or zero cost
                if (procSpell == NULL ||
                    (procSpell->manaCost == 0 && procSpell->ManaCostPercentage == 0) || // Cost check
                    (auraModifier->m_miscvalue & procSpell->SchoolMask) == 0)         // School check
                    continue;
                break;
            case SPELL_AURA_MECHANIC_IMMUNITY:
                // Compare mechanic
                if (procSpell==NULL || procSpell->Mechanic != auraModifier->m_miscvalue)
                    continue;
                break;
            case SPELL_AURA_MOD_MECHANIC_RESISTANCE:
                // Compare mechanic
                if (procSpell==NULL || procSpell->Mechanic != auraModifier->m_miscvalue)
                    continue;
                break;
            default:
                // nothing do, just charges counter
                break;
        }
        // Remove charge (aura can be removed by triggers)
        if(useCharges && triggeredByAura->GetId() != 23920)
        {
            // need found aura on drop (can be dropped by triggers)
            AuraMap::const_iterator lower = GetAuras().lower_bound(i->triggeredByAura_SpellPair);
            AuraMap::const_iterator upper = GetAuras().upper_bound(i->triggeredByAura_SpellPair);
            for(AuraMap::const_iterator itr = lower; itr!= upper; ++itr)
            {
                if(itr->second == i->triggeredByAura)
                {
                     triggeredByAura->m_procCharges -=1;
                     triggeredByAura->UpdateAuraCharges();
                     if (triggeredByAura->m_procCharges <= 0)
                          removedSpells.push_back(triggeredByAura->GetId());
                    break;
                }
            }
        }
    }
    if (removedSpells.size())
    {
        // Sort spells and remove dublicates
        removedSpells.sort();
        removedSpells.unique();
        // Remove auras from removedAuras
        for(RemoveSpellList::const_iterator i = removedSpells.begin(); i != removedSpells.end();i++)
            RemoveAurasDueToSpell(*i);
    }
    --m_procDeep;
}

SpellSchoolMask Unit::GetMeleeDamageSchoolMask() const
{
    return SPELL_SCHOOL_MASK_NORMAL;
}

Player* Unit::GetSpellModOwner() const
{
    if(GetTypeId()==TYPEID_PLAYER) {
        Player *p = const_cast<Player*>(ToPlayer());
        return p;
    }

    if((this->ToCreature())->IsPet() || (this->ToCreature())->isTotem())
    {
        Unit* owner = GetOwner();
        if(owner && owner->GetTypeId()==TYPEID_PLAYER)
            return owner->ToPlayer();
    }
    return NULL;
}

///----------Pet responses methods-----------------
void Unit::SendPetCastFail(uint32 spellid, uint8 msg)
{
    Unit *owner = GetCharmerOrOwner();
    if(!owner || owner->GetTypeId() != TYPEID_PLAYER)
        return;

    WorldPacket data(SMSG_PET_CAST_FAILED, (4+1));
    data << uint32(spellid);
    data << uint8(msg);
    (owner->ToPlayer())->GetSession()->SendPacket(&data);
}

void Unit::SendPetActionFeedback (uint8 msg)
{
    Unit* owner = GetOwner();
    if(!owner || owner->GetTypeId() != TYPEID_PLAYER)
        return;

    WorldPacket data(SMSG_PET_ACTION_FEEDBACK, 1);
    data << uint8(msg);
    (owner->ToPlayer())->GetSession()->SendPacket(&data);
}

void Unit::SendPetTalk (uint32 pettalk)
{
    Unit* owner = GetOwner();
    if(!owner || owner->GetTypeId() != TYPEID_PLAYER)
        return;

    WorldPacket data(SMSG_PET_ACTION_SOUND, 8+4);
    data << uint64(GetGUID());
    data << uint32(pettalk);
    (owner->ToPlayer())->GetSession()->SendPacket(&data);
}

void Unit::SendPetSpellCooldown (uint32 spellid, time_t cooltime)
{
    Unit* owner = GetOwner();
    if(!owner || owner->GetTypeId() != TYPEID_PLAYER)
        return;

    WorldPacket data(SMSG_SPELL_COOLDOWN, 8+1+4+4);
    data << uint64(GetGUID());
    data << uint8(0x0);                                     // flags (0x1, 0x2)
    data << uint32(spellid);
    data << uint32(cooltime);

    (owner->ToPlayer())->GetSession()->SendPacket(&data);
}

void Unit::SendPetClearCooldown (uint32 spellid)
{
    Unit* owner = GetOwner();
    if(!owner || owner->GetTypeId() != TYPEID_PLAYER)
        return;

    WorldPacket data(SMSG_CLEAR_COOLDOWN, (4+8));
    data << uint32(spellid);
    data << uint64(GetGUID());
    (owner->ToPlayer())->GetSession()->SendPacket(&data);
}

void Unit::SendPetAIReaction(uint64 guid)
{
    Unit* owner = GetOwner();
    if(!owner || owner->GetTypeId() != TYPEID_PLAYER)
        return;

    WorldPacket data(SMSG_AI_REACTION, 12);
    data << uint64(guid) << uint32(00000002);
    (owner->ToPlayer())->GetSession()->SendPacket(&data);
}

///----------End of Pet responses methods----------

void Unit::StopMoving()
{
    clearUnitState(UNIT_STAT_MOVING);

    // send explicit stop packet
    // rely on vmaps here because for example stormwind is in air
    //float z = MapManager::Instance().GetBaseMap(GetMapId())->GetHeight(GetPositionX(), GetPositionY(), GetPositionZ(), true);
    //if (fabs(GetPositionZ() - z) < 2.0f)
    //    Relocate(GetPositionX(), GetPositionY(), z);
    Relocate(GetPositionX(), GetPositionY(),GetPositionZ());

    SendMonsterStop();

    // update position and orientation;
    WorldPacket data;
    BuildHeartBeatMsg(&data);
    SendMessageToSet(&data,false);
}

void Unit::SendMovementFlagUpdate()
{
    WorldPacket data;
    BuildHeartBeatMsg(&data);
    SendMessageToSet(&data, false);
}

void Unit::SendMovementFlagUpdate(float dist)
{
    WorldPacket data;
    BuildHeartBeatMsg(&data);
    SendMessageToSetInRange(&data, dist, false);
}

bool Unit::IsSitState() const
{
    uint8 s = getStandState();
    return s == PLAYER_STATE_SIT_CHAIR || s == PLAYER_STATE_SIT_LOW_CHAIR ||
        s == PLAYER_STATE_SIT_MEDIUM_CHAIR || s == PLAYER_STATE_SIT_HIGH_CHAIR ||
        s == PLAYER_STATE_SIT;
}

bool Unit::IsStandState() const
{
    uint8 s = getStandState();
    return !IsSitState() && s != PLAYER_STATE_SLEEP && s != PLAYER_STATE_KNEEL;
}

void Unit::SetStandState(uint8 state)
{
    SetByteValue(UNIT_FIELD_BYTES_1, 0, state);

    if (IsStandState())
       RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_NOT_SEATED);

    if(GetTypeId()==TYPEID_PLAYER)
    {
        WorldPacket data(SMSG_STANDSTATE_UPDATE, 1);
        data << (uint8)state;
        (this->ToPlayer())->GetSession()->SendPacket(&data);
    }
}

bool Unit::IsPolymorphed() const
{
    return GetSpellSpecific(getTransForm())==SPELL_MAGE_POLYMORPH;
}

void Unit::SetDisplayId(uint32 modelId)
{
    SetUInt32Value(UNIT_FIELD_DISPLAYID, modelId);

    if(GetTypeId() == TYPEID_UNIT && (this->ToCreature())->IsPet())
    {
        Pet *pet = ((Pet*)this);
        if(!pet->isControlled())
            return;
        Unit *owner = GetOwner();
        if(owner && (owner->GetTypeId() == TYPEID_PLAYER) && (owner->ToPlayer())->GetGroup())
            (owner->ToPlayer())->SetGroupUpdateFlag(GROUP_UPDATE_FLAG_PET_MODEL_ID);
    }
}

void Unit::ClearComboPointHolders()
{
    while(!m_ComboPointHolders.empty())
    {
        uint32 lowguid = *m_ComboPointHolders.begin();

        Player* plr = objmgr.GetPlayer(MAKE_NEW_GUID(lowguid, 0, HIGHGUID_PLAYER));
        if(plr && plr->GetComboTarget()==GetGUID())         // recheck for safe
            plr->ClearComboPoints();                        // remove also guid from m_ComboPointHolders;
        else
            m_ComboPointHolders.erase(lowguid);             // or remove manually
    }
}

void Unit::ClearAllReactives()
{

    for(int i=0; i < MAX_REACTIVE; ++i)
        m_reactiveTimer[i] = 0;

    if (HasAuraState( AURA_STATE_DEFENSE))
        ModifyAuraState(AURA_STATE_DEFENSE, false);
    if (getClass() == CLASS_HUNTER && HasAuraState( AURA_STATE_HUNTER_PARRY))
        ModifyAuraState(AURA_STATE_HUNTER_PARRY, false);
    if (HasAuraState( AURA_STATE_CRIT))
        ModifyAuraState(AURA_STATE_CRIT, false);
    if (getClass() == CLASS_HUNTER && HasAuraState( AURA_STATE_HUNTER_CRIT_STRIKE)  )
        ModifyAuraState(AURA_STATE_HUNTER_CRIT_STRIKE, false);

    if(getClass() == CLASS_WARRIOR && GetTypeId() == TYPEID_PLAYER)
        (this->ToPlayer())->ClearComboPoints();
}

void Unit::UpdateReactives( uint32 p_time )
{
    for(int i = 0; i < MAX_REACTIVE; ++i)
    {
        ReactiveType reactive = ReactiveType(i);

        if(!m_reactiveTimer[reactive])
            continue;

        if ( m_reactiveTimer[reactive] <= p_time)
        {
            m_reactiveTimer[reactive] = 0;

            switch ( reactive )
            {
                case REACTIVE_DEFENSE:
                    if (HasAuraState(AURA_STATE_DEFENSE))
                        ModifyAuraState(AURA_STATE_DEFENSE, false);
                    break;
                case REACTIVE_HUNTER_PARRY:
                    if ( getClass() == CLASS_HUNTER && HasAuraState(AURA_STATE_HUNTER_PARRY))
                        ModifyAuraState(AURA_STATE_HUNTER_PARRY, false);
                    break;
                case REACTIVE_CRIT:
                    if (HasAuraState(AURA_STATE_CRIT))
                        ModifyAuraState(AURA_STATE_CRIT, false);
                    break;
                case REACTIVE_HUNTER_CRIT:
                    if ( getClass() == CLASS_HUNTER && HasAuraState(AURA_STATE_HUNTER_CRIT_STRIKE) )
                        ModifyAuraState(AURA_STATE_HUNTER_CRIT_STRIKE, false);
                    break;
                case REACTIVE_OVERPOWER:
                    if(getClass() == CLASS_WARRIOR && GetTypeId() == TYPEID_PLAYER)
                        (this->ToPlayer())->ClearComboPoints();
                    break;
                default:
                    break;
            }
        }
        else
        {
            m_reactiveTimer[reactive] -= p_time;
        }
    }
}

Unit* Unit::SelectNearbyTarget(float dist) const
{
    std::list<Unit *> targets;
    Trinity::AnyUnfriendlyUnitInObjectRangeCheck u_check(this, this, dist);
    Trinity::UnitListSearcher<Trinity::AnyUnfriendlyUnitInObjectRangeCheck> searcher(targets, u_check);
    VisitNearbyObject(dist, searcher);

    // remove current target
    if(GetVictim())
        targets.remove(GetVictim());

    // remove not LoS targets
    for(std::list<Unit *>::iterator tIter = targets.begin(); tIter != targets.end();)
    {
        if(!IsWithinLOSInMap(*tIter))
        {
            std::list<Unit *>::iterator tIter2 = tIter;
            ++tIter;
            targets.erase(tIter2);
        }
        else
            ++tIter;
    }

    // no appropriate targets
    if(targets.empty())
        return NULL;

    // select random
    uint32 rIdx = GetMap()->urand(0,targets.size()-1);
    std::list<Unit *>::const_iterator tcIter = targets.begin();
    for(uint32 i = 0; i < rIdx; ++i)
        ++tcIter;

    return *tcIter;
}

void Unit::ApplyAttackTimePercentMod( WeaponAttackType att,float val, bool apply )
{
    //sLog.outDebug("ApplyAttackTimePercentMod(%u,%f,%s)",att,val,apply?"true":"false");
    float remainingTimePct = (float)m_attackTimer[att] / (GetAttackTime(att) * m_modAttackSpeedPct[att]);
    //sLog.outDebug("remainingTimePct = %f",remainingTimePct);    
    //sLog.outDebug("m_modAttackSpeedPct[att] before = %f",m_modAttackSpeedPct[att]);
    if(val > 0)
    {
        ApplyPercentModFloatVar(m_modAttackSpeedPct[att], val, !apply);
        ApplyPercentModFloatValue(UNIT_FIELD_BASEATTACKTIME+att,val,!apply);
    }
    else
    {
        ApplyPercentModFloatVar(m_modAttackSpeedPct[att], -val, apply);
        ApplyPercentModFloatValue(UNIT_FIELD_BASEATTACKTIME+att,-val,apply);
    }
    //sLog.outDebug("m_modAttackSpeedPct[att] after = %f",m_modAttackSpeedPct[att]);
    m_attackTimer[att] = uint32(GetAttackTime(att) * m_modAttackSpeedPct[att] * remainingTimePct);
}

void Unit::ApplyCastTimePercentMod(float val, bool apply )
{
    if(val > 0)
        ApplyPercentModFloatValue(UNIT_MOD_CAST_SPEED,val,!apply);
    else
        ApplyPercentModFloatValue(UNIT_MOD_CAST_SPEED,-val,apply);
}

uint32 Unit::GetCastingTimeForBonus( SpellEntry const *spellProto, DamageEffectType damagetype, uint32 CastingTime )
{
    // Not apply this to creature casted spells with casttime==0
    if(CastingTime==0 && GetTypeId()==TYPEID_UNIT && !(this->ToCreature())->IsPet())
        return 3500;

    if (CastingTime > 7000) CastingTime = 7000;
    if (CastingTime < 1500) CastingTime = 1500;

    if(damagetype == DOT && !IsChanneledSpell(spellProto))
        CastingTime = 3500;

    int32 overTime    = 0;
    uint8 effects     = 0;
    bool DirectDamage = false;
    bool AreaEffect   = false;

    for ( uint32 i=0; i<3;i++)
    {
        switch ( spellProto->Effect[i] )
        {
            case SPELL_EFFECT_SCHOOL_DAMAGE:
            case SPELL_EFFECT_POWER_DRAIN:
            case SPELL_EFFECT_HEALTH_LEECH:
            case SPELL_EFFECT_ENVIRONMENTAL_DAMAGE:
            case SPELL_EFFECT_POWER_BURN:
            case SPELL_EFFECT_HEAL:
                DirectDamage = true;
                break;
            case SPELL_EFFECT_APPLY_AURA:
                switch ( spellProto->EffectApplyAuraName[i] )
                {
                    case SPELL_AURA_PERIODIC_DAMAGE:
                    case SPELL_AURA_PERIODIC_HEAL:
                    case SPELL_AURA_PERIODIC_LEECH:
                        if ( GetSpellDuration(spellProto) )
                            overTime = GetSpellDuration(spellProto);
                        break;
                    default:
                        // -5% per additional effect
                        ++effects;
                        break;
                }
            default:
                break;
        }

        if(IsAreaEffectTarget[spellProto->EffectImplicitTargetA[i]] || IsAreaEffectTarget[spellProto->EffectImplicitTargetB[i]])
            AreaEffect = true;
    }

    // Combined Spells with Both Over Time and Direct Damage
    if ( overTime > 0 && CastingTime > 0 && DirectDamage )
    {
        // mainly for DoTs which are 3500 here otherwise
        uint32 OriginalCastTime = GetSpellCastTime(spellProto);
        if (OriginalCastTime > 7000) OriginalCastTime = 7000;
        if (OriginalCastTime < 1500) OriginalCastTime = 1500;
        // Portion to Over Time
        float PtOT = (overTime / 15000.f) / ((overTime / 15000.f) + (OriginalCastTime / 3500.f));

        if ( damagetype == DOT )
            CastingTime = uint32(CastingTime * PtOT);
        else if ( PtOT < 1.0f )
            CastingTime  = uint32(CastingTime * (1 - PtOT));
        else
            CastingTime = 0;
    }

    // Area Effect Spells receive only half of bonus
    if ( AreaEffect )
        CastingTime /= 2;

    // -5% of total per any additional effect
    for ( uint8 i=0; i<effects; ++i)
    {
        if ( CastingTime > 175 )
        {
            CastingTime -= 175;
        }
        else
        {
            CastingTime = 0;
            break;
        }
    }

    return CastingTime;
}

void Unit::UpdateAuraForGroup(uint8 slot)
{
    if(GetTypeId() == TYPEID_PLAYER)
    {
        Player* player = this->ToPlayer();
        if(player->GetGroup())
        {
            player->SetGroupUpdateFlag(GROUP_UPDATE_FLAG_AURAS);
            player->SetAuraUpdateMask(slot);
        }
    }
    else if(GetTypeId() == TYPEID_UNIT && (this->ToCreature())->IsPet())
    {
        Pet *pet = ((Pet*)this);
        if(pet->isControlled())
        {
            Unit *owner = GetOwner();
            if(owner && (owner->GetTypeId() == TYPEID_PLAYER) && (owner->ToPlayer())->GetGroup())
            {
                (owner->ToPlayer())->SetGroupUpdateFlag(GROUP_UPDATE_FLAG_PET_AURAS);
                pet->SetAuraUpdateMask(slot);
            }
        }
    }
}

float Unit::GetAPMultiplier(WeaponAttackType attType, bool normalized)
{
    if (!normalized || GetTypeId() != TYPEID_PLAYER)
        return float(GetAttackTime(attType))/1000.0f;

    Item *Weapon = (this->ToPlayer())->GetWeaponForAttack(attType);
    if (!Weapon)
        return 2.4;                                         // fist attack

    switch (Weapon->GetProto()->InventoryType)
    {
        case INVTYPE_2HWEAPON:
            return 3.3;
        case INVTYPE_RANGED:
        case INVTYPE_RANGEDRIGHT:
        case INVTYPE_THROWN:
            return 2.8;
        case INVTYPE_WEAPON:
        case INVTYPE_WEAPONMAINHAND:
        case INVTYPE_WEAPONOFFHAND:
        default:
            return Weapon->GetProto()->SubClass==ITEM_SUBCLASS_WEAPON_DAGGER ? 1.7 : 2.4;
    }
}

Aura* Unit::GetDummyAura( uint32 spell_id ) const
{
    Unit::AuraList const& mDummy = GetAurasByType(SPELL_AURA_DUMMY);
    for(Unit::AuraList::const_iterator itr = mDummy.begin(); itr != mDummy.end(); ++itr)
        if ((*itr)->GetId() == spell_id)
            return *itr;

    return NULL;
}

bool Unit::IsUnderLastManaUseEffect() const
{
    return  GetMSTimeDiff(m_lastManaUse,getMSTime()) < 5000;
}

void Unit::SetContestedPvP(Player *attackedPlayer)
{
    Player* player = GetCharmerOrOwnerPlayerOrPlayerItself();

    if(!player || attackedPlayer && (attackedPlayer == player || player->duel && player->duel->opponent == attackedPlayer))
        return;

    player->SetContestedPvPTimer(30000);
    if(!player->HasUnitState(UNIT_STAT_ATTACK_PLAYER))
    {
        player->addUnitState(UNIT_STAT_ATTACK_PLAYER);
        player->SetFlag(PLAYER_FLAGS, PLAYER_FLAGS_CONTESTED_PVP);
        // call MoveInLineOfSight for nearby contested guards
        player->SetVisibility(player->GetVisibility());
    }
    if(!HasUnitState(UNIT_STAT_ATTACK_PLAYER))
    {
        addUnitState(UNIT_STAT_ATTACK_PLAYER);
        // call MoveInLineOfSight for nearby contested guards
        SetVisibility(GetVisibility());
    }
}

void Unit::AddPetAura(PetAura const* petSpell)
{
    m_petAuras.insert(petSpell);
    if(Pet* pet = GetPet())
        pet->CastPetAura(petSpell);
}

void Unit::RemovePetAura(PetAura const* petSpell)
{
    m_petAuras.erase(petSpell);
    if(Pet* pet = GetPet())
        pet->RemoveAurasDueToSpell(petSpell->GetAura(pet->GetEntry()));
}

Pet* Unit::CreateTamedPetFrom(Creature* creatureTarget,uint32 spell_id)
{
    Pet* pet = new Pet(HUNTER_PET);

    if(!pet->CreateBaseAtCreature(creatureTarget))
    {
        delete pet;
        return NULL;
    }

    pet->SetOwnerGUID(GetGUID());
    pet->SetCreatorGUID(GetGUID());
    pet->SetUInt32Value(UNIT_FIELD_FACTIONTEMPLATE, getFaction());
    pet->SetUInt32Value(UNIT_CREATED_BY_SPELL, spell_id);

    if(!pet->InitStatsForLevel(creatureTarget->getLevel()))
    {
        sLog.outError("ERROR: Pet::InitStatsForLevel() failed for creature (Entry: %u)!",creatureTarget->GetEntry());
        delete pet;
        return NULL;
    }

    pet->GetCharmInfo()->SetPetNumber(objmgr.GeneratePetNumber(), true);
    // this enables pet details window (Shift+P)
    pet->AIM_Initialize();
    pet->InitPetCreateSpells();
    pet->SetHealth(pet->GetMaxHealth());

    return pet;
}

bool Unit::IsTriggeredAtSpellProcEvent(Aura* aura, SpellEntry const* procSpell, uint32 procFlag, uint32 procExtra, WeaponAttackType attType, bool isVictim, bool active, SpellProcEventEntry const*& spellProcEvent )
{
    SpellEntry const* spellProto = aura->GetSpellProto ();

    // Get proc Event Entry
    spellProcEvent = spellmgr.GetSpellProcEvent(spellProto->Id);

    // Aura info stored here
    Modifier *mod = aura->GetModifier();

    //sLog.outString("IsTriggeredAtSpellProcEvent1");
    // Skip this auras
    if (isNonTriggerAura[mod->m_auraname])
        return false;
    //sLog.outString("IsTriggeredAtSpellProcEvent2");
    // If not trigger by default and spellProcEvent==NULL - skip
    if (!isTriggerAura[mod->m_auraname] && spellProcEvent==NULL)
        return false;
    //sLog.outString("IsTriggeredAtSpellProcEvent3");
    // Get EventProcFlag
    uint32 EventProcFlag;
    if (spellProcEvent && spellProcEvent->procFlags) // if exist get custom spellProcEvent->procFlags
        EventProcFlag = spellProcEvent->procFlags;
    else
        EventProcFlag = spellProto->procFlags;       // else get from spell proto
    // Continue if no trigger exist
    if (!EventProcFlag)
        return false;
    //sLog.outString("IsTriggeredAtSpellProcEvent4");
    // Inner fire exception
    if (procFlag & PROC_FLAG_HAD_DAMAGE_BUT_ABSORBED && procExtra & PROC_EX_ABSORB) {
        if (spellProto->SpellVisual == 211 && spellProto->SpellFamilyName == 6)
            return false;
    }
    //sLog.outString("IsTriggeredAtSpellProcEvent5");
    // Check spellProcEvent data requirements
    if(!SpellMgr::IsSpellProcEventCanTriggeredBy(spellProcEvent, EventProcFlag, procSpell, procFlag, procExtra, active))
        return false;

    // Aura added by spell can`t trigger from self (prevent drop cahres/do triggers)
    // But except periodic triggers (can triggered from self)
    if(procSpell && procSpell->Id == spellProto->Id && !(spellProto->procFlags & PROC_FLAG_ON_TAKE_PERIODIC))
        return false;
    //sLog.outString("IsTriggeredAtSpellProcEvent6");
    // Check if current equipment allows aura to proc
    if(!isVictim && GetTypeId() == TYPEID_PLAYER)
    {
        if(spellProto->EquippedItemClass == ITEM_CLASS_WEAPON)
        {
            Item *item = NULL;
            if(attType == BASE_ATTACK)
                item = (this->ToPlayer())->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
            else if (attType == OFF_ATTACK)
                item = (this->ToPlayer())->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
            else
                item = (this->ToPlayer())->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);

            if (!(this->ToPlayer())->IsUseEquipedWeapon(attType==BASE_ATTACK))
                return false;

            if(!item || item->IsBroken() || item->GetProto()->Class != ITEM_CLASS_WEAPON || !((1<<item->GetProto()->SubClass) & spellProto->EquippedItemSubClassMask))
                return false;
        }
        else if(spellProto->EquippedItemClass == ITEM_CLASS_ARMOR)
        {
            // Check if player is wearing shield
            Item *item = (this->ToPlayer())->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
            if(!item || item->IsBroken() || item->GetProto()->Class != ITEM_CLASS_ARMOR || !((1<<item->GetProto()->SubClass) & spellProto->EquippedItemSubClassMask))
                return false;
        }
    }
    //sLog.outString("IsTriggeredAtSpellProcEvent7");
    // Get chance from spell
    float chance = (float)spellProto->procChance;
    // If in spellProcEvent exist custom chance, chance = spellProcEvent->customChance;
    if(spellProcEvent && spellProcEvent->customChance)
        chance = spellProcEvent->customChance;
    // If PPM exist calculate chance from PPM
    if(!isVictim && spellProcEvent && spellProcEvent->ppmRate != 0)
    {
        uint32 WeaponSpeed = GetAttackTime(attType);
        chance = GetPPMProcChance(WeaponSpeed, spellProcEvent->ppmRate);
    }
    // Apply chance modifer aura
    if(Player* modOwner = GetSpellModOwner())
        modOwner->ApplySpellMod(spellProto->Id,SPELLMOD_CHANCE_OF_SUCCESS,chance);

    //sLog.outString("IsTriggeredAtSpellProcEvent8");
    return roll_chance_f(chance);
}

bool Unit::HandleMendingAuraProc( Aura* triggeredByAura )
{
    // aura can be deleted at casts
    SpellEntry const* spellProto = triggeredByAura->GetSpellProto();
    uint32 effIdx = triggeredByAura->GetEffIndex();
    int32 heal = triggeredByAura->GetModifier()->m_amount;
    //uint64 caster_guid = triggeredByAura->GetCasterGUID();
    uint64 caster_guid = GetGUID();

    // jumps
    int32 jumps = triggeredByAura->m_procCharges-1;

    // current aura expire
    triggeredByAura->m_procCharges = 1;             // will removed at next charges decrease

    // next target selection
    if(jumps > 0 && GetTypeId()==TYPEID_PLAYER && IS_PLAYER_GUID(caster_guid))
    {
        float radius;
        if (spellProto->EffectRadiusIndex[effIdx])
            radius = GetSpellRadius(sSpellRadiusStore.LookupEntry(spellProto->EffectRadiusIndex[effIdx]));
        else
            radius = GetSpellMaxRange(sSpellRangeStore.LookupEntry(spellProto->rangeIndex));

        if(Player* caster = (triggeredByAura->GetCaster()->ToPlayer()))
        {
            caster->ApplySpellMod(spellProto->Id, SPELLMOD_RADIUS, radius,NULL);

            if(Player* target = (this->ToPlayer())->GetNextRandomRaidMember(radius))
            {
                // aura will applied from caster, but spell casted from current aura holder
                SpellModifier *mod = new SpellModifier;
                mod->op = SPELLMOD_CHARGES;
                mod->value = jumps-5;               // negative
                mod->type = SPELLMOD_FLAT;
                mod->spellId = spellProto->Id;
                mod->effectId = effIdx;
                mod->lastAffected = NULL;
                mod->mask = spellProto->SpellFamilyFlags;
                mod->charges = 0;

                caster->AddSpellMod(mod, true);
                CastCustomSpell(target,spellProto->Id,&heal,NULL,NULL,true,NULL,triggeredByAura,caster->GetGUID());
                caster->AddSpellMod(mod, false);
            }
            heal = caster->SpellHealingBonus(spellProto, heal, HEAL, this);
        }
    }
    // else double heal here?

    // heal
    CastCustomSpell(this,33110,&heal,NULL,NULL,true,NULL,NULL,caster_guid);
    return true;
}

void Unit::RemoveAurasAtChanneledTarget(SpellEntry const* spellInfo, Unit * caster)
{
/*    uint64 target_guid = GetUInt64Value(UNIT_FIELD_CHANNEL_OBJECT);
    if(target_guid == GetGUID())
        return;

    if(!IS_UNIT_GUID(target_guid))
        return;

    Unit* target = ObjectAccessor::GetUnit(*this, target_guid);*/
    if(!caster)
        return;

    for (AuraMap::iterator iter = GetAuras().begin(); iter != GetAuras().end(); )
    {
        if (iter->second->GetId() == spellInfo->Id && iter->second->GetCasterGUID() == caster->GetGUID())
            RemoveAura(iter);
        else
            ++iter;
    }
}

/*-----------------------TRINITY-----------------------------*/

void Unit::SetToNotify()
{
    if(m_IsInNotifyList)
        return;

    if(Map *map = GetMap())
        map->AddUnitToNotify(this);
}

void Unit::Kill(Unit *pVictim, bool durabilityLoss)
{
    pVictim->SetHealth(0);

    // find player: owner of controlled `this` or `this` itself maybe
    Player *player = GetCharmerOrOwnerPlayerOrPlayerItself();

    bool bRewardIsAllowed = true;
    if(pVictim->GetTypeId() == TYPEID_UNIT)
    {
        bRewardIsAllowed = (pVictim->ToCreature())->IsDamageEnoughForLootingAndReward();
        if(!bRewardIsAllowed)
            (pVictim->ToCreature())->SetLootRecipient(NULL);
    }
    
    if(bRewardIsAllowed && pVictim->GetTypeId() == TYPEID_UNIT && (pVictim->ToCreature())->GetLootRecipient())
        player = (pVictim->ToCreature())->GetLootRecipient();
    // Reward player, his pets, and group/raid members
    // call kill spell proc event (before real die and combat stop to triggering auras removed at death/combat stop)
    if(bRewardIsAllowed && player && player!=pVictim)
    {
        if(player->RewardPlayerAndGroupAtKill(pVictim))
            player->ProcDamageAndSpell(pVictim, PROC_FLAG_KILL_AND_GET_XP, PROC_FLAG_KILLED, PROC_EX_NONE, 0);
        else
            player->ProcDamageAndSpell(pVictim, PROC_FLAG_NONE, PROC_FLAG_KILLED,PROC_EX_NONE, 0);
    }

    // if talent known but not triggered (check priest class for speedup check)
    bool SpiritOfRedemption = false;
    if(pVictim->GetTypeId()==TYPEID_PLAYER && pVictim->getClass()==CLASS_PRIEST )
    {
        AuraList const& vDummyAuras = pVictim->GetAurasByType(SPELL_AURA_DUMMY);
        for(AuraList::const_iterator itr = vDummyAuras.begin(); itr != vDummyAuras.end(); ++itr)
        {
            if((*itr)->GetSpellProto()->SpellIconID==1654)
            {
                // save value before aura remove
                uint32 ressSpellId = pVictim->GetUInt32Value(PLAYER_SELF_RES_SPELL);
                if(!ressSpellId)
                    ressSpellId = (pVictim->ToPlayer())->GetResurrectionSpellId();
                //Remove all expected to remove at death auras (most important negative case like DoT or periodic triggers)
                pVictim->RemoveAllAurasOnDeath();
                // restore for use at real death
                pVictim->SetUInt32Value(PLAYER_SELF_RES_SPELL,ressSpellId);

                // FORM_SPIRITOFREDEMPTION and related auras
                pVictim->CastSpell(pVictim,27827,true,NULL,*itr);
                //pVictim->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE); // should not be attackable
                SpiritOfRedemption = true;
                (pVictim->ToPlayer())->SetSpiritRedeptionKiller(GetGUID());
                break;
            }
        }
    }

    if(!SpiritOfRedemption)
    {
        pVictim->setDeathState(JUST_DIED);
        //pVictim->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE); // reactive attackable flag
    }

    // 10% durability loss on death
    // clean InHateListOf
    if (pVictim->GetTypeId() == TYPEID_PLAYER)
    {
        // remember victim PvP death for corpse type and corpse reclaim delay
        // at original death (not at SpiritOfRedemtionTalent timeout)
        (pVictim->ToPlayer())->SetPvPDeath(player!=NULL);

        // only if not player and not controlled by player pet. And not at BG
        if (durabilityLoss && !player && !(pVictim->ToPlayer())->InBattleGround())
        {
            (pVictim->ToPlayer())->DurabilityLossAll(0.10f,false);
            // durability lost message
            WorldPacket data(SMSG_DURABILITY_DAMAGE_DEATH, 0);
            (pVictim->ToPlayer())->GetSession()->SendPacket(&data);
        }
        // Call KilledUnit for creatures
        if (GetTypeId() == TYPEID_UNIT && (this->ToCreature())->IsAIEnabled) {
            (this->ToCreature())->AI()->KilledUnit(pVictim);
            if (ToCreature()->getAI()) {
                ToCreature()->getAI()->onKill(pVictim);
            }
        }
            
        if (GetTypeId() == TYPEID_PLAYER) {
            if (Pet* minipet = ToPlayer()->GetMiniPet()) {
                if (minipet->IsAIEnabled)
                    minipet->AI()->MasterKilledUnit(pVictim);
            }
            if (Pet* pet = ToPlayer()->GetPet()) {
                if (pet->IsAIEnabled)
                    pet->AI()->MasterKilledUnit(pVictim);
            }
            for (uint8 slot = 0; slot < MAX_TOTEM; slot++) {
                if (Creature* totem = Unit::GetCreature(*this, m_TotemSlot[slot]))
                    totem->AI()->MasterKilledUnit(pVictim);
            }
            if (Creature* totem = Unit::GetCreature(*this, m_TotemSlot254)) // Slot for some quest totems
                totem->AI()->MasterKilledUnit(pVictim);
        }

        // last damage from non duel opponent or opponent controlled creature
        if((pVictim->ToPlayer())->duel)
        {
            (pVictim->ToPlayer())->duel->opponent->CombatStopWithPets(true);
            (pVictim->ToPlayer())->CombatStopWithPets(true);
            (pVictim->ToPlayer())->DuelComplete(DUEL_INTERUPTED);
        }
        
        if (ScriptedInstance* instance = ((ScriptedInstance*)pVictim->GetInstanceData()))
            instance->PlayerDied(pVictim->ToPlayer());
    }
    else                                                // creature died
    {
        Creature *cVictim = pVictim->ToCreature();
        
        if(GetTypeId() == TYPEID_PLAYER)
        {
            WorldPacket data(SMSG_PARTYKILLLOG, (8+8));
            data << GetGUID() << pVictim->GetGUID();
            SendMessageToSet(&data, true);
        } 

        if(!cVictim->IsPet())
        {
            cVictim->DeleteThreatList();
            if(!cVictim->GetFormation() || !cVictim->GetFormation()->isLootLinked(cVictim)) //the flag is set when whole group is dead for those with linked loot 
                cVictim->SetFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE);
        }

        // Call KilledUnit for creatures, this needs to be called after the lootable flag is set
        if (GetTypeId() == TYPEID_UNIT && (this->ToCreature())->IsAIEnabled) {
            (this->ToCreature())->AI()->KilledUnit(pVictim);
            if (ToCreature()->getAI())
                ToCreature()->getAI()->onKill(pVictim);
        }
            
        if (GetTypeId() == TYPEID_PLAYER) {
            if (Pet* minipet = ToPlayer()->GetMiniPet()) {
                if (minipet->IsAIEnabled)
                    minipet->AI()->MasterKilledUnit(pVictim);
            }
            if (Pet* pet = ToPlayer()->GetPet()) {
                if (pet->IsAIEnabled)
                    pet->AI()->MasterKilledUnit(pVictim);
            }
            for (uint8 slot = 0; slot < MAX_TOTEM; slot++) {
                if (Creature* totem = Unit::GetCreature(*this, m_TotemSlot[slot]))
                    totem->AI()->MasterKilledUnit(pVictim);
            }
            if (Creature* totem = Unit::GetCreature(*this, m_TotemSlot254)) // Slot for some quest totems
                totem->AI()->MasterKilledUnit(pVictim);
        }

        // Call creature just died function
        if (cVictim->IsAIEnabled) {
            cVictim->AI()->JustDied(this);
            if (cVictim->getAI())
                cVictim->getAI()->onDeath(this);
        }
        
        // Despawn creature pet if alive
        if (Pet* pet = cVictim->GetPet()) {
            if (pet->IsAlive())
                pet->DisappearAndDie();
        }
        
        // Log down if worldboss
        if (cVictim->isWorldBoss() && (cVictim->GetMap()->IsRaid() || cVictim->GetMap()->IsCommon())) {
            if (Player* killingPlayer = GetCharmerOrOwnerPlayerOrPlayerItself()) {
                std::map<uint32, uint32> guildOccurs;
                uint8 groupSize = 0;
                uint32 downByGuildId = 0;
                uint32 leaderGuid = 0;
                float guildPercentage = 0;
                bool mustLog = true;

                if (Group* group = killingPlayer->GetGroup()) {
                    leaderGuid = group->GetLeaderGUID();
                    groupSize = group->GetMembersCount();
                    for (GroupReference* gr = group->GetFirstMember(); gr != NULL; gr = gr->next())
                    {
                        if (Player* groupGuy = gr->getSource())
                            guildOccurs[groupGuy->GetGuildId()]++;
                    }
                }

                if (groupSize) {
                    for (std::map<uint32, uint32>::iterator itr = guildOccurs.begin(); itr != guildOccurs.end(); itr++) {
                        guildPercentage = ((float)itr->second / groupSize) * 100;
                        if (guildPercentage >= 67.0f) {
                            downByGuildId = itr->first;
                            break;
                        }
                    }                    
                }
                else
                    mustLog = false; // Don't log solo'ing

                const char* frName = cVictim->GetNameForLocaleIdx(0);
                const char* guildname = "Groupe pickup";
                if (downByGuildId)
                    guildname = objmgr.GetGuildNameById(downByGuildId).c_str();
                uint32 logEntry = cVictim->GetEntry();
                
                // Special cases
                switch (logEntry) {
                case 15192: // Anachronos
                    mustLog = false;
                    break;
                case 23420: // Essence of Desire -> Reliquary of Souls
                    frName = "Reliquaire des Ames";
                    break;
                case 23418: // Ros
                case 23419:
                case 22856:
                case 18835: // Maulgar adds
                case 18836:
                case 18834:
                case 18832:
                    mustLog = false;
                    break;
                case 22949: // Illidari Council 1st member, kept for logging
                    frName = "Conseil Illidari";
                    break;
                case 22950:
                case 22951:
                case 22952:
                case 15302: // Shadow of Taerar
                case 17256:
                    mustLog = false;
                    break;
                case 25165: // Eredar Twins, log only if both are defeated
                case 25166:
                {
                    frName = "Jumelles Eredar";
                    InstanceData *pInstance = (((InstanceMap*)(cVictim->GetMap()))->GetInstanceData());
                    if (pInstance && pInstance->GetData(4) != 3)
                        mustLog = false;
                    break;
                }
                case 17533: // Romulo
                    mustLog = false;
                    break;
                case 17534: // Julianne
                    frName = "Romulo et Julianne";
                    break;
                default:
                    break;
                }

                if (mustLog)
                    LogsDatabase.PQuery("INSERT INTO boss_down (boss_entry, boss_name, guild_id, guild_name, time, guild_percentage, leaderGuid) VALUES (%u, \"%s\", %u, \"%s\", %u, %.2f, %u)", cVictim->GetEntry(), frName, downByGuildId, guildname, time(NULL), guildPercentage, leaderGuid);
            }
        }

        // Dungeon specific stuff, only applies to players killing creatures
        if(cVictim->GetInstanceId())
        {
            ScriptedInstance *pInstance = ((ScriptedInstance*)cVictim->GetInstanceData());
            if (pInstance)
                pInstance->OnCreatureKill(cVictim);

            Map *m = cVictim->GetMap();
            Player *creditedPlayer = GetCharmerOrOwnerPlayerOrPlayerItself();
            // TODO: do instance binding anyway if the charmer/owner is offline

            if(m->IsDungeon() && creditedPlayer)
            {
                if(m->IsRaid() || m->IsHeroic())
                {
                    if(cVictim->GetCreatureInfo()->flags_extra & CREATURE_FLAG_EXTRA_INSTANCE_BIND)
                        ((InstanceMap *)m)->PermBindAllPlayers(creditedPlayer);
                }
                else
                {
                    // the reset time is set but not added to the scheduler
                    // until the players leave the instance
                    time_t resettime = cVictim->GetRespawnTimeEx() + 2 * HOUR;
                    if(InstanceSave *save = sInstanceSaveManager.GetInstanceSave(cVictim->GetInstanceId()))
                        if(save->GetResetTime() < resettime) save->SetResetTime(resettime);
                }
            }
        }
    }

    // outdoor pvp things, do these after setting the death state, else the player activity notify won't work... doh...
    // handle player kill only if not suicide (spirit of redemption for example)
    if(player && this != pVictim)
        if(OutdoorPvP * pvp = player->GetOutdoorPvP())
            pvp->HandleKill(player, pVictim);

    if(pVictim->GetTypeId() == TYPEID_PLAYER)
        if(OutdoorPvP * pvp = (pVictim->ToPlayer())->GetOutdoorPvP())
            pvp->HandlePlayerActivityChanged(pVictim->ToPlayer());

    // battleground things (do this at the end, so the death state flag will be properly set to handle in the bg->handlekill)
    if(player && player->InBattleGround() && !SpiritOfRedemption)
    {
        if(BattleGround *bg = player->GetBattleGround())
        {
            if(pVictim->GetTypeId() == TYPEID_PLAYER)
                bg->HandleKillPlayer(pVictim->ToPlayer(), player);
            else
                bg->HandleKillUnit(pVictim->ToCreature(), player);
        }
    }
}

void Unit::SetControlled(bool apply, UnitState state)
{
    if(apply)
    {
        if(HasUnitState(state))
            return;

        addUnitState(state);

        switch(state)
        {
        case UNIT_STAT_STUNNED:
            SetStunned(true);
            break;
        case UNIT_STAT_ROOT:
            if(!HasUnitState(UNIT_STAT_STUNNED))
                SetRooted(true);
            break;
        case UNIT_STAT_CONFUSED:
            if(!HasUnitState(UNIT_STAT_STUNNED))
                SetConfused(true);
            break;
        case UNIT_STAT_FLEEING:
            if(!HasUnitState(UNIT_STAT_STUNNED | UNIT_STAT_CONFUSED))
                SetFeared(true);
            break;
        default:
            break;
        }
    }
    else
    {
        switch(state)
        {
            case UNIT_STAT_STUNNED: if(HasAuraType(SPELL_AURA_MOD_STUN))    return;
                                    else    SetStunned(false);    break;
            case UNIT_STAT_ROOT:    if(HasAuraType(SPELL_AURA_MOD_ROOT))    return;
                                    else    SetRooted(false);     break;
            case UNIT_STAT_CONFUSED:if(HasAuraType(SPELL_AURA_MOD_CONFUSE)) return;
                                    else    SetConfused(false);   break;
            case UNIT_STAT_FLEEING: if(HasAuraType(SPELL_AURA_MOD_FEAR))    return;
                                    else    SetFeared(false);     break;
            default: return;
        }

        clearUnitState(state);

        if(HasUnitState(UNIT_STAT_STUNNED))
            SetStunned(true);
        else
        {
            if(HasUnitState(UNIT_STAT_ROOT))
                SetRooted(true);

            if(HasUnitState(UNIT_STAT_CONFUSED))
                SetConfused(true);
            else if(HasUnitState(UNIT_STAT_FLEEING))
                SetFeared(true);
        }
    }
}

void Unit::SetStunned(bool apply)
{
    if(apply)
    {
        SetTarget(0);
        SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_DISABLE_ROTATE);
        CastStop();

        RemoveUnitMovementFlag(MOVEMENTFLAG_MOVING);
        SetUnitMovementFlags(MOVEMENTFLAG_ROOT);

        // Creature specific
        if(GetTypeId() != TYPEID_PLAYER)
            (this->ToCreature())->StopMoving();
        else
            SetStandState(UNIT_STAND_STATE_STAND);

        WorldPacket data(SMSG_FORCE_MOVE_ROOT, 8);
        data.append(GetPackGUID());
        data << uint32(0);
        SendMessageToSet(&data,true);
    }
    else
    {
        AttackStop(); //This will reupdate current victim. patch 2.4.3 : When a stun wears off, the creature that was stunned will prefer the last target with the highest threat, versus the current target. 

        // don't remove UNIT_FLAG_DISABLE_ROTATE for pet when owner is mounted (disabled pet's interface)
        Unit *pOwner = GetOwner();
        if(!pOwner || (pOwner->GetTypeId() == TYPEID_PLAYER && !(pOwner->ToPlayer())->IsMounted()))
            RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_DISABLE_ROTATE);

        if(!HasUnitState(UNIT_STAT_ROOT))         // prevent allow move if have also root effect
        {
            WorldPacket data(SMSG_FORCE_MOVE_UNROOT, 8+4);
            data.append(GetPackGUID());
            data << ++m_rootTimes;
            SendMessageToSet(&data,true);

            RemoveUnitMovementFlag(MOVEMENTFLAG_ROOT);
        }
    }
}

void Unit::SetRooted(bool apply)
{
    //uint32 apply_stat = UNIT_STAT_ROOT;
    if(apply)
    {
        if (m_rootTimes > 0) // blizzard internal check?
            m_rootTimes++;

        // MOVEMENTFLAG_ROOT cannot be used in conjunction with MOVEMENTFLAG_MASK_MOVING (tested 3.3.5a)
        // this will freeze clients. That's why we remove MOVEMENTFLAG_MASK_MOVING before
        // setting MOVEMENTFLAG_ROOT
        //RemoveUnitMovementFlag(MOVEMENTFLAG_MOVING);
        //AddUnitMovementFlag(MOVEMENTFLAG_ROOT);
        //removed for now, this is causing creature to visually teleport after root is removed

        //SetFlag(UNIT_FIELD_FLAGS,(apply_stat<<16)); // probably wrong

        if(GetTypeId() == TYPEID_PLAYER)
        {
            WorldPacket data(SMSG_FORCE_MOVE_ROOT, 10);
            data.append(GetPackGUID());
            data << m_rootTimes;
            SendMessageToSet(&data,true);
        }
        else
            (this->ToCreature())->StopMoving();
    }
    else
    {
        //RemoveUnitMovementFlag(MOVEMENTFLAG_ROOT);

        //RemoveFlag(UNIT_FIELD_FLAGS,(apply_stat<<16)); // probably wrong

        if(!HasUnitState(UNIT_STAT_STUNNED))      // prevent allow move if have also stun effect
        {
            if(GetTypeId() == TYPEID_PLAYER)
            {
                WorldPacket data(SMSG_FORCE_MOVE_UNROOT, 10);
                data.append(GetPackGUID());
                data << m_rootTimes;
                SendMessageToSet(&data,true);
            }
        }
    }
}

void Unit::SetFeared(bool apply)
{
    if(apply)
    {
        Unit *caster = NULL;
        Unit::AuraList const& fearAuras = GetAurasByType(SPELL_AURA_MOD_FEAR);
        if(!fearAuras.empty())
            caster = ObjectAccessor::GetUnit(*this, fearAuras.front()->GetCasterGUID());
        if(!caster)
            caster = getAttackerForHelper();
        GetMotionMaster()->MoveFleeing(caster);             // caster==NULL processed in MoveFleeing
    }
    else
    {
        AttackStop();  //This will reupdate current victim. patch 2.4.3 : When a stun wears off, the creature that was stunned will prefer the last target with the highest threat, versus the current target. I'm not sure this should apply to confuse but this seems logical.
        if(IsAlive() && GetMotionMaster()->GetCurrentMovementGeneratorType() == FLEEING_MOTION_TYPE)
            GetMotionMaster()->MovementExpired();
    }

    if (GetTypeId() == TYPEID_PLAYER)
        (this->ToPlayer())->SetClientControl(this, !apply);
}

void Unit::SetConfused(bool apply)
{
    if(apply)
    {
        GetMotionMaster()->MoveConfused();
    }
    else
    {
        AttackStop();  //This will reupdate current victim. patch 2.4.3 : When a stun wears off, the creature that was stunned will prefer the last target with the highest threat, versus the current target. I'm not sure this should apply to fear but this seems logical.
        if(IsAlive() && GetMotionMaster()->GetCurrentMovementGeneratorType() == CONFUSED_MOTION_TYPE)
            GetMotionMaster()->MovementExpired();
    }

    if(GetTypeId() == TYPEID_PLAYER)
        (this->ToPlayer())->SetClientControl(this, !apply);
}

void Unit::SetCharmedOrPossessedBy(Unit* charmer, bool possess)
{
    if(!charmer)
        return;

    assert(!possess || charmer->GetTypeId() == TYPEID_PLAYER);

    if(this == charmer)
        return;

    if(isInFlight())
        return;

    if(GetTypeId() == TYPEID_PLAYER && (this->ToPlayer())->GetTransport())
        return;

    if(ToCreature())
        ToCreature()->SetWalk(false,false);
    else
        RemoveUnitMovementFlag(MOVEMENTFLAG_WALK_MODE);
    CastStop();
    CombatStop(); //TODO: CombatStop(true) may cause crash (interrupt spells)
    DeleteThreatList();

    // Charmer stop charming
    if(charmer->GetTypeId() == TYPEID_PLAYER)
        (charmer->ToPlayer())->StopCastingCharm();

    // Charmed stop charming
    if(GetTypeId() == TYPEID_PLAYER)
        (this->ToPlayer())->StopCastingCharm();

    // StopCastingCharm may remove a possessed pet?
    if(!IsInWorld())
        return;

    // Set charmed
    charmer->SetCharm(this);
    SetCharmerGUID(charmer->GetGUID());
    setFaction(charmer->getFaction());
    SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PVP_ATTACKABLE);

    if(GetTypeId() == TYPEID_UNIT)
    {
        (this->ToCreature())->AI()->OnCharmed(charmer, true);
        GetMotionMaster()->Clear(false);
        GetMotionMaster()->MoveIdle();
    }
    else
    {
        if((this->ToPlayer())->isAFK())
            (this->ToPlayer())->ToggleAFK();
        (this->ToPlayer())->SetViewport(GetGUID(), false);
    }

    // Pets already have a properly initialized CharmInfo, don't overwrite it.
    if(GetTypeId() == TYPEID_PLAYER || GetTypeId() == TYPEID_UNIT && !(this->ToCreature())->IsPet())
    {
        CharmInfo *charmInfo = InitCharmInfo();
        if(possess)
            charmInfo->InitPossessCreateSpells();
        else
            charmInfo->InitCharmCreateSpells();
    }

    //Set possessed
    if(possess)
    {
        addUnitState(UNIT_STAT_POSSESSED);
        SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PLAYER_CONTROLLED);
        AddPlayerToVision(charmer->ToPlayer());
        (charmer->ToPlayer())->SetViewport(GetGUID(), true);
        charmer->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_DISABLE_MOVE);
    }
    // Charm demon
    else if(GetTypeId() == TYPEID_UNIT && charmer->GetTypeId() == TYPEID_PLAYER && charmer->getClass() == CLASS_WARLOCK)
    {
        CreatureInfo const *cinfo = (this->ToCreature())->GetCreatureInfo();
        if(cinfo && cinfo->type == CREATURE_TYPE_DEMON)
        {
            //to prevent client crash
            SetFlag(UNIT_FIELD_BYTES_0, 2048);

            //just to enable stat window
            if(GetCharmInfo())
                GetCharmInfo()->SetPetNumber(objmgr.GeneratePetNumber(), true);

            //if charmed two demons the same session, the 2nd gets the 1st one's name
            SetUInt32Value(UNIT_FIELD_PET_NAME_TIMESTAMP, time(NULL));
        }
    }

    if(possess)
        (charmer->ToPlayer())->PossessSpellInitialize();
    else if(charmer->GetTypeId() == TYPEID_PLAYER)
        (charmer->ToPlayer())->CharmSpellInitialize();
}

void Unit::RemoveCharmedOrPossessedBy(Unit *charmer)
{
    if(!isCharmed())
        return;

    if(!charmer)
        charmer = GetCharmer();
    else if(charmer != GetCharmer()) // one aura overrides another?
        return;

    bool possess = HasUnitState(UNIT_STAT_POSSESSED);

    CastStop();
    CombatStop(); //TODO: CombatStop(true) may cause crash (interrupt spells)
    getHostilRefManager().deleteReferences();
    DeleteThreatList();
    SetCharmerGUID(0);
    RestoreFaction();

    if(possess)
    {
        clearUnitState(UNIT_STAT_POSSESSED);
        RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PLAYER_CONTROLLED);
    }

    if(GetTypeId() == TYPEID_UNIT)
    {
        if(!(this->ToCreature())->IsPet())
            RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PVP_ATTACKABLE);

        (this->ToCreature())->AI()->OnCharmed(charmer, false);
        if(IsAlive() && (this->ToCreature())->IsAIEnabled)
        {
            if(charmer && !IsFriendlyTo(charmer))
            {
                (this->ToCreature())->AddThreat(charmer, 10000.0f);
                (this->ToCreature())->AI()->AttackStart(charmer);
                if (ToCreature()->getAI())
                    ToCreature()->getAI()->attackStart(charmer);
            }
            else {
                (this->ToCreature())->AI()->EnterEvadeMode();
                if (this->ToCreature()->getAI())
                    this->ToCreature()->getAI()->evade();
            }
        }
    }
    else
        (this->ToPlayer())->SetViewport(GetGUID(), true);

    // If charmer still exists
    if(!charmer)
        return;

    assert(!possess || charmer->GetTypeId() == TYPEID_PLAYER);

    charmer->SetCharm(0);
    if(possess)
    {
        RemovePlayerFromVision(charmer->ToPlayer());
        (charmer->ToPlayer())->SetViewport(charmer->GetGUID(), true);
        charmer->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_DISABLE_MOVE);
    }
    // restore UNIT_FIELD_BYTES_0
    else if(GetTypeId() == TYPEID_UNIT && charmer->GetTypeId() == TYPEID_PLAYER && charmer->getClass() == CLASS_WARLOCK)
    {
        CreatureInfo const *cinfo = (this->ToCreature())->GetCreatureInfo();
        if(cinfo && cinfo->type == CREATURE_TYPE_DEMON)
        {
            CreatureDataAddon const *cainfo = (this->ToCreature())->GetCreatureAddon();
            if(cainfo && cainfo->bytes0 != 0)
                SetUInt32Value(UNIT_FIELD_BYTES_0, cainfo->bytes0);
            else
                RemoveFlag(UNIT_FIELD_BYTES_0, 2048);

            if(GetCharmInfo())
                GetCharmInfo()->SetPetNumber(0, true);
            else
                sLog.outError("Aura::HandleModCharm: target=" I64FMTD " with typeid=%d has a charm aura but no charm info!", GetGUID(), GetTypeId());
        }
    }

    if(GetTypeId() == TYPEID_PLAYER || GetTypeId() == TYPEID_UNIT && !(this->ToCreature())->IsPet())
    {
        DeleteCharmInfo();
    }

    if(possess || charmer->GetTypeId() == TYPEID_PLAYER)
    {
        // Remove pet spell action bar
        WorldPacket data(SMSG_PET_SPELLS, 8);
        data << uint64(0);
        (charmer->ToPlayer())->GetSession()->SendPacket(&data);
    }
}

void Unit::RestoreFaction()
{
    if(GetTypeId() == TYPEID_PLAYER)
        (this->ToPlayer())->setFactionForRace(getRace());
    else
    {
        CreatureInfo const *cinfo = (this->ToCreature())->GetCreatureInfo();

        if((this->ToCreature())->IsPet())
        {
            if(Unit* owner = GetOwner())
                setFaction(owner->getFaction());
            else if(cinfo)
                setFaction(cinfo->faction_A);
        }
        else if(cinfo)  // normal creature
            setFaction(cinfo->faction_A);
    }
}

bool Unit::IsInPartyWith(Unit const *unit) const
{
    if(this == unit)
        return true;

    const Unit *u1 = GetCharmerOrOwnerOrSelf();
    const Unit *u2 = unit->GetCharmerOrOwnerOrSelf();
    if(u1 == u2)
        return true;

    if(u1->GetTypeId() == TYPEID_PLAYER && u2->GetTypeId() == TYPEID_PLAYER)
        return (u1->ToPlayer())->IsInSameGroupWith(u2->ToPlayer());
    else
        return false;
}

bool Unit::IsInRaidWith(Unit const *unit) const
{
    if(this == unit)
        return true;

    const Unit *u1 = GetCharmerOrOwnerOrSelf();
    const Unit *u2 = unit->GetCharmerOrOwnerOrSelf();
    if(u1 == u2)
        return true;

    if(u1->GetTypeId() == TYPEID_PLAYER && u2->GetTypeId() == TYPEID_PLAYER)
        return (u1->ToPlayer())->IsInSameRaidWith(u2->ToPlayer());
    else
        return false;
}

void Unit::GetRaidMember(std::list<Unit*> &nearMembers, float radius)
{
    Player *owner = GetCharmerOrOwnerPlayerOrPlayerItself();
    if(!owner)
        return;

    Group *pGroup = owner->GetGroup();
    if(!pGroup)
        return;

    for(GroupReference *itr = pGroup->GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player* Target = itr->getSource();

        // IsHostileTo check duel and controlled by enemy
        if( Target && Target != this && Target->IsAlive()
            && IsWithinDistInMap(Target, radius) && !IsHostileTo(Target) )
            nearMembers.push_back(Target);
    }
}

void Unit::GetPartyMember(std::list<Unit*> &TagUnitMap, float radius)
{
    Unit *owner = GetCharmerOrOwnerOrSelf();
    Group *pGroup = NULL;
    if (owner->GetTypeId() == TYPEID_PLAYER)
        pGroup = (owner->ToPlayer())->GetGroup();

    if(pGroup)
    {
        uint8 subgroup = (owner->ToPlayer())->GetSubGroup();

        for(GroupReference *itr = pGroup->GetFirstMember(); itr != NULL; itr = itr->next())
        {
            Player* Target = itr->getSource();

            // IsHostileTo check duel and controlled by enemy
            if( Target && Target->GetSubGroup()==subgroup && !IsHostileTo(Target) )
            {
                if(Target->IsAlive() && IsWithinDistInMap(Target, radius) )
                    TagUnitMap.push_back(Target);

                if(Pet* pet = Target->GetPet())
                    if(pet->IsAlive() &&  IsWithinDistInMap(pet, radius) )
                        TagUnitMap.push_back(pet);
            }
        }
    }
    else
    {
        if(owner->IsAlive() && (owner == this || IsWithinDistInMap(owner, radius)))
            TagUnitMap.push_back(owner);
        if(Pet* pet = owner->GetPet())
            if(pet->IsAlive() && (pet == this && IsWithinDistInMap(pet, radius)))
                TagUnitMap.push_back(pet);
    }
}

void Unit::AddAura(uint32 spellId, Unit* target)
{
    SpellEntry const *spellInfo = spellmgr.LookupSpell(spellId);
    if(!spellInfo)
        return;
        
    if(!target || (!target->IsAlive() && !(spellInfo->Attributes & SPELL_ATTR_CASTABLE_WHILE_DEAD)))
        return;

    if (target->IsImmunedToSpell(spellInfo))
        return;
        
    for(uint32 i = 0; i < 3; ++i)
    {
        if(spellInfo->Effect[i] == SPELL_EFFECT_APPLY_AURA)
        {
            if(target->IsImmunedToSpellEffect(spellInfo->Effect[i], spellInfo->EffectMechanic[i]))
                continue;

            /*if(spellInfo->EffectImplicitTargetA[i] == TARGET_UNIT_CASTER)
            {
                Aura *Aur = CreateAura(spellInfo, i, NULL, this, this);
                AddAura(Aur);
            }
            else*/
            {
                Aura *Aur = CreateAura(spellInfo, i, NULL, target, this);
                target->AddAura(Aur);
            }
        }
    }
}

Creature* Unit::FindCreatureInGrid(uint32 entry, float range, bool isAlive)
{
    Creature* pCreature = NULL;

    CellPair pair(Trinity::ComputeCellPair(this->GetPositionX(), this->GetPositionY()));
    Cell cell(pair);
    cell.data.Part.reserved = ALL_DISTRICT;
    cell.SetNoCreate();

    Trinity::NearestCreatureEntryWithLiveStateInObjectRangeCheck creature_check(*this, entry, isAlive, range);
    Trinity::CreatureLastSearcher<Trinity::NearestCreatureEntryWithLiveStateInObjectRangeCheck> searcher(pCreature, creature_check);

    TypeContainerVisitor<Trinity::CreatureLastSearcher<Trinity::NearestCreatureEntryWithLiveStateInObjectRangeCheck>, GridTypeMapContainer> creature_searcher(searcher);

    cell.Visit(pair, creature_searcher, *GetMap());
    
    return pCreature;
}

Player* Unit::FindPlayerInGrid(float range, bool isAlive)
{
    Player* pPlayer = NULL;

    CellPair pair(Trinity::ComputeCellPair(this->GetPositionX(), this->GetPositionY()));
    Cell cell(pair);
    cell.data.Part.reserved = ALL_DISTRICT;
    cell.SetNoCreate();

    Trinity::NearestPlayerInObjectRangeCheck creature_check(*this, isAlive, range);
    Trinity::PlayerSearcher<Trinity::NearestPlayerInObjectRangeCheck> searcher(pPlayer, creature_check);

    TypeContainerVisitor<Trinity::PlayerSearcher<Trinity::NearestPlayerInObjectRangeCheck>, GridTypeMapContainer> player_searcher(searcher);

    cell.Visit(pair, player_searcher, *GetMap());
    
    return pPlayer;
}

GameObject* Unit::FindGOInGrid(uint32 entry, float range)
{
    GameObject* pGo = NULL;

    CellPair pair(Trinity::ComputeCellPair(this->GetPositionX(), this->GetPositionY()));
    Cell cell(pair);
    cell.data.Part.reserved = ALL_DISTRICT;
    cell.SetNoCreate();

    Trinity::NearestGameObjectEntryInObjectRangeCheck go_check(*this, entry, range);
    Trinity::GameObjectLastSearcher<Trinity::NearestGameObjectEntryInObjectRangeCheck> searcher(pGo, go_check);

    TypeContainerVisitor<Trinity::GameObjectLastSearcher<Trinity::NearestGameObjectEntryInObjectRangeCheck>, GridTypeMapContainer> go_searcher(searcher);

    cell.Visit(pair, go_searcher, *GetMap());
    
    return pGo;
}

void Unit::SetFullTauntImmunity(bool apply)
{
    ApplySpellImmune(0, IMMUNITY_ID, 31789, apply);
    ApplySpellImmune(0, IMMUNITY_ID, 39377, apply);
    ApplySpellImmune(0, IMMUNITY_ID, 54794, apply);
    ApplySpellImmune(0, IMMUNITY_ID, 37017, apply);
    ApplySpellImmune(0, IMMUNITY_ID, 37486, apply);
    ApplySpellImmune(0, IMMUNITY_ID, 49613, apply);
    ApplySpellImmune(0, IMMUNITY_ID, 694, apply);
    ApplySpellImmune(0, IMMUNITY_ID, 25266, apply);
    ApplySpellImmune(0, IMMUNITY_ID, 39270, apply);
    ApplySpellImmune(0, IMMUNITY_ID, 27344, apply);
    ApplySpellImmune(0, IMMUNITY_ID, 6795, apply);
    ApplySpellImmune(0, IMMUNITY_ID, 39270, apply);
    ApplySpellImmune(0, IMMUNITY_ID, 1161, apply);
    ApplySpellImmune(0, IMMUNITY_ID, 5209, apply);
    ApplySpellImmune(0, IMMUNITY_ID, 355, apply);
    ApplySpellImmune(0, IMMUNITY_ID, 34105, apply);
    ApplySpellImmune(0, IMMUNITY_ID, 53477, apply);
}

void Unit::MonsterMoveByPath(float x, float y, float z, uint32 speed, bool smoothPath)
{
    PathInfo path(this, x, y, z, !smoothPath, true);
    PointPath pointPath = path.getFullPath();

    uint32 traveltime = uint32(pointPath.GetTotalLength()/float(speed));
    SendMonsterMoveByPath(pointPath, 1, pointPath.size(), traveltime);
}

// From MaNGOS
bool Unit::CanReachWithMeleeAttack(Unit* pVictim, float flat_mod /*= 0.0f*/) const
{
    if (!pVictim)
        return false;

    // The measured values show BASE_MELEE_OFFSET in (1.3224, 1.342)
    float reach = GetFloatValue(UNIT_FIELD_COMBATREACH) + pVictim->GetFloatValue(UNIT_FIELD_COMBATREACH) +
        1.33f + flat_mod;

    if (reach < ATTACK_DISTANCE)
        reach = ATTACK_DISTANCE;

    // This check is not related to bounding radius
    float dx = GetPositionX() - pVictim->GetPositionX();
    float dy = GetPositionY() - pVictim->GetPositionY();
    float dz = GetPositionZ() - pVictim->GetPositionZ();

    return dx*dx + dy*dy + dz*dz < reach*reach;
}

bool Unit::IsCCed() const
{
    return (IsAlive() && (isFeared() || isCharmed() || HasUnitState(UNIT_STAT_STUNNED) || HasUnitState(UNIT_STAT_CONFUSED)));
}

////////////////////////////////////////////////////////////
// Methods of class GlobalCooldownMgr

bool GlobalCooldownMgr::HasGlobalCooldown(SpellEntry const* spellInfo) const
{
    GlobalCooldownList::const_iterator itr = m_GlobalCooldowns.find(spellInfo->StartRecoveryCategory);
    return itr != m_GlobalCooldowns.end() && itr->second.duration && GetMSTimeDiff(itr->second.cast_time, getMSTime()) < itr->second.duration;
}

void GlobalCooldownMgr::AddGlobalCooldown(SpellEntry const* spellInfo, uint32 gcd)
{
    m_GlobalCooldowns[spellInfo->StartRecoveryCategory] = GlobalCooldown(gcd, getMSTime());
}

void GlobalCooldownMgr::CancelGlobalCooldown(SpellEntry const* spellInfo)
{
    m_GlobalCooldowns[spellInfo->StartRecoveryCategory].duration = 0;
}

bool Unit::HasAuraWithMechanic(Mechanics mechanic) const
{
    AuraMap const &auras = GetAuras();
    for(AuraMap::const_iterator itr = auras.begin(); itr != auras.end(); ++itr)
        if(SpellEntry const *iterSpellProto = itr->second->GetSpellProto())
            if(iterSpellProto->Mechanic == mechanic)
                return true;
    return false;
}

void Unit::RestoreDisplayId()
{
    Aura* handledAura = NULL;
    // try to receive model from transform auras
    Unit::AuraList const& transforms = GetAurasByType(SPELL_AURA_TRANSFORM);
    if (!transforms.empty())
    {
        // iterate over already applied transform auras - from newest to oldest
        for (Unit::AuraList::const_reverse_iterator i = transforms.rbegin(); i != transforms.rend(); ++i)
        {
            if (!handledAura)
                handledAura = (*i);
            // prefer negative auras
            if(!IsPositiveSpell((*i)->GetSpellProto()->Id))
            {
                handledAura = (*i);
                break;
            }
        }
    }
    
    // transform aura was found
    if (handledAura)
        handledAura->ApplyModifier(true);
    // we've found shapeshift
    else if (uint32 modelId = GetModelForForm(GetShapeshiftForm()))
        SetDisplayId(modelId);
    // no auras found - set modelid to default
    else
    {
        SetDisplayId(GetNativeDisplayId());
        setTransForm(0);
    }
}

uint32 Unit::GetModelForForm(ShapeshiftForm form) const
{
    //set different model the first april
    time_t t = time(NULL);
	tm* timePtr = localtime(&t);
    bool firstApril = timePtr->tm_mon == 3 && timePtr->tm_mday == 1;

    uint32 modelid = 0;
    switch(form)
    {
        case FORM_CAT:
            if(Player::TeamForRace(getRace()) == ALLIANCE)
                modelid = firstApril ? 729 : 892;
            else
                modelid = firstApril ? 657 : 8571;
            break;
        case FORM_TRAVEL:
            if(firstApril)
                modelid = getGender() == GENDER_FEMALE ? (rand()%2 ? 1547 : 18406) : 1917;
            else
                modelid = 632;
            break;
        case FORM_AQUA:
            modelid = firstApril ? 4591 : 2428;
            break;
        case FORM_GHOUL:
            if(Player::TeamForRace(getRace()) == ALLIANCE)
                modelid = 10045;
            break;
        case FORM_BEAR:
        case FORM_DIREBEAR:
            if(Player::TeamForRace(getRace()) == ALLIANCE)
                modelid = firstApril ? 865 : 2281;
            else
                modelid = firstApril ? 706 : 2289;
            break;
        case FORM_CREATUREBEAR:
            modelid = 902;
            break;
        case FORM_GHOSTWOLF:
            modelid = firstApril ? 1531 : 4613;
            break;
        case FORM_FLIGHT:
            if(Player::TeamForRace(getRace()) == ALLIANCE)
                modelid = firstApril ? 9345 : 20857;
            else
                modelid = firstApril ? 9345 : 20872;
            break;
        case FORM_MOONKIN:
            if(Player::TeamForRace(getRace()) == ALLIANCE)
                modelid = firstApril ? 17034 : 15374;
            else
                modelid = firstApril ? 17034 : 15375;
            break;
        case FORM_FLIGHT_EPIC:
            if(Player::TeamForRace(getRace()) == ALLIANCE)
                modelid = firstApril ? 6212 : 21243;
            else
                modelid = firstApril ? 19259 : 21244;
            break;
        case FORM_TREE:
            modelid = firstApril ? ( getGender() == GENDER_FEMALE ? 17340 : 2432) : 864;
            break;
        case FORM_SPIRITOFREDEMPTION:
            modelid = 16031;
            break;
        case FORM_AMBIENT:
        case FORM_SHADOW:
        case FORM_STEALTH:
        case FORM_BATTLESTANCE:
        case FORM_BERSERKERSTANCE:
        case FORM_DEFENSIVESTANCE:
        case FORM_NONE:
            break;
        default:
            sLog.outError("Unit::GetModelForForm : Unknown Shapeshift Type: %u", form);
    }

    return modelid;
}

bool Unit::isSpellDisabled(uint32 const spellId)
{
    if(GetTypeId() == TYPEID_PLAYER)
    {
        if(objmgr.IsPlayerSpellDisabled(spellId))
            return true;
    }
    else if (GetTypeId() == TYPEID_UNIT && (ToCreature())->IsPet())
    {
        if(objmgr.IsPetSpellDisabled(spellId))
            return true;
    }
    else
    {
        if(objmgr.IsCreatureSpellDisabled(spellId))
            return true;
    }
    return false;
}

void Unit::HandleParryRush()
{
    if(ToCreature() && ToCreature()->GetCreatureInfo()->flags_extra & CREATURE_FLAG_EXTRA_NO_PARRY_RUSH)
        return;

    uint32 timeLeft = getAttackTimer(BASE_ATTACK);
    uint32 attackTime = GetAttackTime(BASE_ATTACK);
    float percentTimeLeft = timeLeft / (float)attackTime;

    int newAttackTime = timeLeft - (int)(0.4*attackTime);
    float newPercentTimeLeft = newAttackTime / (float)attackTime;
    if(newPercentTimeLeft < 0.2)
        setAttackTimer(BASE_ATTACK, (uint32)(0.2*attackTime) ); //20% floor
    else
        setAttackTimer(BASE_ATTACK, (int)newAttackTime );
}

bool Unit::SetDisableGravity(bool disable)
{
    if (disable)
        AddUnitMovementFlag(MOVEMENTFLAG_LEVITATING);
    else
        RemoveUnitMovementFlag(MOVEMENTFLAG_LEVITATING);

    return true;
}